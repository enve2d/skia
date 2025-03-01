/*
 * Copyright 2019 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkColorFilter.h"
#include "include/core/SkData.h"
#include "include/effects/SkRuntimeEffect.h"
#include "include/private/SkChecksum.h"
#include "include/private/SkMutex.h"
#include "src/core/SkRasterPipeline.h"
#include "src/core/SkReadBuffer.h"
#include "src/core/SkUtils.h"
#include "src/core/SkVM.h"
#include "src/core/SkWriteBuffer.h"
#include "src/sksl/SkSLByteCode.h"
#include "src/sksl/SkSLCompiler.h"
#include "src/sksl/ir/SkSLFunctionDefinition.h"
#include "src/sksl/ir/SkSLVarDeclarations.h"

#if SK_SUPPORT_GPU
#include "include/private/GrRecordingContext.h"
#include "src/gpu/GrColorInfo.h"
#include "src/gpu/GrFPArgs.h"
#include "src/gpu/effects/GrSkSLFP.h"
#endif

namespace SkSL {
class SharedCompiler {
public:
    SharedCompiler() : fLock(compiler_mutex()) {
        if (!gCompiler) {
            gCompiler = new SkSL::Compiler{};
        }
    }

    SkSL::Compiler* operator->() const { return gCompiler; }

private:
    SkAutoMutexExclusive fLock;

    static SkMutex& compiler_mutex() {
        static SkMutex& mutex = *(new SkMutex);
        return mutex;
    }

    static SkSL::Compiler* gCompiler;
};
SkSL::Compiler* SharedCompiler::gCompiler = nullptr;
}

SkRuntimeEffect::EffectResult SkRuntimeEffect::Make(SkString sksl) {
    SkSL::SharedCompiler compiler;
    auto program = compiler->convertProgram(SkSL::Program::kPipelineStage_Kind,
                                            SkSL::String(sksl.c_str(), sksl.size()),
                                            SkSL::Program::Settings());
    // TODO: Many errors aren't caught until we process the generated Program here. Catching those
    // in the IR generator would provide better errors messages (with locations).
    #define RETURN_FAILURE(...) return std::make_pair(nullptr, SkStringPrintf(__VA_ARGS__))

    if (!program) {
        RETURN_FAILURE("%s", compiler->errorText().c_str());
    }
    SkASSERT(!compiler->errorCount());

    size_t offset = 0, uniformSize = 0;
    std::vector<Variable> inAndUniformVars;
    std::vector<SkString> children;
    std::vector<Varying> varyings;
    const SkSL::Context& ctx(compiler->context());

    // Scrape the varyings
    for (const auto& e : *program) {
        if (e.fKind == SkSL::ProgramElement::kVar_Kind) {
            SkSL::VarDeclarations& v = (SkSL::VarDeclarations&) e;
            for (const auto& varStatement : v.fVars) {
                const SkSL::Variable& var = *((SkSL::VarDeclaration&) *varStatement).fVar;

                if (var.fModifiers.fFlags & SkSL::Modifiers::kVarying_Flag) {
                    varyings.push_back({var.fName, var.fType.kind() == SkSL::Type::kVector_Kind
                                                           ? var.fType.columns()
                                                           : 1});
                }
            }
        }
    }

    // Gather the inputs in two passes, to de-interleave them in our input layout.
    // We put the uniforms *first*, so that the CPU backend can alias the combined input block as
    // the uniform block when calling the interpreter.
    for (auto flag : { SkSL::Modifiers::kUniform_Flag, SkSL::Modifiers::kIn_Flag }) {
        if (flag == SkSL::Modifiers::kIn_Flag) {
            uniformSize = offset;
        }
        for (const auto& e : *program) {
            if (e.fKind == SkSL::ProgramElement::kVar_Kind) {
                SkSL::VarDeclarations& v = (SkSL::VarDeclarations&) e;
                for (const auto& varStatement : v.fVars) {
                    const SkSL::Variable& var = *((SkSL::VarDeclaration&) *varStatement).fVar;

                    // Sanity check some rules that should be enforced by the IR generator.
                    // These are all layout options that only make sense in .fp files.
                    SkASSERT(!var.fModifiers.fLayout.fKey);
                    SkASSERT((var.fModifiers.fFlags & SkSL::Modifiers::kIn_Flag) == 0 ||
                        (var.fModifiers.fFlags & SkSL::Modifiers::kUniform_Flag) == 0);
                    SkASSERT(var.fModifiers.fLayout.fCType == SkSL::Layout::CType::kDefault);
                    SkASSERT(var.fModifiers.fLayout.fWhen.fLength == 0);
                    SkASSERT((var.fModifiers.fLayout.fFlags & SkSL::Layout::kTracked_Flag) == 0);

                    if (var.fModifiers.fFlags & flag) {
                        if (&var.fType == ctx.fFragmentProcessor_Type.get()) {
                            children.push_back(var.fName);
                            continue;
                        }

                        Variable v;
                        v.fName = var.fName;
                        v.fQualifier = (var.fModifiers.fFlags & SkSL::Modifiers::kUniform_Flag)
                                ? Variable::Qualifier::kUniform
                                : Variable::Qualifier::kIn;
                        v.fFlags = 0;
                        v.fCount = 1;

                        const SkSL::Type* type = &var.fType;
                        if (type->kind() == SkSL::Type::kArray_Kind) {
                            v.fFlags |= Variable::kArray_Flag;
                            v.fCount = type->columns();
                            type = &type->componentType();
                        }

#if SK_SUPPORT_GPU
#define SET_TYPES(cpuType, gpuType) do { v.fType = cpuType; v.fGPUType = gpuType;} while (false)
#else
#define SET_TYPES(cpuType, gpuType) do { v.fType = cpuType; } while (false)
#endif

                        if (type == ctx.fBool_Type.get()) {
                            SET_TYPES(Variable::Type::kBool, kVoid_GrSLType);
                        } else if (type == ctx.fInt_Type.get()) {
                            SET_TYPES(Variable::Type::kInt, kVoid_GrSLType);
                        } else if (type == ctx.fFloat_Type.get()) {
                            SET_TYPES(Variable::Type::kFloat, kFloat_GrSLType);
                        } else if (type == ctx.fHalf_Type.get()) {
                            SET_TYPES(Variable::Type::kFloat, kHalf_GrSLType);
                        } else if (type == ctx.fFloat2_Type.get()) {
                            SET_TYPES(Variable::Type::kFloat2, kFloat2_GrSLType);
                        } else if (type == ctx.fHalf2_Type.get()) {
                            SET_TYPES(Variable::Type::kFloat2, kHalf2_GrSLType);
                        } else if (type == ctx.fFloat3_Type.get()) {
                            SET_TYPES(Variable::Type::kFloat3, kFloat3_GrSLType);
                        } else if (type == ctx.fHalf3_Type.get()) {
                            SET_TYPES(Variable::Type::kFloat3, kHalf3_GrSLType);
                        } else if (type == ctx.fFloat4_Type.get()) {
                            SET_TYPES(Variable::Type::kFloat4, kFloat4_GrSLType);
                        } else if (type == ctx.fHalf4_Type.get()) {
                            SET_TYPES(Variable::Type::kFloat4, kHalf4_GrSLType);
                        } else if (type == ctx.fFloat2x2_Type.get()) {
                            SET_TYPES(Variable::Type::kFloat2x2, kFloat2x2_GrSLType);
                        } else if (type == ctx.fHalf2x2_Type.get()) {
                            SET_TYPES(Variable::Type::kFloat2x2, kHalf2x2_GrSLType);
                        } else if (type == ctx.fFloat3x3_Type.get()) {
                            SET_TYPES(Variable::Type::kFloat3x3, kFloat3x3_GrSLType);
                        } else if (type == ctx.fHalf3x3_Type.get()) {
                            SET_TYPES(Variable::Type::kFloat3x3, kHalf3x3_GrSLType);
                        } else if (type == ctx.fFloat4x4_Type.get()) {
                            SET_TYPES(Variable::Type::kFloat4x4, kFloat4x4_GrSLType);
                        } else if (type == ctx.fHalf4x4_Type.get()) {
                            SET_TYPES(Variable::Type::kFloat4x4, kHalf4x4_GrSLType);
                        } else {
                            RETURN_FAILURE("Invalid input/uniform type: '%s'",
                                           type->displayName().c_str());
                        }

#undef SET_TYPES

                        switch (v.fType) {
                            case Variable::Type::kBool:
                            case Variable::Type::kInt:
                                if (v.fQualifier == Variable::Qualifier::kUniform) {
                                    RETURN_FAILURE("'uniform' variables may not have '%s' type",
                                                   type->displayName().c_str());
                                }
                                break;

                            case Variable::Type::kFloat:
                                // Floats can be 'in' or 'uniform'
                                break;

                            case Variable::Type::kFloat2:
                            case Variable::Type::kFloat3:
                            case Variable::Type::kFloat4:
                            case Variable::Type::kFloat2x2:
                            case Variable::Type::kFloat3x3:
                            case Variable::Type::kFloat4x4:
                                if (v.fQualifier == Variable::Qualifier::kIn) {
                                    RETURN_FAILURE("'in' variables may not have '%s' type",
                                                   type->displayName().c_str());
                                }
                                break;
                        }

                        if (v.fType != Variable::Type::kBool) {
                            offset = SkAlign4(offset);
                        }
                        v.fOffset = offset;
                        offset += v.sizeInBytes();
                        inAndUniformVars.push_back(v);
                    }
                }
            }
        }
    }

#undef RETURN_FAILURE

    sk_sp<SkRuntimeEffect> effect(new SkRuntimeEffect(std::move(sksl),
                                                      std::move(program),
                                                      std::move(inAndUniformVars),
                                                      std::move(children),
                                                      std::move(varyings),
                                                      uniformSize));
    return std::make_pair(std::move(effect), SkString());
}

size_t SkRuntimeEffect::Variable::sizeInBytes() const {
    auto element_size = [](Type type) -> size_t {
        switch (type) {
            case Type::kBool:   return 1;
            case Type::kInt:    return sizeof(int32_t);
            case Type::kFloat:  return sizeof(float);
            case Type::kFloat2: return sizeof(float) * 2;
            case Type::kFloat3: return sizeof(float) * 3;
            case Type::kFloat4: return sizeof(float) * 4;

            case Type::kFloat2x2: return sizeof(float) * 4;
            case Type::kFloat3x3: return sizeof(float) * 9;
            case Type::kFloat4x4: return sizeof(float) * 16;
            default: SkUNREACHABLE;
        }
    };
    return element_size(fType) * fCount;
}

SkRuntimeEffect::SkRuntimeEffect(SkString sksl,
                                 std::unique_ptr<SkSL::Program> baseProgram,
                                 std::vector<Variable>&& inAndUniformVars,
                                 std::vector<SkString>&& children,
                                 std::vector<Varying>&& varyings,
                                 size_t uniformSize)
        : fHash(SkGoodHash()(sksl))
        , fSkSL(std::move(sksl))
        , fBaseProgram(std::move(baseProgram))
        , fInAndUniformVars(std::move(inAndUniformVars))
        , fChildren(std::move(children))
        , fVaryings(std::move(varyings))
        , fUniformSize(uniformSize) {
    SkASSERT(fBaseProgram);
    SkASSERT(SkIsAlign4(fUniformSize));
    SkASSERT(fUniformSize <= this->inputSize());
}

SkRuntimeEffect::~SkRuntimeEffect() = default;

size_t SkRuntimeEffect::inputSize() const {
    return fInAndUniformVars.empty() ? 0
                                     : SkAlign4(fInAndUniformVars.back().fOffset +
                                                fInAndUniformVars.back().sizeInBytes());
}

SkRuntimeEffect::SpecializeResult
SkRuntimeEffect::specialize(SkSL::Program& baseProgram,
                            const void* inputs,
                            const SkSL::SharedCompiler& compiler) const {
    std::unordered_map<SkSL::String, SkSL::Program::Settings::Value> inputMap;
    for (const auto& v : fInAndUniformVars) {
        if (v.fQualifier != Variable::Qualifier::kIn) {
            continue;
        }
        // 'in' arrays are not supported
        SkASSERT(!v.isArray());
        SkSL::String name(v.fName.c_str(), v.fName.size());
        switch (v.fType) {
            case Variable::Type::kBool: {
                bool b = *SkTAddOffset<const bool>(inputs, v.fOffset);
                inputMap.insert(std::make_pair(name, SkSL::Program::Settings::Value(b)));
                break;
            }
            case Variable::Type::kInt: {
                int32_t i = *SkTAddOffset<const int32_t>(inputs, v.fOffset);
                inputMap.insert(std::make_pair(name, SkSL::Program::Settings::Value(i)));
                break;
            }
            case Variable::Type::kFloat: {
                float f = *SkTAddOffset<const float>(inputs, v.fOffset);
                inputMap.insert(std::make_pair(name, SkSL::Program::Settings::Value(f)));
                break;
            }
            default:
                SkDEBUGFAIL("Unsupported input variable type");
                return SpecializeResult{nullptr, SkString("Unsupported input variable type")};
        }
    }

    auto specialized = compiler->specialize(baseProgram, inputMap);
    bool optimized = compiler->optimize(*specialized);
    if (!optimized) {
        return SpecializeResult{nullptr, SkString(compiler->errorText().c_str())};
    }
    return SpecializeResult{std::move(specialized), SkString()};
}

#if SK_SUPPORT_GPU
bool SkRuntimeEffect::toPipelineStage(const void* inputs, const GrShaderCaps* shaderCaps,
                                      GrContextOptions::ShaderErrorHandler* errorHandler,
                                      SkSL::PipelineStageArgs* outArgs) {
    SkSL::SharedCompiler compiler;

    // This function is used by the GPU backend, and can't reuse our previously built fBaseProgram.
    // If the supplied shaderCaps have any non-default values, we have baked in the wrong settings.
    SkSL::Program::Settings settings;
    settings.fCaps = shaderCaps;

    auto baseProgram = compiler->convertProgram(SkSL::Program::kPipelineStage_Kind,
                                                SkSL::String(fSkSL.c_str(), fSkSL.size()),
                                                settings);
    if (!baseProgram) {
        errorHandler->compileError(fSkSL.c_str(), compiler->errorText().c_str());
        return false;
    }

    auto [specialized, errorText] = this->specialize(*baseProgram, inputs, compiler);
    if (!specialized) {
        errorHandler->compileError(fSkSL.c_str(), errorText.c_str());
        return false;
    }

    if (!compiler->toPipelineStage(*specialized, outArgs)) {
        errorHandler->compileError(fSkSL.c_str(), compiler->errorText().c_str());
        return false;
    }

    return true;
}
#endif

SkRuntimeEffect::ByteCodeResult SkRuntimeEffect::toByteCode(const void* inputs) const {
    SkSL::SharedCompiler compiler;

    auto [specialized, errorText] = this->specialize(*fBaseProgram, inputs, compiler);
    if (!specialized) {
        return ByteCodeResult{nullptr, errorText};
    }
    auto byteCode = compiler->toByteCode(*specialized);
    return ByteCodeResult(std::move(byteCode), SkString(compiler->errorText().c_str()));
}

///////////////////////////////////////////////////////////////////////////////////////////////////

static std::vector<skvm::F32> program_fn(skvm::Builder* p,
                                         const SkSL::ByteCodeFunction& fn,
                                         const std::vector<skvm::F32>& uniform,
                                         std::vector<skvm::F32> stack) {
    auto push = [&](skvm::F32 x) { stack.push_back(x); };
    auto pop  = [&]{ skvm::F32 x = stack.back(); stack.pop_back(); return x; };

    for (const uint8_t *ip = fn.code(), *end = ip + fn.size(); ip != end; ) {
        using Inst = SkSL::ByteCodeInstruction;

        auto inst = (Inst)(uintptr_t)sk_unaligned_load<SkSL::instruction>(ip);
        ip += sizeof(SkSL::instruction);

        auto u8  = [&]{ auto x = sk_unaligned_load<uint8_t >(ip); ip += sizeof(x); return x; };
      //auto u16 = [&]{ auto x = sk_unaligned_load<uint16_t>(ip); ip += sizeof(x); return x; };
        auto u32 = [&]{ auto x = sk_unaligned_load<uint32_t>(ip); ip += sizeof(x); return x; };

        switch (inst) {
            default:
                #if 0
                    fn.disassemble();
                    SkDebugf("inst %04x unimplemented\n", inst);
                    __builtin_debugtrap();
                #endif
                return {};

            case Inst::kLoad: {
                SkAssertResult(u8() == 1);
                int ix = u8();
                push(stack[ix + 0]);
            } break;

            case Inst::kLoad2: {
                SkAssertResult(u8() == 2);
                int ix = u8();
                push(stack[ix + 0]);
                push(stack[ix + 1]);
            } break;

            case Inst::kPushImmediate: {
                push(bit_cast(p->splat(u32())));
            } break;

            case Inst::kDup: {
                int off = u8();
                push(stack[stack.size() - off]);
            } break;

            case Inst::kAddF: {
                SkAssertResult(u8() == 1);
                skvm::F32 x = pop(),
                          a = pop();
                push(x+a);
            } break;

            case Inst::kMultiplyF: {
                SkAssertResult(u8() == 1);
                skvm::F32 x = pop(),
                          a = pop();
                push(x*a);
            } break;

            case Inst::kMultiplyF2: {
                SkAssertResult(u8() == 2);
                skvm::F32 x = pop(), y = pop(),
                          a = pop(), b = pop();
                push(y*b);
                push(x*a);
            } break;

            case Inst::kLoadUniform: {
                SkAssertResult(u8() == 1);
                int ix = u8();
                push(uniform[ix]);
            } break;

            case Inst::kStore: {
                int ix = u8();
                stack[ix + 0] = pop();
            } break;

            case Inst::kStore2: {
                int ix = u8();
                stack[ix + 1] = pop();
                stack[ix + 0] = pop();
            } break;

            case Inst::kStore4: {
                int ix = u8();
                stack[ix + 3] = pop();
                stack[ix + 2] = pop();
                stack[ix + 1] = pop();
                stack[ix + 0] = pop();
            } break;

            case Inst::kReturn: {
                SkAssertResult(u8() == 0);
                SkASSERT(ip == end);
            } break;
        }
    }
    return stack;
}


class SkRuntimeColorFilter : public SkColorFilter {
public:
    SkRuntimeColorFilter(sk_sp<SkRuntimeEffect> effect, sk_sp<SkData> inputs,
                         sk_sp<SkColorFilter> children[], size_t childCount)
            : fEffect(std::move(effect))
            , fInputs(std::move(inputs))
            , fChildren(children, children + childCount) {}

#if SK_SUPPORT_GPU
    std::unique_ptr<GrFragmentProcessor> asFragmentProcessor(
            GrRecordingContext* context, const GrColorInfo& colorInfo) const override {
        auto fp = GrSkSLFP::Make(context, fEffect, "Runtime Color Filter", fInputs);
        for (const auto& child : fChildren) {
            auto childFP = child ? child->asFragmentProcessor(context, colorInfo) : nullptr;
            if (!childFP) {
                // TODO: This is the case that should eventually mean "the original input color"
                return nullptr;
            }
            fp->addChild(std::move(childFP));
        }
        return std::move(fp);
    }
#endif

    const SkSL::ByteCode* byteCode() const {
        SkAutoMutexExclusive ama(fByteCodeMutex);
        if (!fByteCode) {
            auto [byteCode, errorText] = fEffect->toByteCode(fInputs->data());
            if (!byteCode) {
                SkDebugf("%s\n", errorText.c_str());
                return nullptr;
            }
            fByteCode = std::move(byteCode);
        }
        return fByteCode.get();
    }

    bool onAppendStages(const SkStageRec& rec, bool shaderIsOpaque) const override {
        auto ctx = rec.fAlloc->make<SkRasterPipeline_InterpreterCtx>();
        // don't need to set ctx->paintColor
        ctx->inputs = fInputs->data();
        ctx->ninputs = fEffect->uniformSize() / 4;
        ctx->shaderConvention = false;

        ctx->byteCode = this->byteCode();
        if (!ctx->byteCode) {
            return false;
        }

        ctx->fn = ctx->byteCode->getFunction("main");
        rec.fPipeline->append(SkRasterPipeline::interpreter, ctx);
        return true;
    }

    skvm::Color onProgram(skvm::Builder* p, skvm::Color c,
                          SkColorSpace* /*dstCS*/,
                          skvm::Uniforms* uniforms, SkArenaAlloc*) const override {
        const SkSL::ByteCode* bc = this->byteCode();
        if (!bc) {
            return {};
        }

        const SkSL::ByteCodeFunction* fn = bc->getFunction("main");
        if (!fn) {
            return {};
        }

        std::vector<skvm::F32> uniform;
        for (int i = 0; i < (int)fEffect->uniformSize() / 4; i++) {
            float f;
            memcpy(&f, (const char*)fInputs->data() + 4*i, 4);
            uniform.push_back(p->uniformF(uniforms->pushF(f)));
        }

        std::vector<skvm::F32> stack =
            program_fn(p, *fn, uniform, {c.r, c.g, c.b, c.a});

        if (stack.size() == 4) {
            return {stack[0], stack[1], stack[2], stack[3]};
        }
        return {};
    }

    void flatten(SkWriteBuffer& buffer) const override {
        buffer.writeString(fEffect->source().c_str());
        if (fInputs) {
            buffer.writeDataAsByteArray(fInputs.get());
        } else {
            buffer.writeByteArray(nullptr, 0);
        }
        buffer.write32(fChildren.size());
        for (const auto& child : fChildren) {
            buffer.writeFlattenable(child.get());
        }
    }

    SK_FLATTENABLE_HOOKS(SkRuntimeColorFilter)

