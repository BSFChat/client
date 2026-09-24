#include "voice/video/NV12Pack.h"

#include <libyuv.h>

namespace nv12 {

bool packFromI420(const uint8_t* srcY, int srcStrideY,
                  const uint8_t* srcU, int srcStrideU,
                  const uint8_t* srcV, int srcStrideV,
                  uint8_t* dstY, int dstStrideY,
                  uint8_t* dstUV, int dstStrideUV,
                  int width, int height) {
    const int w = width & ~1;
    const int h = height & ~1;
    if (w <= 0 || h <= 0) return false;
    if (!srcY || !srcU || !srcV || !dstY || !dstUV) return false;
    // Negative strides are libyuv's bottom-up convention. This packer
    // never needs them and a caller passing one has almost certainly
    // passed a size where a stride belongs, so refuse rather than
    // silently flip the image.
    if (srcStrideY < w || srcStrideU < w / 2 || srcStrideV < w / 2) return false;
    // The UV plane is interleaved, so it carries w bytes per row even
    // though it only covers w/2 chroma columns.
    if (dstStrideY < w || dstStrideUV < w) return false;

    return libyuv::I420ToNV12(srcY, srcStrideY, srcU, srcStrideU,
                              srcV, srcStrideV,
                              dstY, dstStrideY, dstUV, dstStrideUV,
                              w, h) == 0;
}

} // namespace nv12
