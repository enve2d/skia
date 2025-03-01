/*
 * Copyright 2019 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/dawn/GrDawnUniformHandler.h"
#include "src/gpu/glsl/GrGLSLProgramBuilder.h"

GrDawnUniformHandler::GrDawnUniformHandler(GrGLSLProgramBuilder* program)
    : INHERITED(program)
    , fUniforms(kUniformsPerBlock)
    , fSamplers(kUniformsPerBlock)
    , fTextures(kUniformsPerBlock)
{
}

const GrShaderVar& GrDawnUniformHandler::getUniformVariable(UniformHandle u) const {
    return fUniforms.item(u.toIndex()).fVariable;
}

const char* GrDawnUniformHandler::getUniformCStr(UniformHandle u) const {
    return fUniforms.item(u.toIndex()).fVariable.getName().c_str();
}

// FIXME: this code was ripped from GrVkUniformHandler; should be refactored.
namespace {

uint32_t grsltype_to_alignment_mask(GrSLType type) {
    switch(type) {
        case kByte_GrSLType: // fall through
        case kUByte_GrSLType:
            return 0x0;
        case kByte2_GrSLType: // fall through
        case kUByte2_GrSLType:
            return 0x1;
        case kByte3_GrSLType: // fall through
        case kByte4_GrSLType:
        case kUByte3_GrSLType:
        case kUByte4_GrSLType:
            return 0x3;
        case kShort_GrSLType: // fall through
        case kUShort_GrSLType:
            return 0x1;
        case kShort2_GrSLType: // fall through
        case kUShort2_GrSLType:
            return 0x3;
        case kShort3_GrSLType: // fall through
        case kShort4_GrSLType:
        case kUShort3_GrSLType:
        case kUShort4_GrSLType:
            return 0x7;
        case kInt_GrSLType:
        case kUint_GrSLType:
            return 0x3;
        case kHalf_GrSLType: // fall through
        case kFloat_GrSLType:
            return 0x3;
        case kHalf2_GrSLType: // fall through
        case kFloat2_GrSLType:
            return 0x7;
        case kHalf3_GrSLType: // fall through
        case kFloat3_GrSLType:
            return 0xF;
        case kHalf4_GrSLType: // fall through
        case kFloat4_GrSLType:
            return 0xF;
        case kUint2_GrSLType:
            return 0x7;
        case kInt2_GrSLType:
            return 0x7;
        case kInt3_GrSLType:
            return 0xF;
        case kInt4_GrSLType:
            return 0xF;
        case kHalf2x2_GrSLType: // fall through
        case kFloat2x2_GrSLType:
            return 0x7;
        case kHalf3x3_GrSLType: // fall through
        case kFloat3x3_GrSLType:
            return 0xF;
        case kHalf4x4_GrSLType: // fall through
        case kFloat4x4_GrSLType:
            return 0xF;

        // This query is only valid for certain types.
        case kVoid_GrSLType:
        case kBool_GrSLType:
        case kTexture2DSampler_GrSLType:
        case kTextureExternalSampler_GrSLType:
        case kTexture2DRectSampler_GrSLType:
        case kTexture2D_GrSLType:
        case kSampler_GrSLType:
            break;
    }
    SK_ABORT("Unexpected type");
}

static inline uint32_t grsltype_to_size(GrSLType type) {
    switch(type) {
        case kByte_GrSLType:
        case kUByte_GrSLType:
            return 1;
        case kByte2_GrSLType:
        case kUByte2_GrSLType:
            return 2;
        case kByte3_GrSLType:
        case kUByte3_GrSLType:
            return 3;
        case kByte4_GrSLType:
        case kUByte4_GrSLType:
            return 4;
        case kShort_GrSLType:
            return sizeof(int16_t);
        case kShort2_GrSLType:
            return 2 * sizeof(int16_t);
        case kShort3_GrSLType:
            return 3 * sizeof(int16_t);
        case kShort4_GrSLType:
            return 4 * sizeof(int16_t);
        case kUShort_GrSLType:
            return sizeof(uint16_t);
        case kUShort2_GrSLType:
            return 2 * sizeof(uint16_t);
        case kUShort3_GrSLType:
            return 3 * sizeof(uint16_t);
        case kUShort4_GrSLType:
            return 4 * sizeof(uint16_t);
        case kInt_GrSLType:
            return sizeof(int32_t);
        case kUint_GrSLType:
            return sizeof(int32_t);
        case kHalf_GrSLType: // fall through
        case kFloat_GrSLType:
            return sizeof(float);
        case kHalf2_GrSLType: // fall through
        case kFloat2_GrSLType:
            return 2 * sizeof(float);
        case kHalf3_GrSLType: // fall through
        case kFloat3_GrSLType:
            return 3 * sizeof(float);
        case kHalf4_GrSLType: // fall through
        case kFloat4_GrSLType:
            return 4 * sizeof(float);
        case kUint2_GrSLType:
            return 2 * sizeof(uint32_t);
        case kInt2_GrSLType:
            return 2 * sizeof(int32_t);
        case kInt3_GrSLType:
            return 3 * sizeof(int32_t);
        case kInt4_GrSLType:
            return 4 * sizeof(int32_t);
        case kHalf2x2_GrSLType: // fall through
        case kFloat2x2_GrSLType:
            //TODO: this will be 4 * szof(float) on std430.
            return 8 * sizeof(float);
        case kHalf3x3_GrSLType: // fall through
        case kFloat3x3_GrSLType:
            return 12 * sizeof(float);
        case kHalf4x4_GrSLType: // fall through
        case kFloat4x4_GrSLType:
            return 16 * sizeof(float);

        // This query is only valid for certain types.
        case kVoid_GrSLType:
        case kBool_GrSLType:
        case kTexture2DSampler_GrSLType:
        case kTextureExternalSampler_GrSLType:
        case kTexture2DRectSampler_GrSLType:
        case kTexture2D_GrSLType:
        case kSampler_GrSLType:
            break;
    }
    SK_ABORT("Unexpected type");
}

uint32_t get_ubo_offset(uint32_t* currentOffset, GrSLType type, int arrayCount) {
    uint32_t alignmentMask = grsltype_to_alignment_mask(type);
    // We want to use the std140 layout here, so we must make arrays align to 16 bytes.
    if (arrayCount || type == kFloat2x2_GrSLType) {
        alignmentMask = 0xF;
    }
    uint32_t offsetDiff = *currentOffset & alignmentMask;
    if (offsetDiff != 0) {
        offsetDiff = alignmentMask - offsetDiff + 1;
    }
    uint32_t uniformOffset = *currentOffset + offsetDiff;
    SkASSERT(sizeof(float) == 4);
    if (arrayCount) {
        uint32_t elementSize = std::max<uint32_t>(16, grsltype_to_size(type));
        SkASSERT(0 == (elementSize & 0xF));
        *currentOffset = uniformOffset + elementSize * arrayCount;
    } else {
        *currentOffset = uniformOffset + grsltype_to_size(type);
    }
    return uniformOffset;
}

}

GrGLSLUniformHandler::UniformHandle GrDawnUniformHandler::internalAddUniformArray(
        const GrFragmentProcessor* owner,
        uint32_t visibility,
        GrSLType type,
        const char* name,
        bool mangleName,
        int arrayCount,
        const char** outName) {
    SkString resolvedName;
    char prefix = 'u';
    if ('u' == name[0] || !strncmp(name, GR_NO_MANGLE_PREFIX, strlen(GR_NO_MANGLE_PREFIX))) {
        prefix = '\0';
    }
    fProgramBuilder->nameVariable(&resolvedName, prefix, name, mangleName);

    int offset = get_ubo_offset(&fCurrentUBOOffset, type, arrayCount);
    SkString layoutQualifier;
    layoutQualifier.appendf("offset = %d", offset);

    UniformInfo& info = fUniforms.push_back(DawnUniformInfo{
        {
            GrShaderVar{std::move(resolvedName), type, GrShaderVar::TypeModifier::None, arrayCount,
                        std::move(layoutQualifier), SkString()},
            visibility, owner, SkString(name)
        },
        offset
    });

    if (outName) {
        *outName = info.fVariable.c_str();
    }
    return GrGLSLUniformHandler::UniformHandle(fUniforms.count() - 1);
}

GrGLSLUniformHandler::SamplerHandle GrDawnUniformHandler::addSampler(const GrBackendFormat&,
                                                                     GrSamplerState,
                                                                     const GrSwizzle& swizzle,
                                                                     const char* name,
                                                                     const GrShaderCaps* caps) {
    int binding = fSamplers.count() * 2;

    SkString mangleName;
    fProgramBuilder->nameVariable(&mangleName, 's', name, true);
    SkString layoutQualifier;
    layoutQualifier.appendf("set = 1, binding = %d", binding);
    DawnUniformInfo& info = fSamplers.push_back(DawnUniformInfo{
        {
            GrShaderVar{std::move(mangleName), kSampler_GrSLType,
                        GrShaderVar::TypeModifier::Uniform, GrShaderVar::kNonArray,
                        std::move(layoutQualifier), SkString()},
            kFragment_GrShaderFlag, nullptr, SkString(name)
        },
        0
    });

    fSamplerSwizzles.push_back(swizzle);
    SkASSERT(fSamplerSwizzles.count() == fSamplers.count());

    SkString mangleTexName;
    fProgramBuilder->nameVariable(&mangleTexName, 't', name, true);
    SkString texLayoutQualifier;
    texLayoutQualifier.appendf("set = 1, binding = %d", binding + 1);
    UniformInfo& texInfo = fTextures.push_back(DawnUniformInfo{
        {
            GrShaderVar{std::move(mangleTexName), kTexture2D_GrSLType,
                        GrShaderVar::TypeModifier::Uniform, GrShaderVar::kNonArray,
                        std::move(texLayoutQualifier), SkString()},
            kFragment_GrShaderFlag, nullptr, SkString(name)
        },
        0
    });

    SkString reference;
    reference.printf("makeSampler2D(%s, %s)", texInfo.fVariable.getName().c_str(),
                                              info.fVariable.getName().c_str());
    fSamplerReferences.emplace_back(std::move(reference));
    return GrGLSLUniformHandler::SamplerHandle(fSamplers.count() - 1);
}

const char* GrDawnUniformHandler::samplerVariable(
        GrGLSLUniformHandler::SamplerHandle handle) const {
    return fSamplerReferences[handle.toIndex()].c_str();
}

GrSwizzle GrDawnUniformHandler::samplerSwizzle(GrGLSLUniformHandler::SamplerHandle handle) const {
    return fSamplerSwizzles[handle.toIndex()];
}

void GrDawnUniformHandler::appendUniformDecls(GrShaderFlags visibility, SkString* out) const {
    auto textures = fTextures.items().begin();
    for (const DawnUniformInfo& sampler : fSamplers.items()) {
        if (sampler.fVisibility & visibility) {
            sampler.fVariable.appendDecl(fProgramBuilder->shaderCaps(), out);
            out->append(";\n");
            (*textures).fVariable.appendDecl(fProgramBuilder->shaderCaps(), out);
            out->append(";\n");
        }
        ++textures;
    }
    SkString uniformsString;
    for (const UniformInfo& uniform : fUniforms.items()) {
        if (uniform.fVisibility & visibility) {
            uniform.fVariable.appendDecl(fProgramBuilder->shaderCaps(), &uniformsString);
            uniformsString.append(";\n");
        }
    }
    if (!uniformsString.isEmpty()) {
        out->appendf("layout (set = 0, binding = %d) uniform UniformBuffer\n{\n", kUniformBinding);
        out->appendf("%s\n};\n", uniformsString.c_str());
    }
}

uint32_t GrDawnUniformHandler::getRTHeightOffset() const {
    uint32_t dummy = fCurrentUBOOffset;
    return get_ubo_offset(&dummy, kFloat_GrSLType, 0);
}
