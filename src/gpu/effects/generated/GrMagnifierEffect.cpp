/*
 * Copyright 2018 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/**************************************************************************************************
 *** This file was autogenerated from GrMagnifierEffect.fp; do not modify.
 **************************************************************************************************/
#include "GrMagnifierEffect.h"

#include "src/gpu/GrTexture.h"
#include "src/gpu/glsl/GrGLSLFragmentProcessor.h"
#include "src/gpu/glsl/GrGLSLFragmentShaderBuilder.h"
#include "src/gpu/glsl/GrGLSLProgramBuilder.h"
#include "src/sksl/SkSLCPP.h"
#include "src/sksl/SkSLUtil.h"
class GrGLSLMagnifierEffect : public GrGLSLFragmentProcessor {
public:
    GrGLSLMagnifierEffect() {}
    void emitCode(EmitArgs& args) override {
        GrGLSLFPFragmentBuilder* fragBuilder = args.fFragBuilder;
        const GrMagnifierEffect& _outer = args.fFp.cast<GrMagnifierEffect>();
        (void)_outer;
        auto bounds = _outer.bounds;
        (void)bounds;
        auto srcRect = _outer.srcRect;
        (void)srcRect;
        auto xInvZoom = _outer.xInvZoom;
        (void)xInvZoom;
        auto yInvZoom = _outer.yInvZoom;
        (void)yInvZoom;
        auto xInvInset = _outer.xInvInset;
        (void)xInvInset;
        auto yInvInset = _outer.yInvInset;
        (void)yInvInset;
        boundsUniformVar = args.fUniformHandler->addUniform(
                &_outer, kFragment_GrShaderFlag, kFloat4_GrSLType, "boundsUniform");
        xInvZoomVar = args.fUniformHandler->addUniform(
                &_outer, kFragment_GrShaderFlag, kFloat_GrSLType, "xInvZoom");
        yInvZoomVar = args.fUniformHandler->addUniform(
                &_outer, kFragment_GrShaderFlag, kFloat_GrSLType, "yInvZoom");
        xInvInsetVar = args.fUniformHandler->addUniform(
                &_outer, kFragment_GrShaderFlag, kFloat_GrSLType, "xInvInset");
        yInvInsetVar = args.fUniformHandler->addUniform(
                &_outer, kFragment_GrShaderFlag, kFloat_GrSLType, "yInvInset");
        offsetVar = args.fUniformHandler->addUniform(
                &_outer, kFragment_GrShaderFlag, kHalf2_GrSLType, "offset");
        SkString sk_TransformedCoords2D_0 =
                fragBuilder->ensureCoords2D(args.fTransformedCoords[0].fVaryingPoint);
        fragBuilder->codeAppendf(
                "float2 coord = %s;\nfloat2 zoom_coord = float2(%s) + coord * float2(%s, "
                "%s);\nfloat2 delta = (coord - %s.xy) * %s.zw;\ndelta = min(delta, "
                "float2(half2(1.0, 1.0)) - delta);\ndelta *= float2(%s, %s);\nfloat weight = "
                "0.0;\nif (delta.x < 2.0 && delta.y < 2.0) {\n    delta = float2(half2(2.0, 2.0)) "
                "- delta;\n    float dist = length(delta);\n    dist = max(2.0 - dist, 0.0);\n    "
                "weight = min(dist * dist, 1.0);\n} else {\n    float2 delta_squared = delta * "
                "delta;\n    weight = min(min(delta_squared.x, delta_square",
                sk_TransformedCoords2D_0.c_str(),
                args.fUniformHandler->getUniformCStr(offsetVar),
                args.fUniformHandler->getUniformCStr(xInvZoomVar),
                args.fUniformHandler->getUniformCStr(yInvZoomVar),
                args.fUniformHandler->getUniformCStr(boundsUniformVar),
                args.fUniformHandler->getUniformCStr(boundsUniformVar),
                args.fUniformHandler->getUniformCStr(xInvInsetVar),
                args.fUniformHandler->getUniformCStr(yInvInsetVar));
        fragBuilder->codeAppendf(
                "d.y), 1.0);\n}\n%s = sample(%s, mix(coord, zoom_coord, weight)).%s;\n",
                args.fOutputColor,
                fragBuilder->getProgramBuilder()->samplerVariable(args.fTexSamplers[0]),
                fragBuilder->getProgramBuilder()
                        ->samplerSwizzle(args.fTexSamplers[0])
                        .asString()
                        .c_str());
    }

private:
    void onSetData(const GrGLSLProgramDataManager& pdman,
                   const GrFragmentProcessor& _proc) override {
        const GrMagnifierEffect& _outer = _proc.cast<GrMagnifierEffect>();
        {
            pdman.set1f(xInvZoomVar, (_outer.xInvZoom));
            pdman.set1f(yInvZoomVar, (_outer.yInvZoom));
            pdman.set1f(xInvInsetVar, (_outer.xInvInset));
            pdman.set1f(yInvInsetVar, (_outer.yInvInset));
        }
        const GrSurfaceProxyView& srcView = _outer.textureSampler(0).view();
        GrTexture& src = *srcView.proxy()->peekTexture();
        (void)src;
        auto bounds = _outer.bounds;
        (void)bounds;
        UniformHandle& boundsUniform = boundsUniformVar;
        (void)boundsUniform;
        auto srcRect = _outer.srcRect;
        (void)srcRect;
        UniformHandle& xInvZoom = xInvZoomVar;
        (void)xInvZoom;
        UniformHandle& yInvZoom = yInvZoomVar;
        (void)yInvZoom;
        UniformHandle& xInvInset = xInvInsetVar;
        (void)xInvInset;
        UniformHandle& yInvInset = yInvInsetVar;
        (void)yInvInset;
        UniformHandle& offset = offsetVar;
        (void)offset;

        SkScalar invW = 1.0f / src.width();
        SkScalar invH = 1.0f / src.height();

        {
            SkScalar y = srcRect.y() * invH;
            if (srcView.origin() != kTopLeft_GrSurfaceOrigin) {
                y = 1.0f - (srcRect.height() / bounds.height()) - y;
            }

            pdman.set2f(offset, srcRect.x() * invW, y);
        }

        {
            SkScalar y = bounds.y() * invH;
            SkScalar hSign = 1.f;
            if (srcView.origin() != kTopLeft_GrSurfaceOrigin) {
                y = 1.0f - bounds.y() * invH;
                hSign = -1.f;
            }

            pdman.set4f(boundsUniform,
                        bounds.x() * invW,
                        y,
                        SkIntToScalar(src.width()) / bounds.width(),
                        hSign * SkIntToScalar(src.height()) / bounds.height());
        }
    }
    UniformHandle boundsUniformVar;
    UniformHandle offsetVar;
    UniformHandle xInvZoomVar;
    UniformHandle yInvZoomVar;
    UniformHandle xInvInsetVar;
    UniformHandle yInvInsetVar;
};
GrGLSLFragmentProcessor* GrMagnifierEffect::onCreateGLSLInstance() const {
    return new GrGLSLMagnifierEffect();
}
void GrMagnifierEffect::onGetGLSLProcessorKey(const GrShaderCaps& caps,
                                              GrProcessorKeyBuilder* b) const {}