private:
    sk_sp<SkRuntimeEffect> fEffect;
    sk_sp<SkData> fInputs;
    std::vector<sk_sp<SkColorFilter>> fChildren;

    mutable SkMutex fByteCodeMutex;
    mutable std::unique_ptr<SkSL::ByteCode> fByteCode;
};

sk_sp<SkFlattenable> SkRuntimeColorFilter::CreateProc(SkReadBuffer& buffer) {
    SkString sksl;
    buffer.readString(&sksl);
    sk_sp<SkData> inputs = buffer.readByteArrayAsData();

    auto effect = std::get<0>(SkRuntimeEffect::Make(std::move(sksl)));
    if (!effect) {
        buffer.validate(false);
        return nullptr;
    }

    size_t childCount = buffer.read32();
    if (childCount != effect->children().count()) {
        buffer.validate(false);
        return nullptr;
    }

    std::vector<sk_sp<SkColorFilter>> children;
    children.resize(childCount);
    for (size_t i = 0; i < children.size(); ++i) {
        children[i] = buffer.readColorFilter();
    }

    return effect->makeColorFilter(std::move(inputs), children.data(), children.size());
}

///////////////////////////////////////////////////////////////////////////////////////////////////

class SkRTShader : public SkShaderBase {
public:
    SkRTShader(sk_sp<SkRuntimeEffect> effect, sk_sp<SkData> inputs, const SkMatrix* localMatrix,
               sk_sp<SkShader>* children, size_t childCount, bool isOpaque)
            : SkShaderBase(localMatrix)
            , fEffect(std::move(effect))
            , fIsOpaque(isOpaque)
            , fInputs(std::move(inputs))
            , fChildren(children, children + childCount) {}

