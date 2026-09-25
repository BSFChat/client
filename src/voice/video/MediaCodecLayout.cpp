#include "voice/video/MediaCodecLayout.h"

#include <libyuv.h>

namespace mediacodec {
namespace {

// Chroma plane geometry for a resolved layout.
//
// MediaFormat reports ONE stride, the luma one. For the planar family
// the chroma planes are half-width, and stride/2 is the convention
// every COLOR_FormatYUV420Planar codec follows — there is no second key
// to ask, so a codec that aligned its chroma planes independently would
// have no way to say so. For the semiplanar family the single
// interleaved plane carries a full stride's worth of bytes per row,
// because it holds stride/2 U,V PAIRS.
struct ChromaGeometry {
    int strideU = 0;
    int strideV = 0;
    size_t offsetU = 0;
    size_t offsetV = 0;
};

ChromaGeometry chromaFor(const BufferLayout& l) {
    ChromaGeometry g;
    const size_t luma = size_t(l.stride) * size_t(l.sliceHeight);
    if (l.plane == BufferLayout::Plane::I420) {
        g.strideU = g.strideV = l.stride / 2;
        g.offsetU = luma;
        g.offsetV = luma + size_t(l.stride / 2) * size_t(l.sliceHeight / 2);
    } else {
        g.strideU = g.strideV = l.stride;   // interleaved UV
        g.offsetU = g.offsetV = luma;
    }
    return g;
}

} // namespace

BufferLayout layoutFor(int32_t colorFormat, int stride, int sliceHeight,
                       int width, int height) {
    BufferLayout out;
    const int w = width & ~1;
    const int h = height & ~1;
    if (w <= 0 || h <= 0) return out;

    switch (colorFormat) {
    case kFormatYUV420Planar:
    case kFormatYUV420PackedPlanar:
        out.plane = BufferLayout::Plane::I420;
        break;
    case kFormatYUV420SemiPlanar:
    case kFormatYUV420PackedSemiPlanar:
    case kFormatTiYUV420PackedSemiPlanar:
    case kFormatQcomYUV420SemiPlanar:
    case kFormatQcomYUV420PackedSemiPlanar32m:
        out.plane = BufferLayout::Plane::NV12;
        break;
    default:
        // Includes kFormatQcomYUV420PackedSemiPlanarTiled (a tiled
        // layout no linear copy can read), kFormatYUV420Flexible (the
        // codec never told us what it chose), every surface/opaque
        // format, and anything 10-bit. All of them are "do not touch
        // this buffer", which is what Unsupported means.
        return out;
    }

    // An absent key is 0 and means "no padding". A key SMALLER than the
    // picture is a codec reporting nonsense — honouring it would read
    // or write outside the frame — so it is treated the same way.
    out.stride = stride >= w ? stride : w;
    out.sliceHeight = sliceHeight >= h ? sliceHeight : h;
    // The planar family halves the stride for chroma, so an odd luma
    // stride would lose a column's worth of addressing. No codec
    // reports one; clamp rather than trust it.
    if (out.plane == BufferLayout::Plane::I420 && (out.stride & 1))
        ++out.stride;
    if (out.sliceHeight & 1) ++out.sliceHeight;
    return out;
}

bool packInput(const BufferLayout& layout,
               const uint8_t* srcY, int srcStrideY,
               const uint8_t* srcU, int srcStrideU,
               const uint8_t* srcV, int srcStrideV,
               uint8_t* dst, size_t dstCapacity,
               int width, int height) {
    if (!layout.isValid()) return false;
    const int w = width & ~1;
    const int h = height & ~1;
    if (w <= 0 || h <= 0) return false;
    if (!srcY || !srcU || !srcV || !dst) return false;
    // Negative strides are libyuv's bottom-up convention. Nothing here
    // needs them, and a caller passing one has almost certainly passed
    // a size where a stride belongs — refuse rather than silently flip
    // the image. (Same rule as NV12Pack::packFromI420.)
    if (srcStrideY < w || srcStrideU < w / 2 || srcStrideV < w / 2) return false;
    // The codec's own buffer must be at least as tall and wide as the
    // frame, or the layout was resolved against a different size.
    if (layout.stride < w || layout.sliceHeight < h) return false;
    if (dstCapacity < layout.sizeBytes()) return false;

    const ChromaGeometry g = chromaFor(layout);
    if (layout.plane == BufferLayout::Plane::NV12) {
        return libyuv::I420ToNV12(srcY, srcStrideY, srcU, srcStrideU,
                                  srcV, srcStrideV,
                                  dst, layout.stride,
                                  dst + g.offsetU, g.strideU,
                                  w, h) == 0;
    }
    return libyuv::I420Copy(srcY, srcStrideY, srcU, srcStrideU,
                            srcV, srcStrideV,
                            dst, layout.stride,
                            dst + g.offsetU, g.strideU,
                            dst + g.offsetV, g.strideV,
                            w, h) == 0;
}

bool unpackOutputToNV12(const BufferLayout& layout,
                        const uint8_t* src, size_t srcSize,
                        uint8_t* dstY, int dstStrideY,
                        uint8_t* dstUV, int dstStrideUV,
                        int width, int height) {
    if (!layout.isValid()) return false;
    const int w = width & ~1;
    const int h = height & ~1;
    if (w <= 0 || h <= 0) return false;
    if (!src || !dstY || !dstUV) return false;
    if (dstStrideY < w || dstStrideUV < w) return false;
    if (layout.stride < w || layout.sliceHeight < h) return false;
    if (srcSize < layout.sizeBytes()) return false;

    const ChromaGeometry g = chromaFor(layout);
    if (layout.plane == BufferLayout::Plane::I420) {
        return libyuv::I420ToNV12(src, layout.stride,
                                  src + g.offsetU, g.strideU,
                                  src + g.offsetV, g.strideV,
                                  dstY, dstStrideY, dstUV, dstStrideUV,
                                  w, h) == 0;
    }
    // Already NV12 — two plane copies that drop the codec's padding.
    // CopyPlane rather than one memcpy per row so libyuv's SIMD does the
    // work, and because a run where stride == width is the common case
    // it already special-cases into a single copy.
    libyuv::CopyPlane(src, layout.stride, dstY, dstStrideY, w, h);
    libyuv::CopyPlane(src + g.offsetU, g.strideU, dstUV, dstStrideUV, w, h / 2);
    return true;
}

} // namespace mediacodec