bool GrMagnifierEffect::onIsEqual(const GrFragmentProcessor& other) const {
    const GrMagnifierEffect& that = other.cast<GrMagnifierEffect>();
    (void)that;
    if (src != that.src) return false;
    if (bounds != that.bounds) return false;
    if (srcRect != that.srcRect) return false;
    if (xInvZoom != that.xInvZoom) return false;
    if (yInvZoom != that.yInvZoom) return false;
    if (xInvInset != that.xInvInset) return false;
    if (yInvInset != that.yInvInset) return false;
    return true;
}
GrMagnifierEffect::GrMagnifierEffect(const GrMagnifierEffect& src)
        : INHERITED(kGrMagnifierEffect_ClassID, src.optimizationFlags())
        , srcCoordTransform(src.srcCoordTransform)
        , src(src.src)
        , bounds(src.bounds)
        , srcRect(src.srcRect)
        , xInvZoom(src.xInvZoom)
        , yInvZoom(src.yInvZoom)
        , xInvInset(src.xInvInset)
        , yInvInset(src.yInvInset) {
    this->setTextureSamplerCnt(1);
    this->addCoordTransform(&srcCoordTransform);
}
std::unique_ptr<GrFragmentProcessor> GrMagnifierEffect::clone() const {
    return std::unique_ptr<GrFragmentProcessor>(new GrMagnifierEffect(*this));
}
const GrFragmentProcessor::TextureSampler& GrMagnifierEffect::onTextureSampler(int index) const {
    return IthTextureSampler(index, src);
}
GR_DEFINE_FRAGMENT_PROCESSOR_TEST(GrMagnifierEffect);
#if GR_TEST_UTILS
std::unique_ptr<GrFragmentProcessor> GrMagnifierEffect::TestCreate(GrProcessorTestData* d) {
    auto[view, ct, at] = d->randomView();
    const int kMaxWidth = 200;
    const int kMaxHeight = 200;
    const SkScalar kMaxInset = 20.0f;
    uint32_t width = d->fRandom->nextULessThan(kMaxWidth);
    uint32_t height = d->fRandom->nextULessThan(kMaxHeight);
    SkScalar inset = d->fRandom->nextRangeScalar(1.0f, kMaxInset);

    SkIRect bounds = SkIRect::MakeWH(SkIntToScalar(kMaxWidth), SkIntToScalar(kMaxHeight));
    SkRect srcRect = SkRect::MakeWH(SkIntToScalar(width), SkIntToScalar(height));

    auto effect = GrMagnifierEffect::Make(std::move(view),
                                          bounds,
                                          srcRect,
                                          srcRect.width() / bounds.width(),
                                          srcRect.height() / bounds.height(),
                                          bounds.width() / inset,
                                          bounds.height() / inset);
    SkASSERT(effect);
    return effect;
}
#endif