    bool isOpaque() const override { return fIsOpaque; }

#if SK_SUPPORT_GPU
    std::unique_ptr<GrFragmentProcessor> asFragmentProcessor(const GrFPArgs& args) const override {
        SkMatrix matrix;
        if (!this->totalLocalMatrix(args.fPreLocalMatrix)->invert(&matrix)) {
            return nullptr;
        }
        auto fp = GrSkSLFP::Make(args.fContext, fEffect, "runtime_shader", fInputs, &matrix);
        for (const auto& child : fChildren) {
            auto childFP = child ? as_SB(child)->asFragmentProcessor(args) : nullptr;
            if (!childFP) {
                // TODO: This is the case that should eventually mean "the original input color"
                return nullptr;
            }
            fp->addChild(std::move(childFP));
        }
        if (GrColorTypeClampType(args.fDstColorInfo->colorType()) != GrClampType::kNone) {
            return GrFragmentProcessor::ClampPremulOutput(std::move(fp));
        } else {
            return std::move(fp);
        }
    }
#endif

    const SkSL::ByteCode* byteCode() const {
        SkAutoMutexExclusive ama(fByteCodeMutex);
        if (!fByteCode) {
            auto [byteCode, errorText] = fEffect->toByteCode(fInputs->data());
            if (!byteCode) {
                SkDebugf("%s\n", errorText.c_str());
                return nullptr;
            }
            fByteCode = std::move(byteCode);
        }
        return fByteCode.get();
    }

