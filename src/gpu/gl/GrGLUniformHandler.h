/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef GrGLUniformHandler_DEFINED
#define GrGLUniformHandler_DEFINED

#include "src/gpu/gl/GrGLProgramDataManager.h"
#include "src/gpu/glsl/GrGLSLUniformHandler.h"

#include <vector>

class GrGLCaps;

class GrGLUniformHandler : public GrGLSLUniformHandler {
public:
    static const int kUniformsPerBlock = 8;

    const GrShaderVar& getUniformVariable(UniformHandle u) const override {
        return fUniforms.item(u.toIndex()).fVariable;
    }

    const char* getUniformCStr(UniformHandle u) const override {
        return this->getUniformVariable(u).c_str();
    }
private:
    explicit GrGLUniformHandler(GrGLSLProgramBuilder* program)
        : INHERITED(program)
        , fUniforms(kUniformsPerBlock)
        , fSamplers(kUniformsPerBlock) {}

    UniformHandle internalAddUniformArray(const GrFragmentProcessor* owner,
                                          uint32_t visibility,
                                          GrSLType type,
                                          const char* name,
                                          bool mangleName,
                                          int arrayCount,
                                          const char** outName) override;

    SamplerHandle addSampler(const GrBackendFormat&, GrSamplerState, const GrSwizzle&,
                             const char* name, const GrShaderCaps*) override;

    const char* samplerVariable(SamplerHandle handle) const override {
        return fSamplers.item(handle.toIndex()).fVariable.c_str();
    }

    GrSwizzle samplerSwizzle(SamplerHandle handle) const override {
        return fSamplerSwizzles[handle.toIndex()];
    }

    void appendUniformDecls(GrShaderFlags visibility, SkString*) const override;

    // Manually set uniform locations for all our uniforms.
    void bindUniformLocations(GrGLuint programID, const GrGLCaps& caps);

    // Updates the loction of the Uniforms if we cannot bind uniform locations manually
    void getUniformLocations(GrGLuint programID, const GrGLCaps& caps, bool force);

    const GrGLGpu* glGpu() const;

    typedef GrGLProgramDataManager::GLUniformInfo GLUniformInfo;
    typedef GrGLProgramDataManager::UniformInfoArray UniformInfoArray;

    UniformInfoArray            fUniforms;
    UniformInfoArray            fSamplers;
    SkTArray<GrSwizzle>         fSamplerSwizzles;

    friend class GrGLProgramBuilder;

    typedef GrGLSLUniformHandler INHERITED;
};

#endif
