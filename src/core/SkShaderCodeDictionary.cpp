/*
 * Copyright 2022 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/core/SkShaderCodeDictionary.h"

#include "include/private/SkSLString.h"
#include "src/core/SkOpts.h"

namespace {

void add_indent(std::string* result, int indent) {
    result->append(4*indent, ' ');
}

} // anonymous namespace


std::string SkShaderSnippet::getMangledUniformName(int uniformIndex, int mangleId) const {
    std::string result;
    result = fUniforms[uniformIndex].name() + std::string("_") + std::to_string(mangleId);
    return result;
}

// TODO: SkShaderInfo::toSkSL needs to work outside of both just graphite and metal. To do
// so we'll need to switch over to using SkSL's uniform capabilities.
#if SK_SUPPORT_GPU && defined(SK_GRAPHITE_ENABLED) && defined(SK_METAL)

#include <set>

// TODO: switch this over to using SkSL's uniform system
namespace skgpu::graphite {
std::string GetMtlUniforms(int bufferID,
                           const char* name,
                           const std::vector<SkPaintParamsKey::BlockReader>&);
std::string GetMtlTexturesAndSamplers(const std::vector<SkPaintParamsKey::BlockReader>&,
                                      int* binding);
} // namespace skgpu::graphite

// Emit the glue code needed to invoke a single static helper isolated w/in its own scope.
// The structure of this will be:
//
//     half4 outColor%d;
//     {
//         half4 child-outColor%d;  // for each child
//         {
//             /* emitted snippet sksl assigns to child-outColor%d */
//         }
//
//         /* emitted snippet sksl assigns to outColor%d - taking a vector of child var names */
//     }
// Where the %d is filled in with 'entryIndex'.
std::string SkShaderInfo::emitGlueCodeForEntry(int* entryIndex,
                                               const std::string& priorStageOutputName,
                                               std::string* result,
                                               int indent) const {
    const SkPaintParamsKey::BlockReader& reader = fBlockReaders[*entryIndex];
    int curEntryIndex = *entryIndex;

    std::string scopeOutputVar(std::string("outColor") + std::to_string(curEntryIndex));

    add_indent(result, indent);
    SkSL::String::appendf(result, "half4 %s;\n", scopeOutputVar.c_str());
    add_indent(result, indent);
    *result += "{\n";

    // Although the children appear after the parent in the shader info they are emitted
    // before the parent
    std::vector<std::string> childNames;
    for (int j = 0; j < reader.numChildren(); ++j) {
        *entryIndex += 1;
        std::string childOutputVar = this->emitGlueCodeForEntry(entryIndex, priorStageOutputName,
                                                                result, indent+1);
        childNames.push_back(childOutputVar);
    }

    *result += (reader.entry()->fGlueCodeGenerator)(scopeOutputVar, curEntryIndex, reader,
                                                    priorStageOutputName, childNames, indent+1);
    add_indent(result, indent);
    *result += "}\n";

    return scopeOutputVar;
}