    bool onAppendStages(const SkStageRec& rec) const override {
        SkMatrix inverse;
        if (!this->computeTotalInverse(rec.fCTM, rec.fLocalM, &inverse)) {
            return false;
        }

        auto ctx = rec.fAlloc->make<SkRasterPipeline_InterpreterCtx>();
        ctx->paintColor = rec.fPaint.getColor4f();
        ctx->inputs = fInputs->data();
        ctx->ninputs = fEffect->uniformSize() / 4;
        ctx->shaderConvention = true;

        ctx->byteCode = this->byteCode();
        if (!ctx->byteCode) {
            return false;
        }
        ctx->fn = ctx->byteCode->getFunction("main");
        rec.fPipeline->append(SkRasterPipeline::seed_shader);
        rec.fPipeline->append_matrix(rec.fAlloc, inverse);
        rec.fPipeline->append(SkRasterPipeline::interpreter, ctx);
        return true;
    }

    skvm::Color onProgram(skvm::Builder* p, skvm::F32 x, skvm::F32 y, skvm::Color paint,
                          const SkMatrix& ctm, const SkMatrix* localM,
                          SkFilterQuality, const SkColorInfo& /*dst*/,
                          skvm::Uniforms* uniforms, SkArenaAlloc*) const override {
        const SkSL::ByteCode* bc = this->byteCode();
        if (!bc) {
            return {};
        }

        const SkSL::ByteCodeFunction* fn = bc->getFunction("main");
        if (!fn) {
            return {};
        }

        std::vector<skvm::F32> uniform;
        for (int i = 0; i < (int)fEffect->uniformSize() / 4; i++) {
            float f;
            memcpy(&f, (const char*)fInputs->data() + 4*i, 4);
            uniform.push_back(p->uniformF(uniforms->pushF(f)));
        }

        SkMatrix inv;
        if (!this->computeTotalInverse(ctm, localM, &inv)) {
            return {};
        }
        SkShaderBase::ApplyMatrix(p,inv, &x,&y,uniforms);

        std::vector<skvm::F32> stack =
            program_fn(p, *fn, uniform, {x,y, paint.r, paint.g, paint.b, paint.a});

        if (stack.size() == 6) {
            return {stack[2], stack[3], stack[4], stack[5]};
        }
        return {};
    }

