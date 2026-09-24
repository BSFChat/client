#pragma once

#include <cstdint>

// I420 -> NV12 plane packing, split out of MacVTEncoder so it can be
// tested without a VideoToolbox session, an IOSurface or a camera.
//
// Why NV12 at all: every Apple hardware video encoder's native input is
// bi-planar 4:2:0 with interleaved chroma
// (kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange). Handing VideoToolbox
// the tri-planar kCVPixelFormatType_420YpCbCr8Planar this encoder used to
// allocate makes it insert its own conversion before the media engine —
// tolerable on a Mac, and on iOS the format may simply be refused. The
// pipeline upstream produces I420 (FrameConverter, libyuv), so the
// interleave has to happen somewhere; doing it here, straight into the
// pixel buffer the encoder handed us, keeps it to one pass with no
// intermediate allocation.
//
// Strides are honoured separately for source and destination because a
// CVPixelBuffer's rows are padded to the hardware's alignment (typically
// 64 bytes) and almost never match the frame width.

namespace nv12 {

// Pack tri-planar I420 into bi-planar NV12.
//
// `dstUV` receives width bytes per row (width/2 U,V pairs) over
// height/2 rows. Odd `width`/`height` are rounded DOWN to even — 4:2:0
// has no meaning otherwise — so the caller may pass the frame's real
// size without pre-flooring it.
//
// Returns false and writes nothing when any pointer is null, a stride is
// too small for the data it must hold, or the rounded size is empty.
bool packFromI420(const uint8_t* srcY, int srcStrideY,
                  const uint8_t* srcU, int srcStrideU,
                  const uint8_t* srcV, int srcStrideV,
                  uint8_t* dstY, int dstStrideY,
                  uint8_t* dstUV, int dstStrideUV,
                  int width, int height);

} // namespace nv12