// The current, incomplete, model for shader construction is:
//   each static code snippet (which can have an arbitrary signature) gets emitted once as a
//            preamble
//   glue code is then generated w/in the "main" method. The glue code is responsible for:
//            1) gathering the correct (mangled) uniforms
//            2) passing the uniforms and any other parameters to the helper method
//   The result of the last code snippet is then copied into "sk_FragColor".
// Note: that each entry's 'fStaticFunctionName' field must match the name of the method defined
// in the 'fStaticSkSL' field.
std::string SkShaderInfo::toSkSL() const {
    // The uniforms are mangled by having their index in 'fEntries' as a suffix (i.e., "_%d")
    std::string result = skgpu::graphite::GetMtlUniforms(2, "FS", fBlockReaders);

    int binding = 0;
    result += skgpu::graphite::GetMtlTexturesAndSamplers(fBlockReaders, &binding);

    std::set<const char*> emittedStaticSnippets;
    for (const auto& reader : fBlockReaders) {
        const SkShaderSnippet* e = reader.entry();
        if (emittedStaticSnippets.find(e->fStaticFunctionName) == emittedStaticSnippets.end()) {
            result += e->fStaticSkSL;
            emittedStaticSnippets.insert(e->fStaticFunctionName);
        }
    }

    result += "layout(location = 0, index = 0) out half4 sk_FragColor;\n";
    result += "void main() {\n";

    std::string lastOutputVar = "initialColor";

    // TODO: what is the correct initial color to feed in?
    add_indent(&result, 1);
    SkSL::String::appendf(&result, "half4 %s = half4(0.0, 0.0, 0.0, 0.0);",
                          lastOutputVar.c_str());

    for (int entryIndex = 0; entryIndex < (int) fBlockReaders.size(); ++entryIndex) {
        lastOutputVar = this->emitGlueCodeForEntry(&entryIndex, lastOutputVar, &result, 1);
    }

    SkSL::String::appendf(&result, "    sk_FragColor = %s;\n", lastOutputVar.c_str());
    result += "}\n";

    return result;
}
#endif

SkShaderCodeDictionary::Entry* SkShaderCodeDictionary::makeEntry(
        const SkPaintParamsKey& key
#ifdef SK_GRAPHITE_ENABLED
        , const SkPipelineDataGatherer::BlendInfo& blendInfo
#endif
        ) {
    uint8_t* newKeyData = fArena.makeArray<uint8_t>(key.sizeInBytes());
    memcpy(newKeyData, key.data(), key.sizeInBytes());

    SkSpan<const uint8_t> newKeyAsSpan = SkMakeSpan(newKeyData, key.sizeInBytes());
#ifdef SK_GRAPHITE_ENABLED
    return fArena.make([&](void *ptr) { return new(ptr) Entry(newKeyAsSpan, blendInfo); });
#else
    return fArena.make([&](void *ptr) { return new(ptr) Entry(newKeyAsSpan); });
#endif
}

size_t SkShaderCodeDictionary::Hash::operator()(const SkPaintParamsKey* key) const {
    return SkOpts::hash_fn(key->data(), key->sizeInBytes(), 0);
}

const SkShaderCodeDictionary::Entry* SkShaderCodeDictionary::findOrCreate(
        const SkPaintParamsKey& key
#ifdef SK_GRAPHITE_ENABLED
        , const SkPipelineDataGatherer::BlendInfo& blendInfo
#endif
        ) {
    SkAutoSpinlock lock{fSpinLock};

    auto iter = fHash.find(&key);
    if (iter != fHash.end()) {
        SkASSERT(fEntryVector[iter->second->uniqueID().asUInt()] == iter->second);
        return iter->second;
    }

#ifdef SK_GRAPHITE_ENABLED
    Entry* newEntry = this->makeEntry(key, blendInfo);
#else
    Entry* newEntry = this->makeEntry(key);
#endif
    newEntry->setUniqueID(fEntryVector.size());
    fHash.insert(std::make_pair(&newEntry->paintParamsKey(), newEntry));
    fEntryVector.push_back(newEntry);

    return newEntry;
}

const SkShaderCodeDictionary::Entry* SkShaderCodeDictionary::lookup(
        SkUniquePaintParamsID codeID) const {

    if (!codeID.isValid()) {
        return nullptr;
    }

    SkAutoSpinlock lock{fSpinLock};

    SkASSERT(codeID.asUInt() < fEntryVector.size());

    return fEntryVector[codeID.asUInt()];
}

SkSpan<const SkUniform> SkShaderCodeDictionary::getUniforms(SkBuiltInCodeSnippetID id) const {
    return fBuiltInCodeSnippets[(int) id].fUniforms;
}

SkSpan<const SkPaintParamsKey::DataPayloadField> SkShaderCodeDictionary::dataPayloadExpectations(
        int codeSnippetID) const {
    // All callers of this entry point should already have ensured that 'codeSnippetID' is valid
    return this->getEntry(codeSnippetID)->fDataPayloadExpectations;
}