    void flatten(SkWriteBuffer& buffer) const override {
        uint32_t flags = 0;
        if (fIsOpaque) {
            flags |= kIsOpaque_Flag;
        }
        if (!this->getLocalMatrix().isIdentity()) {
            flags |= kHasLocalMatrix_Flag;
        }

        buffer.writeString(fEffect->source().c_str());
        if (fInputs) {
            buffer.writeDataAsByteArray(fInputs.get());
        } else {
            buffer.writeByteArray(nullptr, 0);
        }
        buffer.write32(flags);
        if (flags & kHasLocalMatrix_Flag) {
            buffer.writeMatrix(this->getLocalMatrix());
        }
        buffer.write32(fChildren.size());
        for (const auto& child : fChildren) {
            buffer.writeFlattenable(child.get());
        }
    }

    SkRuntimeEffect* asRuntimeEffect() const override { return fEffect.get(); }

    SK_FLATTENABLE_HOOKS(SkRTShader)

private:
    enum Flags {
        kIsOpaque_Flag          = 1 << 0,
        kHasLocalMatrix_Flag    = 1 << 1,
    };

    sk_sp<SkRuntimeEffect> fEffect;
    bool fIsOpaque;

    sk_sp<SkData> fInputs;
    std::vector<sk_sp<SkShader>> fChildren;

