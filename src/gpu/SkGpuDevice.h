/*
 * Copyright 2010 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SkGpuDevice_DEFINED
#define SkGpuDevice_DEFINED

#include "include/core/SkBitmap.h"
#include "include/core/SkPicture.h"
#include "include/core/SkRegion.h"
#include "include/core/SkSurface.h"
#include "include/core/SkVertices.h"
#include "include/gpu/GrContext.h"
#include "include/gpu/GrTypes.h"
#include "src/core/SkClipStackDevice.h"
#include "src/gpu/GrClipStackClip.h"
#include "src/gpu/GrContextPriv.h"
#include "src/gpu/GrRenderTargetContext.h"
#include "src/gpu/SkGr.h"

class GrAccelData;
class GrTextureMaker;
class GrTextureProducer;
struct GrCachedLayer;

class SkSpecialImage;
class SkSurface;

/**
 *  Subclass of SkBaseDevice, which directs all drawing to the GrGpu owned by the
 *  canvas.
 */
class SkGpuDevice : public SkClipStackDevice {
public:
    enum InitContents {
        kClear_InitContents,
        kUninit_InitContents
    };

    /**
     * Creates an SkGpuDevice from a GrRenderTargetContext whose backing width/height is
     * different than its actual width/height (e.g., approx-match scratch texture).
     */
    static sk_sp<SkGpuDevice> Make(
            GrContext*, std::unique_ptr<GrRenderTargetContext>, InitContents);

    /**
     * New device that will create an offscreen renderTarget based on the ImageInfo and
     * sampleCount. The mipMapped flag tells the gpu to create the underlying render target with
     * mips. The Budgeted param controls whether the device's backing store counts against the
     * resource cache budget. On failure, returns nullptr.
     * This entry point creates a kExact backing store. It is used when creating SkGpuDevices
     * for SkSurfaces.
     */
    static sk_sp<SkGpuDevice> Make(GrContext*, SkBudgeted, const SkImageInfo&,
                                   int sampleCount, GrSurfaceOrigin, const SkSurfaceProps*,
                                   GrMipMapped mipMapped, InitContents);

    ~SkGpuDevice() override {}

    GrContext* context() const override { return fContext.get(); }

    // set all pixels to 0
    void clearAll();

    void replaceRenderTargetContext(SkSurface::ContentChangeMode mode);
    void replaceRenderTargetContext(std::unique_ptr<GrRenderTargetContext>,
                                    SkSurface::ContentChangeMode mode);

    GrRenderTargetContext* accessRenderTargetContext() override;

    void drawPaint(const SkPaint& paint) override;
    void drawPoints(SkCanvas::PointMode mode, size_t count, const SkPoint[],
                    const SkPaint& paint) override;
    void drawRect(const SkRect& r, const SkPaint& paint) override;
    void drawRRect(const SkRRect& r, const SkPaint& paint) override;
    void drawDRRect(const SkRRect& outer, const SkRRect& inner, const SkPaint& paint) override;
    void drawRegion(const SkRegion& r, const SkPaint& paint) override;
    void drawOval(const SkRect& oval, const SkPaint& paint) override;
    void drawArc(const SkRect& oval, SkScalar startAngle, SkScalar sweepAngle,
                 bool useCenter, const SkPaint& paint) override;
    void drawPath(const SkPath& path, const SkPaint& paint, bool pathIsMutable) override;

    void drawGlyphRunList(const SkGlyphRunList& glyphRunList) override;
    void drawVertices(const SkVertices*, SkBlendMode, const SkPaint&) override;
    void drawShadow(const SkPath&, const SkDrawShadowRec&) override;
    void drawAtlas(const SkImage* atlas, const SkRSXform[], const SkRect[],
                   const SkColor[], int count, SkBlendMode, const SkPaint&) override;

    void drawImageRect(const SkImage*, const SkRect* src, const SkRect& dst,
                       const SkPaint&, SkCanvas::SrcRectConstraint) override;
    void drawImageNine(const SkImage* image, const SkIRect& center,
                       const SkRect& dst, const SkPaint& paint) override;
    void drawImageLattice(const SkImage*, const SkCanvas::Lattice&,
                          const SkRect& dst, const SkPaint&) override;

    void drawDrawable(SkDrawable*, const SkMatrix*, SkCanvas* canvas) override;