const SkShaderSnippet* SkShaderCodeDictionary::getEntry(int codeSnippetID) const {
    SkASSERT(codeSnippetID >= 0 && codeSnippetID <= this->maxCodeSnippetID());

    if (codeSnippetID < kBuiltInCodeSnippetIDCount) {
        return &fBuiltInCodeSnippets[codeSnippetID];
    }

    int userDefinedCodeSnippetID = codeSnippetID - kBuiltInCodeSnippetIDCount;
    if (userDefinedCodeSnippetID < SkTo<int>(fUserDefinedCodeSnippets.size())) {
        return fUserDefinedCodeSnippets[userDefinedCodeSnippetID].get();
    }

    return nullptr;
}

void SkShaderCodeDictionary::getShaderInfo(SkUniquePaintParamsID uniqueID, SkShaderInfo* info) {
    auto entry = this->lookup(uniqueID);

    entry->paintParamsKey().toShaderInfo(this, info);

#ifdef SK_GRAPHITE_ENABLED
    info->setBlendInfo(entry->blendInfo());
#endif
}

//--------------------------------------------------------------------------------------------------
namespace {

using DataPayloadField = SkPaintParamsKey::DataPayloadField;

// The default glue code just calls a helper function with the signature:
//    half4 fStaticFunctionName(/* all uniforms as parameters */);
// and stores the result in a variable named "resultName".
std::string GenerateDefaultGlueCode(const std::string& resultName,
                                    int entryIndex,
                                    const SkPaintParamsKey::BlockReader& reader,
                                    const std::string& priorStageOutputName,
                                    const std::vector<std::string>& childNames,
                                    int indent) {
    SkASSERT(childNames.empty());

    std::string result;

    add_indent(&result, indent);
    SkSL::String::appendf(&result,
                          "%s = %s(", resultName.c_str(),
                          reader.entry()->fStaticFunctionName);
    for (size_t i = 0; i < reader.entry()->fUniforms.size(); ++i) {
        // The uniform names are mangled w/ the entry's index as a suffix
        result += reader.entry()->getMangledUniformName(i, entryIndex);
        if (i+1 < reader.entry()->fUniforms.size()) {
            result += ", ";
        }
    }
    result += ");\n";

    return result;
}

//--------------------------------------------------------------------------------------------------
static constexpr int kNumLinearGrad4Fields = 1;
static constexpr DataPayloadField kLinearGrad4Fields[kNumLinearGrad4Fields] = {
        { "tilemode", SkPaintParamsKey::DataPayloadType::kByte, 1 }
};

static constexpr int kFourStopGradient = 4;

// TODO: For the sprint we unify all the gradient uniforms into a standard set of 6:
//   kMaxStops colors
//   kMaxStops offsets
//   2 points
//   2 radii
static constexpr int kNumGradientUniforms = 7;
static constexpr SkUniform kGradientUniforms[kNumGradientUniforms] = {
        { "colors",  SkSLType::kFloat4, kFourStopGradient },
        { "offsets", SkSLType::kFloat,  kFourStopGradient },
        { "point0",  SkSLType::kFloat2 },
        { "point1",  SkSLType::kFloat2 },
        { "radius0", SkSLType::kFloat },
        { "radius1", SkSLType::kFloat },
        { "padding", SkSLType::kFloat2 } // TODO: add automatic uniform padding
};

static constexpr char kLinearGradient4Name[] = "sk_linear_grad_4_shader";
static constexpr char kLinearGradient4SkSL[] = "";

//--------------------------------------------------------------------------------------------------
static constexpr int kNumSolidShaderUniforms = 1;
static constexpr SkUniform kSolidShaderUniforms[kNumSolidShaderUniforms] = {
        { "color", SkSLType::kFloat4 }
};

static constexpr char kSolidShaderName[] = "sk_solid_shader";
static constexpr char kSolidShaderSkSL[] = "";

//--------------------------------------------------------------------------------------------------
static constexpr int kNumImageShaderUniforms = 5;
static constexpr SkUniform kImageShaderUniforms[kNumImageShaderUniforms] = {
        { "subset",    SkSLType::kFloat4 },
        { "tilemodeX", SkSLType::kInt },
        { "tilemodeY", SkSLType::kInt },
        { "pad1",      SkSLType::kInt }, // TODO: add automatic uniform padding
        { "pad2",      SkSLType::kInt },
};

static constexpr int kNumImageShaderTexturesAndSamplers = 1;
static constexpr SkTextureAndSampler kISTexturesAndSamplers[kNumImageShaderTexturesAndSamplers] = {
        {"sampler"},
};

static_assert(0 == static_cast<int>(SkTileMode::kClamp),  "ImageShader code depends on SkTileMode");
static_assert(1 == static_cast<int>(SkTileMode::kRepeat), "ImageShader code depends on SkTileMode");
static_assert(2 == static_cast<int>(SkTileMode::kMirror), "ImageShader code depends on SkTileMode");
static_assert(3 == static_cast<int>(SkTileMode::kDecal),  "ImageShader code depends on SkTileMode");

static constexpr char kImageShaderName[] = "sk_compute_coords";
static constexpr char kImageShaderSkSL[] = "";

static constexpr int kNumImageShaderFields = 1;
static constexpr DataPayloadField kImageShaderFields[kNumImageShaderFields] = {
        { "borderColor", SkPaintParamsKey::DataPayloadType::kFloat4, 1 }
};

// This is _not_ what we want to do.
// Ideally the "compute_coords" code snippet could just take texture and
// sampler references and do everything. That is going to take more time to figure out though so,
// for the sake of expediency, we're generating custom code to do the sampling.
std::string GenerateImageShaderGlueCode(const std::string& resultName,
                                        int entryIndex,
                                        const SkPaintParamsKey::BlockReader& reader,
                                        const std::string& priorStageOutputName,
                                        const std::vector<std::string>& childNames,
                                        int indent) {
    SkASSERT(childNames.empty());

    std::string samplerName = std::string("sampler_") + std::to_string(entryIndex) + "_0";

    std::string subsetName = reader.entry()->getMangledUniformName(0, entryIndex);
    std::string tmXName = reader.entry()->getMangledUniformName(1, entryIndex);
    std::string tmYName = reader.entry()->getMangledUniformName(2, entryIndex);

    std::string result;

    add_indent(&result, indent);
    SkSL::String::appendf(&result,
                          "float2 coords = %s(%s, %s, %s);",
                          reader.entry()->fStaticFunctionName,
                          subsetName.c_str(),
                          tmXName.c_str(),
                          tmYName.c_str());

    add_indent(&result, indent);
    SkSL::String::appendf(&result,
                          "%s = sample(%s, coords);\n",
                          resultName.c_str(),
                          samplerName.c_str());


    return result;
}

//--------------------------------------------------------------------------------------------------
static constexpr int kNumBlendShaderUniforms = 4;
static constexpr SkUniform kBlendShaderUniforms[kNumBlendShaderUniforms] = {
        { "blendMode", SkSLType::kInt },
        { "padding1",  SkSLType::kInt }, // TODO: add automatic uniform padding
        { "padding2",  SkSLType::kInt },
        { "padding3",  SkSLType::kInt },
};

static constexpr int kNumBlendShaderChildren = 2;

static constexpr char kBlendHelperName[] = "sk_blend";
static constexpr char kBlendHelperSkSL[] = "";

std::string GenerateBlendShaderGlueCode(const std::string& resultName,
                                        int entryIndex,
                                        const SkPaintParamsKey::BlockReader& reader,
                                        const std::string& priorStageOutputName,
                                        const std::vector<std::string>& childNames,
                                        int indent) {
    SkASSERT(childNames.size() == kNumBlendShaderChildren);
    SkASSERT(reader.entry()->fUniforms.size() == 4); // actual blend uniform + 3 padding int

    std::string uniformName = reader.entry()->getMangledUniformName(0, entryIndex);

    std::string result;

    add_indent(&result, indent);
    SkSL::String::appendf(&result, "%s = %s(%s, %s, %s);\n",
                          resultName.c_str(),
                          reader.entry()->fStaticFunctionName,
                          uniformName.c_str(),
                          childNames[1].c_str(),
                          childNames[0].c_str());

    return result;
}

//--------------------------------------------------------------------------------------------------
static constexpr char kErrorName[] = "sk_error";
static constexpr char kErrorSkSL[] = "";

//--------------------------------------------------------------------------------------------------
static constexpr int kNumFixedFunctionBlenderFields = 1;
static constexpr DataPayloadField kFixedFunctionBlenderFields[kNumFixedFunctionBlenderFields] = {
        { "blendmode", SkPaintParamsKey::DataPayloadType::kByte, 1 }
};

// This method generates the glue code for the case where the SkBlendMode-based blending is
// handled with fixed function blending.
std::string GenerateFixedFunctionBlenderGlueCode(const std::string& resultName,
                                                 int entryIndex,
                                                 const SkPaintParamsKey::BlockReader& reader,
                                                 const std::string& priorStageOutputName,
                                                 const std::vector<std::string>& childNames,
                                                 int indent) {
    SkASSERT(childNames.empty());
    SkASSERT(reader.entry()->fUniforms.empty());
    SkASSERT(reader.numDataPayloadFields() == 1);

    // The actual blending is set up via the fixed function pipeline so we don't actually
    // need to access the blend mode in the glue code.

    std::string result;
    add_indent(&result, indent);
    result += "// Fixed-function blending\n";
    add_indent(&result, indent);
    SkSL::String::appendf(&result, "%s = %s;", resultName.c_str(), priorStageOutputName.c_str());

    return result;
}

//--------------------------------------------------------------------------------------------------
static constexpr int kNumShaderBasedBlenderFields = 1;
static constexpr DataPayloadField kShaderBasedBlenderFields[kNumShaderBasedBlenderFields] = {
        { "blendmode", SkPaintParamsKey::DataPayloadType::kByte, 1 }
};

// This method generates the glue code for the case where the SkBlendMode-based blending must occur
// in the shader (i.e., fixed function blending isn't possible).
std::string GenerateShaderBasedBlenderGlueCode(const std::string& resultName,
                                               int entryIndex,
                                               const SkPaintParamsKey::BlockReader& reader,
                                               const std::string& priorStageOutputName,
                                               const std::vector<std::string>& childNames,
                                               int indent) {
    SkASSERT(childNames.empty());
    SkASSERT(reader.entry()->fUniforms.empty());
    SkASSERT(reader.numDataPayloadFields() == 1);

    SkSpan<const uint8_t> blendMode = reader.bytes(0);
    SkASSERT(blendMode.size() == 1);
    SkASSERT(blendMode[0] <= static_cast<int>(SkBlendMode::kLastMode));

    std::string result;

    add_indent(&result, indent);
    result += "// Shader-based blending\n";
    // TODO: emit code to perform dest read
    add_indent(&result, indent);
    result += "half4 dummyDst = half4(1.0, 1.0, 1.0, 1.0);\n";

    add_indent(&result, indent);
    SkSL::String::appendf(&result, "%s = %s(%d, %s, dummyDst);",
                          resultName.c_str(),
                          reader.entry()->fStaticFunctionName,
                          blendMode[0],
                          priorStageOutputName.c_str());

    return result;
}

//--------------------------------------------------------------------------------------------------

} // anonymous namespace