    mutable SkMutex fByteCodeMutex;
    mutable std::unique_ptr<SkSL::ByteCode> fByteCode;
};

sk_sp<SkFlattenable> SkRTShader::CreateProc(SkReadBuffer& buffer) {
    SkString sksl;
    buffer.readString(&sksl);
    sk_sp<SkData> inputs = buffer.readByteArrayAsData();
    uint32_t flags = buffer.read32();

    bool isOpaque = SkToBool(flags & kIsOpaque_Flag);
    SkMatrix localM, *localMPtr = nullptr;
    if (flags & kHasLocalMatrix_Flag) {
        buffer.readMatrix(&localM);
        localMPtr = &localM;
    }

    auto effect = std::get<0>(SkRuntimeEffect::Make(std::move(sksl)));
    if (!effect) {
        buffer.validate(false);
        return nullptr;
    }

    size_t childCount = buffer.read32();
    if (childCount != effect->children().count()) {
        buffer.validate(false);
        return nullptr;
    }

    std::vector<sk_sp<SkShader>> children;
    children.resize(childCount);
    for (size_t i = 0; i < children.size(); ++i) {
        children[i] = buffer.readShader();
    }

    return effect->makeShader(std::move(inputs), children.data(), children.size(), localMPtr,
                              isOpaque);
}