    void drawDevice(SkBaseDevice*, int x, int y, const SkPaint&) override;

    void drawSpecial(SkSpecialImage*, int left, int top, const SkPaint& paint,
                     SkImage*, const SkMatrix&) override;

    void drawEdgeAAQuad(const SkRect& rect, const SkPoint clip[4], SkCanvas::QuadAAFlags aaFlags,
                        const SkColor4f& color, SkBlendMode mode) override;
    void drawEdgeAAImageSet(const SkCanvas::ImageSetEntry[], int count, const SkPoint dstClips[],
                            const SkMatrix[], const SkPaint&, SkCanvas::SrcRectConstraint) override;

    sk_sp<SkSpecialImage> makeSpecial(const SkBitmap&) override;
    sk_sp<SkSpecialImage> makeSpecial(const SkImage*) override;
    sk_sp<SkSpecialImage> snapSpecial(const SkIRect&, bool = false) override;

    void flush() override;
    GrSemaphoresSubmitted flush(SkSurface::BackendSurfaceAccess access, const GrFlushInfo&);
    bool wait(int numSemaphores, const GrBackendSemaphore* waitSemaphores);

    bool onAccessPixels(SkPixmap*) override;

    bool android_utils_clipWithStencil() override;

protected:
    bool onReadPixels(const SkPixmap&, int, int) override;
    bool onWritePixels(const SkPixmap&, int, int) override;

private:
    // We want these unreffed in RenderTargetContext, GrContext order.
    sk_sp<GrContext> fContext;
    std::unique_ptr<GrRenderTargetContext> fRenderTargetContext;

    enum Flags {
        kNeedClear_Flag = 1 << 0,  //!< Surface requires an initial clear
        kIsOpaque_Flag  = 1 << 1,  //!< Hint from client that rendering to this device will be
                                   //   opaque even if the config supports alpha.
    };
    static bool CheckAlphaTypeAndGetFlags(const SkImageInfo* info, InitContents init,
                                          unsigned* flags);

    SkGpuDevice(GrContext*, std::unique_ptr<GrRenderTargetContext>, unsigned flags);

    SkBaseDevice* onCreateDevice(const CreateInfo&, const SkPaint*) override;

    sk_sp<SkSurface> makeSurface(const SkImageInfo&, const SkSurfaceProps&) override;

    SkImageFilterCache* getImageFilterCache() override;

    bool forceConservativeRasterClip() const override { return true; }

    GrClipStackClip clip() const { return GrClipStackClip(&this->cs()); }

    sk_sp<SkSpecialImage> filterTexture(SkSpecialImage*,
                                        int left, int top,
                                        SkIPoint* offset,
                                        const SkImageFilter* filter);

    // If not null, dstClip must be contained inside dst and will also respect the edge AA flags.
    // If 'preViewMatrix' is not null, final CTM will be this->ctm() * preViewMatrix.
    void drawImageQuad(const SkImage*, const SkRect* src, const SkRect* dst,
                       const SkPoint dstClip[4], GrAA aa, GrQuadAAFlags aaFlags,
                       const SkMatrix* preViewMatrix, const SkPaint&, SkCanvas::SrcRectConstraint);

    // FIXME(michaelludwig) - Should be removed in favor of using drawImageQuad with edge flags to
    // for every element in the SkLatticeIter.
    void drawProducerLattice(GrTextureProducer*, std::unique_ptr<SkLatticeIter>, const SkRect& dst,
                             const SkPaint&);

    void drawStrokedLine(const SkPoint pts[2], const SkPaint&);

    void wireframeVertices(SkVertices::VertexMode, int vertexCount, const SkPoint verts[],
                           SkBlendMode, const uint16_t indices[], int indexCount, const SkPaint&);

    static std::unique_ptr<GrRenderTargetContext> MakeRenderTargetContext(GrContext*,
                                                                          SkBudgeted,
                                                                          const SkImageInfo&,
                                                                          int sampleCount,
                                                                          GrSurfaceOrigin,
                                                                          const SkSurfaceProps*,
                                                                          GrMipMapped);

    friend class GrAtlasTextContext;
    friend class SkSurface_Gpu;      // for access to surfaceProps
    typedef SkClipStackDevice INHERITED;
};

#endif