static constexpr int kNoChildren = 0;

int SkShaderCodeDictionary::addUserDefinedSnippet(
        const char* name,
        SkSpan<const SkPaintParamsKey::DataPayloadField> dataPayloadExpectations) {

    std::unique_ptr<SkShaderSnippet> entry(new SkShaderSnippet({}, // no uniforms
                                                               {}, // no samplers
                                                               name,
                                                               ";",
                                                               GenerateDefaultGlueCode,
                                                               kNoChildren,
                                                               dataPayloadExpectations));

    // TODO: the memory for user-defined entries could go in the dictionary's arena but that
    // would have to be a thread safe allocation since the arena also stores entries for
    // 'fHash' and 'fEntryVector'
    fUserDefinedCodeSnippets.push_back(std::move(entry));

    return kBuiltInCodeSnippetIDCount + fUserDefinedCodeSnippets.size() - 1;
}

SkShaderCodeDictionary::SkShaderCodeDictionary() {
    // The 0th index is reserved as invalid
    fEntryVector.push_back(nullptr);

    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kDepthStencilOnlyDraw] = {
            { },     // no uniforms
            { },     // no samplers
            kErrorName, kErrorSkSL,
            GenerateDefaultGlueCode,
            kNoChildren,
            {}
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kError] = {
            { },     // no uniforms
            { },     // no samplers
            kErrorName, kErrorSkSL,
            GenerateDefaultGlueCode,
            kNoChildren,
            {}
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kSolidColorShader] = {
            SkMakeSpan(kSolidShaderUniforms, kNumSolidShaderUniforms),
            { },     // no samplers
            kSolidShaderName, kSolidShaderSkSL,
            GenerateDefaultGlueCode,
            kNoChildren,
            {}
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kLinearGradientShader] = {
            SkMakeSpan(kGradientUniforms, kNumGradientUniforms),
            { },     // no samplers
            kLinearGradient4Name, kLinearGradient4SkSL,
            GenerateDefaultGlueCode,
            kNoChildren,
            { kLinearGrad4Fields, kNumLinearGrad4Fields }
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kRadialGradientShader] = {
            SkMakeSpan(kGradientUniforms, kNumGradientUniforms),
            { },     // no samplers
            kLinearGradient4Name, kLinearGradient4SkSL,
            GenerateDefaultGlueCode,
            kNoChildren,
            { kLinearGrad4Fields, kNumLinearGrad4Fields }
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kSweepGradientShader] = {
            SkMakeSpan(kGradientUniforms, kNumGradientUniforms),
            { },     // no samplers
            kLinearGradient4Name, kLinearGradient4SkSL,
            GenerateDefaultGlueCode,
            kNoChildren,
            { kLinearGrad4Fields, kNumLinearGrad4Fields }
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kConicalGradientShader] = {
            SkMakeSpan(kGradientUniforms, kNumGradientUniforms),
            { },     // no samplers
            kLinearGradient4Name, kLinearGradient4SkSL,
            GenerateDefaultGlueCode,
            kNoChildren,
            { kLinearGrad4Fields, kNumLinearGrad4Fields }
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kImageShader] = {
            SkMakeSpan(kImageShaderUniforms, kNumImageShaderUniforms),
            SkMakeSpan(kISTexturesAndSamplers, kNumImageShaderTexturesAndSamplers),
            kImageShaderName, kImageShaderSkSL,
            GenerateImageShaderGlueCode,
            kNoChildren,
            { kImageShaderFields, kNumImageShaderFields }
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kBlendShader] = {
            { kBlendShaderUniforms, kNumBlendShaderUniforms },
            { },     // no samplers
            kBlendHelperName, kBlendHelperSkSL,
            GenerateBlendShaderGlueCode,
            kNumBlendShaderChildren,
            {}
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kFixedFunctionBlender] = {
            { },     // no uniforms
            { },     // no samplers
            "FF-blending", "",  // fixed function blending doesn't have any static SkSL
            GenerateFixedFunctionBlenderGlueCode,
            kNoChildren,
            { kFixedFunctionBlenderFields, kNumFixedFunctionBlenderFields }
    };
    fBuiltInCodeSnippets[(int) SkBuiltInCodeSnippetID::kShaderBasedBlender] = {
            { },     // no uniforms
            { },     // no samplers
            kBlendHelperName, kBlendHelperSkSL,
            GenerateShaderBasedBlenderGlueCode,
            kNoChildren,
            { kShaderBasedBlenderFields, kNumShaderBasedBlenderFields }
    };
}
