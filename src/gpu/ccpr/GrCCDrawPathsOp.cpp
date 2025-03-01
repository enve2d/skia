/*
 * Copyright 2018 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/ccpr/GrCCDrawPathsOp.h"

#include "include/private/GrRecordingContext.h"
#include "src/gpu/GrMemoryPool.h"
#include "src/gpu/GrOpFlushState.h"
#include "src/gpu/GrRecordingContextPriv.h"
#include "src/gpu/ccpr/GrCCPathCache.h"
#include "src/gpu/ccpr/GrCCPerFlushResources.h"
#include "src/gpu/ccpr/GrCoverageCountingPathRenderer.h"
#include "src/gpu/ccpr/GrOctoBounds.h"

static bool has_coord_transforms(const GrPaint& paint) {
    for (const auto& fp : GrFragmentProcessor::PaintCRange(paint)) {
        if (!fp.coordTransforms().empty()) {
            return true;
        }
    }
    return false;
}

std::unique_ptr<GrCCDrawPathsOp> GrCCDrawPathsOp::Make(
        GrRecordingContext* context, const SkIRect& clipIBounds, const SkMatrix& m,
        const GrShape& shape, GrPaint&& paint) {
    SkRect conservativeDevBounds;
    m.mapRect(&conservativeDevBounds, shape.bounds());

    const SkStrokeRec& stroke = shape.style().strokeRec();
    float strokeDevWidth = 0;
    float conservativeInflationRadius = 0;
    if (!stroke.isFillStyle()) {
        strokeDevWidth = GrCoverageCountingPathRenderer::GetStrokeDevWidth(
                m, stroke, &conservativeInflationRadius);
        conservativeDevBounds.outset(conservativeInflationRadius, conservativeInflationRadius);
    }

    std::unique_ptr<GrCCDrawPathsOp> op;
    float conservativeSize = std::max(conservativeDevBounds.height(), conservativeDevBounds.width());
    if (conservativeSize > GrCoverageCountingPathRenderer::kPathCropThreshold) {
        // The path is too large. Crop it or analytic AA can run out of fp32 precision.
        SkPath croppedDevPath;
        shape.asPath(&croppedDevPath);
        croppedDevPath.transform(m, &croppedDevPath);

        SkIRect cropBox = clipIBounds;
        GrShape croppedDevShape;
        if (stroke.isFillStyle()) {
            GrCoverageCountingPathRenderer::CropPath(croppedDevPath, cropBox, &croppedDevPath);
            croppedDevShape = GrShape(croppedDevPath);
            conservativeDevBounds = croppedDevShape.bounds();
        } else {
            int r = SkScalarCeilToInt(conservativeInflationRadius);
            cropBox.outset(r, r);
            GrCoverageCountingPathRenderer::CropPath(croppedDevPath, cropBox, &croppedDevPath);
            SkStrokeRec devStroke = stroke;
            devStroke.setStrokeStyle(strokeDevWidth);
            croppedDevShape = GrShape(croppedDevPath, GrStyle(devStroke, nullptr));
            conservativeDevBounds = croppedDevPath.getBounds();
            conservativeDevBounds.outset(conservativeInflationRadius, conservativeInflationRadius);
        }

        // FIXME: This breaks local coords: http://skbug.com/8003
        return InternalMake(context, clipIBounds, SkMatrix::I(), croppedDevShape, strokeDevWidth,
                            conservativeDevBounds, std::move(paint));
    }

    return InternalMake(context, clipIBounds, m, shape, strokeDevWidth, conservativeDevBounds,
                        std::move(paint));
}

std::unique_ptr<GrCCDrawPathsOp> GrCCDrawPathsOp::InternalMake(
        GrRecordingContext* context, const SkIRect& clipIBounds, const SkMatrix& m,
        const GrShape& shape, float strokeDevWidth, const SkRect& conservativeDevBounds,
        GrPaint&& paint) {
    // The path itself should have been cropped if larger than kPathCropThreshold. If it had a
    // stroke, that would have further inflated its draw bounds.
    SkASSERT(std::max(conservativeDevBounds.height(), conservativeDevBounds.width()) <
             GrCoverageCountingPathRenderer::kPathCropThreshold +
             GrCoverageCountingPathRenderer::kMaxBoundsInflationFromStroke*2 + 1);

    SkIRect shapeConservativeIBounds;
    conservativeDevBounds.roundOut(&shapeConservativeIBounds);

    SkIRect maskDevIBounds;
    if (!maskDevIBounds.intersect(clipIBounds, shapeConservativeIBounds)) {
        return nullptr;
    }

    GrOpMemoryPool* pool = context->priv().opMemoryPool();
    return pool->allocate<GrCCDrawPathsOp>(m, shape, strokeDevWidth, shapeConservativeIBounds,
                                           maskDevIBounds, conservativeDevBounds, std::move(paint));
}

GrCCDrawPathsOp::GrCCDrawPathsOp(const SkMatrix& m, const GrShape& shape, float strokeDevWidth,
                                 const SkIRect& shapeConservativeIBounds,
                                 const SkIRect& maskDevIBounds, const SkRect& conservativeDevBounds,
                                 GrPaint&& paint)
        : GrDrawOp(ClassID())
        , fViewMatrixIfUsingLocalCoords(has_coord_transforms(paint) ? m : SkMatrix::I())
        , fDraws(m, shape, strokeDevWidth, shapeConservativeIBounds, maskDevIBounds,
                 paint.getColor4f())
        , fProcessors(std::move(paint)) {  // Paint must be moved after fetching its color above.
    SkDEBUGCODE(fBaseInstance = -1);
    // If the path is clipped, CCPR will only draw the visible portion. This helps improve batching,
    // since it eliminates the need for scissor when drawing to the main canvas.
    // FIXME: We should parse the path right here. It will provide a tighter bounding box for us to
    // give the opsTask, as well as enabling threaded parsing when using DDL.
    SkRect clippedDrawBounds;
    if (!clippedDrawBounds.intersect(conservativeDevBounds, SkRect::Make(maskDevIBounds))) {
        clippedDrawBounds.setEmpty();
    }
    // We always have AA bloat, even in MSAA atlas mode. This is because by the time this Op comes
    // along and draws to the main canvas, the atlas has been resolved to analytic coverage.
    this->setBounds(clippedDrawBounds, GrOp::HasAABloat::kYes, GrOp::IsHairline::kNo);
}

GrCCDrawPathsOp::~GrCCDrawPathsOp() {
    if (fOwningPerOpsTaskPaths) {
        // Remove the list's dangling pointer to this Op before deleting it.
        fOwningPerOpsTaskPaths->fDrawOps.remove(this);
    }
}

GrCCDrawPathsOp::SingleDraw::SingleDraw(const SkMatrix& m, const GrShape& shape,
                                        float strokeDevWidth,
                                        const SkIRect& shapeConservativeIBounds,
                                        const SkIRect& maskDevIBounds, const SkPMColor4f& color)
        : fMatrix(m)
        , fShape(shape)
        , fStrokeDevWidth(strokeDevWidth)
        , fShapeConservativeIBounds(shapeConservativeIBounds)
        , fMaskDevIBounds(maskDevIBounds)
        , fColor(color) {
#ifdef SK_BUILD_FOR_ANDROID_FRAMEWORK
    if (fShape.hasUnstyledKey()) {
        // On AOSP we round view matrix translates to integer values for cachable paths. We do this
        // to match HWUI's cache hit ratio, which doesn't consider the matrix when caching paths.
        fMatrix.setTranslateX(SkScalarRoundToScalar(fMatrix.getTranslateX()));
        fMatrix.setTranslateY(SkScalarRoundToScalar(fMatrix.getTranslateY()));
    }
#endif
}

GrProcessorSet::Analysis GrCCDrawPathsOp::finalize(
        const GrCaps& caps, const GrAppliedClip* clip, bool hasMixedSampledCoverage,
        GrClampType clampType) {
    SkASSERT(1 == fNumDraws);  // There should only be one single path draw in this Op right now.
    return fDraws.head().finalize(caps, clip, hasMixedSampledCoverage, clampType, &fProcessors);
}

GrProcessorSet::Analysis GrCCDrawPathsOp::SingleDraw::finalize(
        const GrCaps& caps, const GrAppliedClip* clip, bool hasMixedSampledCoverage, GrClampType
        clampType, GrProcessorSet* processors) {
    const GrProcessorSet::Analysis& analysis = processors->finalize(
            fColor, GrProcessorAnalysisCoverage::kSingleChannel, clip,
            &GrUserStencilSettings::kUnused, hasMixedSampledCoverage, caps, clampType, &fColor);

    // Lines start looking jagged when they get thinner than 1px. For thin strokes it looks better
    // if we can convert them to hairline (i.e., inflate the stroke width to 1px), and instead
    // reduce the opacity to create the illusion of thin-ness. This strategy also helps reduce
    // artifacts from coverage dilation when there are self intersections.
    if (analysis.isCompatibleWithCoverageAsAlpha() &&
            !fShape.style().strokeRec().isFillStyle() && fStrokeDevWidth < 1) {
        // Modifying the shape affects its cache key. The draw can't have a cache entry yet or else
        // our next step would invalidate it.
        SkASSERT(!fCacheEntry);
        SkASSERT(SkStrokeRec::kStroke_Style == fShape.style().strokeRec().getStyle());

        SkPath path;
        fShape.asPath(&path);

        // Create a hairline version of our stroke.
        SkStrokeRec hairlineStroke = fShape.style().strokeRec();
        hairlineStroke.setStrokeStyle(0);

        // How transparent does a 1px stroke have to be in order to appear as thin as the real one?
        float coverage = fStrokeDevWidth;

        fShape = GrShape(path, GrStyle(hairlineStroke, nullptr));
        fStrokeDevWidth = 1;

        // fShapeConservativeIBounds already accounted for this possibility of inflating the stroke.
        fColor = fColor * coverage;
    }

    return analysis;
}

GrOp::CombineResult GrCCDrawPathsOp::onCombineIfPossible(GrOp* op, GrRecordingContext::Arenas*,
                                                         const GrCaps&) {
    GrCCDrawPathsOp* that = op->cast<GrCCDrawPathsOp>();
    SkASSERT(fOwningPerOpsTaskPaths);
    SkASSERT(fNumDraws);
    SkASSERT(!that->fOwningPerOpsTaskPaths ||
             that->fOwningPerOpsTaskPaths == fOwningPerOpsTaskPaths);
    SkASSERT(that->fNumDraws);

    if (fProcessors != that->fProcessors ||
        fViewMatrixIfUsingLocalCoords != that->fViewMatrixIfUsingLocalCoords) {
        return CombineResult::kCannotCombine;
    }

    fDraws.append(std::move(that->fDraws), &fOwningPerOpsTaskPaths->fAllocator);

    SkDEBUGCODE(fNumDraws += that->fNumDraws);
    SkDEBUGCODE(that->fNumDraws = 0);
    return CombineResult::kMerged;
}

void GrCCDrawPathsOp::addToOwningPerOpsTaskPaths(sk_sp<GrCCPerOpsTaskPaths> owningPerOpsTaskPaths) {
    SkASSERT(1 == fNumDraws);
    SkASSERT(!fOwningPerOpsTaskPaths);
    fOwningPerOpsTaskPaths = std::move(owningPerOpsTaskPaths);
    fOwningPerOpsTaskPaths->fDrawOps.addToTail(this);
}

void GrCCDrawPathsOp::accountForOwnPaths(GrCCPathCache* pathCache,
                                         GrOnFlushResourceProvider* onFlushRP,
                                         GrCCPerFlushResourceSpecs* specs) {
    for (SingleDraw& draw : fDraws) {
        draw.accountForOwnPath(pathCache, onFlushRP, specs);
    }
}

void GrCCDrawPathsOp::SingleDraw::accountForOwnPath(
        GrCCPathCache* pathCache, GrOnFlushResourceProvider* onFlushRP,
        GrCCPerFlushResourceSpecs* specs) {
    using CoverageType = GrCCAtlas::CoverageType;

    SkPath path;
    fShape.asPath(&path);

    SkASSERT(!fCacheEntry);

    if (pathCache) {
        fCacheEntry = pathCache->find(
                onFlushRP, fShape, fMaskDevIBounds, fMatrix, &fCachedMaskShift);
    }

    if (fCacheEntry) {
        if (const GrCCCachedAtlas* cachedAtlas = fCacheEntry->cachedAtlas()) {
            SkASSERT(cachedAtlas->getOnFlushProxy());
            if (CoverageType::kA8_LiteralCoverage == cachedAtlas->coverageType()) {
                ++specs->fNumCachedPaths;
            } else {
                // Suggest that this path be copied to a literal coverage atlas, to save memory.
                // (The client may decline this copy via DoCopiesToA8Coverage::kNo.)
                int idx = (fShape.style().strokeRec().isFillStyle())
                        ? GrCCPerFlushResourceSpecs::kFillIdx
                        : GrCCPerFlushResourceSpecs::kStrokeIdx;
                ++specs->fNumCopiedPaths[idx];
                specs->fCopyPathStats[idx].statPath(path);
                specs->fCopyAtlasSpecs.accountForSpace(fCacheEntry->width(), fCacheEntry->height());
                fDoCopyToA8Coverage = true;
            }
            return;
        }

        if (this->shouldCachePathMask(onFlushRP->caps()->maxRenderTargetSize())) {
            fDoCachePathMask = true;
            // We don't cache partial masks; ensure the bounds include the entire path.
            fMaskDevIBounds = fShapeConservativeIBounds;
        }
    }

    // Plan on rendering this path in a new atlas.
    int idx = (fShape.style().strokeRec().isFillStyle())
            ? GrCCPerFlushResourceSpecs::kFillIdx
            : GrCCPerFlushResourceSpecs::kStrokeIdx;
    ++specs->fNumRenderedPaths[idx];
    specs->fRenderedPathStats[idx].statPath(path);
    specs->fRenderedAtlasSpecs.accountForSpace(fMaskDevIBounds.width(), fMaskDevIBounds.height());
    SkDEBUGCODE(fWasCountedAsRender = true);
}

bool GrCCDrawPathsOp::SingleDraw::shouldCachePathMask(int maxRenderTargetSize) const {
    SkASSERT(fCacheEntry);
    SkASSERT(!fCacheEntry->cachedAtlas());
    if (fCacheEntry->hitCount() <= 1) {
        return false;  // Don't cache a path mask until at least its second hit.
    }

    int shapeMaxDimension = std::max(
            fShapeConservativeIBounds.height(), fShapeConservativeIBounds.width());
    if (shapeMaxDimension > maxRenderTargetSize) {
        return false;  // This path isn't cachable.
    }

    int64_t shapeArea = sk_64_mul(
            fShapeConservativeIBounds.height(), fShapeConservativeIBounds.width());
    if (shapeArea < 100*100) {
        // If a path is small enough, we might as well try to render and cache the entire thing, no
        // matter how much of it is actually visible.
        return true;
    }

    // The hitRect should already be contained within the shape's bounds, but we still intersect it
    // because it's possible for edges very near pixel boundaries (e.g., 0.999999), to round out
    // inconsistently, depending on the integer translation values and fp32 precision.
    SkIRect hitRect = fCacheEntry->hitRect().makeOffset(fCachedMaskShift);
    hitRect.intersect(fShapeConservativeIBounds);

    // Render and cache the entire path mask if we see enough of it to justify rendering all the
    // pixels. Our criteria for "enough" is that we must have seen at least 50% of the path in the
    // past, and in this particular draw we must see at least 10% of it.
    int64_t hitArea = sk_64_mul(hitRect.height(), hitRect.width());
    int64_t drawArea = sk_64_mul(fMaskDevIBounds.height(), fMaskDevIBounds.width());
    return hitArea*2 >= shapeArea && drawArea*10 >= shapeArea;
}

void GrCCDrawPathsOp::setupResources(
        GrCCPathCache* pathCache, GrOnFlushResourceProvider* onFlushRP,
        GrCCPerFlushResources* resources, DoCopiesToA8Coverage doCopies) {
    SkASSERT(fNumDraws > 0);
    SkASSERT(-1 == fBaseInstance);
    fBaseInstance = resources->nextPathInstanceIdx();

    for (SingleDraw& draw : fDraws) {
        draw.setupResources(pathCache, onFlushRP, resources, doCopies, this);
    }

    if (!fInstanceRanges.empty()) {
        fInstanceRanges.back().fEndInstanceIdx = resources->nextPathInstanceIdx();
    }
}

void GrCCDrawPathsOp::SingleDraw::setupResources(
        GrCCPathCache* pathCache, GrOnFlushResourceProvider* onFlushRP,
        GrCCPerFlushResources* resources, DoCopiesToA8Coverage doCopies, GrCCDrawPathsOp* op) {
    SkPath path;
    fShape.asPath(&path);

    auto fillRule = (fShape.style().strokeRec().isFillStyle())
            ? GrFillRuleForSkPath(path)
            : GrFillRule::kNonzero;

    if (fCacheEntry) {
        // Does the path already exist in a cached atlas texture?
        if (fCacheEntry->cachedAtlas()) {
            SkASSERT(fCacheEntry->cachedAtlas()->getOnFlushProxy());
            if (DoCopiesToA8Coverage::kYes == doCopies && fDoCopyToA8Coverage) {
                resources->upgradeEntryToLiteralCoverageAtlas(
                        pathCache, onFlushRP, fCacheEntry.get(), fillRule);
                SkASSERT(fCacheEntry->cachedAtlas());
                SkASSERT(GrCCAtlas::CoverageType::kA8_LiteralCoverage
                                 == fCacheEntry->cachedAtlas()->coverageType());
                SkASSERT(fCacheEntry->cachedAtlas()->getOnFlushProxy());
            }
#if 0
            // Simple color manipulation to visualize cached paths.
            fColor = (GrCCAtlas::CoverageType::kA8_LiteralCoverage
                              == fCacheEntry->cachedAtlas()->coverageType())
                    ? SkPMColor4f{0,0,.25,.25} : SkPMColor4f{0,.25,0,.25};
#endif
            auto coverageMode = GrCCAtlas::CoverageTypeToPathCoverageMode(
                    fCacheEntry->cachedAtlas()->coverageType());
            op->recordInstance(coverageMode, fCacheEntry->cachedAtlas()->getOnFlushProxy(),
                               resources->nextPathInstanceIdx());
            resources->appendDrawPathInstance().set(*fCacheEntry, fCachedMaskShift, fColor,
                                                    fillRule);
#ifdef SK_DEBUG
            if (fWasCountedAsRender) {
                // A path mask didn't exist for this path at the beginning of flush, but we have one
                // now. What this means is that we've drawn the same path multiple times this flush.
                // Let the resources know that we reused one for their internal debug counters.
                resources->debugOnly_didReuseRenderedPath();
            }
#endif
            return;
        }
    }

    // Render the raw path into a coverage count atlas. renderShapeInAtlas() gives us two tight
    // bounding boxes: One in device space, as well as a second one rotated an additional 45
    // degrees. The path vertex shader uses these two bounding boxes to generate an octagon that
    // circumscribes the path.
    GrOctoBounds octoBounds;
    SkIRect devIBounds;
    SkIVector devToAtlasOffset;
    if (auto atlas = resources->renderShapeInAtlas(
                fMaskDevIBounds, fMatrix, fShape, fStrokeDevWidth, &octoBounds, &devIBounds,
                &devToAtlasOffset)) {
        auto coverageMode = GrCCAtlas::CoverageTypeToPathCoverageMode(
                resources->renderedPathCoverageType());
        op->recordInstance(coverageMode, atlas->textureProxy(), resources->nextPathInstanceIdx());
        resources->appendDrawPathInstance().set(octoBounds, devToAtlasOffset, fColor, fillRule);

        if (fDoCachePathMask) {
            SkASSERT(fCacheEntry);
            SkASSERT(!fCacheEntry->cachedAtlas());
            SkASSERT(fShapeConservativeIBounds == fMaskDevIBounds);
            fCacheEntry->setCoverageCountAtlas(
                    onFlushRP, atlas, devToAtlasOffset, octoBounds, devIBounds, fCachedMaskShift);
        }
    }
}

inline void GrCCDrawPathsOp::recordInstance(
        GrCCPathProcessor::CoverageMode coverageMode, GrTextureProxy* atlasProxy, int instanceIdx) {
    if (fInstanceRanges.empty()) {
        fInstanceRanges.push_back({coverageMode, atlasProxy, instanceIdx});
    } else if (fInstanceRanges.back().fAtlasProxy != atlasProxy) {
        fInstanceRanges.back().fEndInstanceIdx = instanceIdx;
        fInstanceRanges.push_back({coverageMode, atlasProxy, instanceIdx});
    }
    SkASSERT(fInstanceRanges.back().fCoverageMode == coverageMode);
    SkASSERT(fInstanceRanges.back().fAtlasProxy == atlasProxy);
}

void GrCCDrawPathsOp::onPrepare(GrOpFlushState* flushState) {
    // The CCPR ops don't know their atlas textures until after the preFlush calls have been
    // executed at the start GrDrawingManger::flush. Thus the proxies are not added during the
    // normal visitProxies calls doing addDrawOp. Therefore, the atlas proxies are added now.
    for (const InstanceRange& range : fInstanceRanges) {
        flushState->sampledProxyArray()->push_back(range.fAtlasProxy);
    }
}

void GrCCDrawPathsOp::onExecute(GrOpFlushState* flushState, const SkRect& chainBounds) {
    SkASSERT(fOwningPerOpsTaskPaths);

    const GrCCPerFlushResources* resources = fOwningPerOpsTaskPaths->fFlushResources.get();
    if (!resources) {
        return;  // Setup failed.
    }

    GrPipeline::InitArgs initArgs;
    initArgs.fCaps = &flushState->caps();
    initArgs.fDstProxyView = flushState->drawOpArgs().dstProxyView();
    initArgs.fWriteSwizzle = flushState->drawOpArgs().writeSwizzle();
    auto clip = flushState->detachAppliedClip();
    GrPipeline pipeline(initArgs, std::move(fProcessors), std::move(clip));

    int baseInstance = fBaseInstance;
    SkASSERT(baseInstance >= 0);  // Make sure setupResources() has been called.

    for (const InstanceRange& range : fInstanceRanges) {
        SkASSERT(range.fEndInstanceIdx > baseInstance);

        GrSurfaceProxy* atlas = range.fAtlasProxy;
        if (atlas->isInstantiated()) {  // Instantiation can fail in exceptional circumstances.
            GrColorType ct = GrCCPathProcessor::GetColorTypeFromCoverageMode(range.fCoverageMode);
            GrSwizzle swizzle = flushState->caps().getReadSwizzle(atlas->backendFormat(), ct);
            GrCCPathProcessor pathProc(range.fCoverageMode, atlas->peekTexture(), swizzle,
                                       GrCCAtlas::kTextureOrigin, fViewMatrixIfUsingLocalCoords);
            pathProc.drawPaths(flushState, pipeline, *atlas, *resources, baseInstance,
                               range.fEndInstanceIdx, this->bounds());
        }

        baseInstance = range.fEndInstanceIdx;
    }
}