///////////////////////////////////////////////////////////////////////////////////////////////////

sk_sp<SkShader> SkRuntimeEffect::makeShader(sk_sp<SkData> inputs,
                                            sk_sp<SkShader> children[], size_t childCount,
                                            const SkMatrix* localMatrix, bool isOpaque) {
    if (!inputs) {
        inputs = SkData::MakeEmpty();
    }
    return inputs->size() == this->inputSize() && childCount == fChildren.size()
        ? sk_sp<SkShader>(new SkRTShader(sk_ref_sp(this), std::move(inputs), localMatrix,
                                         children, childCount, isOpaque))
        : nullptr;
}

sk_sp<SkColorFilter> SkRuntimeEffect::makeColorFilter(sk_sp<SkData> inputs,
                                                      sk_sp<SkColorFilter> children[],
                                                      size_t childCount) {
    if (!inputs) {
        inputs = SkData::MakeEmpty();
    }
    return inputs && inputs->size() == this->inputSize() && childCount == fChildren.size()
        ? sk_sp<SkColorFilter>(new SkRuntimeColorFilter(sk_ref_sp(this), std::move(inputs),
                                                        children, childCount))
        : nullptr;
}

sk_sp<SkColorFilter> SkRuntimeEffect::makeColorFilter(sk_sp<SkData> inputs) {
    return this->makeColorFilter(std::move(inputs), nullptr, 0);
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void SkRuntimeEffect::RegisterFlattenables() {
    SK_REGISTER_FLATTENABLE(SkRuntimeColorFilter);
    SK_REGISTER_FLATTENABLE(SkRTShader);
}
