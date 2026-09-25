#pragma once

#include <cstddef>
#include <cstdint>

// The part of the Android MediaCodec backends that is pure arithmetic
// and memory, split out of MediaCodecEncoder/MediaCodecDecoder exactly
// as NV12Pack.h was split out of MacVTEncoder — so it can be tested on
// a desktop host, with no NDK, no device and no codec.
//
// It is also the part most likely to be wrong. Everything else in those
// two files is "call the API in the documented order"; this is where a
// stride is confused with a width and the picture gets a green band
// down one side, or a diagonal shear, for a month before anyone says
// anything.
//
// ---------------------------------------------------------------------
// Why there is anything to resolve at all
//
// MediaCodec does not have ONE raw video buffer layout. It has a per-
// device set, advertised as `color-format` integers from
// android.media.MediaCodecInfo.CodecCapabilities, and the two families
// that matter for 8-bit 4:2:0 are:
//
//   * planar    (I420): Y plane, then a half-width U plane, then a
//                       half-width V plane — what FrameConverter already
//                       produces.
//   * semiplanar (NV12): Y plane, then ONE plane of interleaved U,V
//                       pairs at full stride and half height — what most
//                       hardware actually wants, and what the Apple
//                       encoders take (see NV12Pack.h, same reasoning).
//
// COLOR_FormatYUV420Flexible (0x7F420888) is the portable ASK: every
// codec since API 21 accepts it at configure() time. It is not an
// answer, though — it means "you pick", and the codec then reports the
// concrete format it picked in the format returned by
// AMediaCodec_getInputFormat() / getOutputFormat(). Resolving that
// readback is what layoutFor() is for.
//
// ---------------------------------------------------------------------
// Why stride and sliceHeight are separate from width and height
//
// A codec buffer's rows are padded to the hardware's alignment, and the
// chroma planes start at a row offset that is padded independently:
//
//   stride      — bytes between the start of one luma row and the next.
//                 >= width, typically aligned to 16, 32, 64 or 128.
//   sliceHeight — luma ROWS between the start of the Y plane and the
//                 start of the first chroma plane. >= height. 1080 is
//                 routinely 1088 here, which is the single most common
//                 way to get a garbage bottom eighth of the picture.
//
// Both come from the MediaFormat when the codec sets them
// (AMEDIAFORMAT_KEY_STRIDE / AMEDIAFORMAT_KEY_SLICE_HEIGHT) and are
// defaulted to width/height when it does not, which is what every
// codec that omits them means.

namespace mediacodec {

// android.media.MediaCodecInfo.CodecCapabilities constants. Named here
// rather than used as bare integers because the vendor ones are
// unreadable and their membership of a family is the whole point.
enum ColorFormat : int32_t {
    // Standard OMX values.
    kFormatYUV420Planar = 19,                 // 0x13, I420
    kFormatYUV420PackedPlanar = 20,           // 0x14, I420 with no inter-plane gap
    kFormatYUV420SemiPlanar = 21,             // 0x15, NV12
    kFormatYUV420PackedSemiPlanar = 39,       // 0x27, NV12 with no gap
    // Vendor semiplanar variants that are plain NV12 in memory.
    kFormatTiYUV420PackedSemiPlanar = 0x7F000100,
    kFormatQcomYUV420SemiPlanar = 0x7FA30C00,
    kFormatQcomYUV420PackedSemiPlanar32m = 0x7FA30C04,
    // Qualcomm's 64x32 TILED layout. Listed so it is REFUSED by name
    // rather than mistaken for the semiplanar values above that it sits
    // between: its macroblocks are interleaved in a space-filling curve
    // and a linear row copy of it produces confetti, not a picture.
    kFormatQcomYUV420PackedSemiPlanarTiled = 0x7FA30C03,
    // "You choose" — what we ask for, never what we should act on.
    kFormatYUV420Flexible = 0x7F420888,
};

// A resolved MediaCodec raw-video buffer layout.
struct BufferLayout {
    enum class Plane {
        Unsupported,  // tiled, opaque, or a format not in either family
        I420,         // Y, U, V — three planes
        NV12,         // Y, then interleaved UV — two planes
    };
    Plane plane = Plane::Unsupported;
    int stride = 0;       // bytes per luma row
    int sliceHeight = 0;  // luma rows before the chroma planes begin
    // Bytes the codec buffer must hold for `plane` at this stride and
    // slice height. Both families come to the same number: the Y plane
    // is stride*sliceHeight and the chroma is exactly half of it.
    size_t sizeBytes() const {
        if (plane == Plane::Unsupported) return 0;
        return size_t(stride) * size_t(sliceHeight) * 3 / 2;
    }
    bool isValid() const { return plane != Plane::Unsupported; }
};

// Turn a MediaFormat readback into a layout.
//
// `stride` and `sliceHeight` are what AMEDIAFORMAT_KEY_STRIDE and
// AMEDIAFORMAT_KEY_SLICE_HEIGHT gave, or 0 when the codec did not set
// them — in which case they default to `width` and `height`, which is
// what their absence means. A value SMALLER than the frame is a codec
// reporting nonsense and is also treated as absent, because honouring
// it would read or write outside the picture.
//
// `colorFormat` of kFormatYUV420Flexible means the codec never resolved
// its choice; that is not something to guess at, so it returns
// Unsupported and the caller retries configure() with a concrete format.
BufferLayout layoutFor(int32_t colorFormat, int stride, int sliceHeight,
                       int width, int height);

// Tightly-packed I420 (what FrameConverter produces) -> a codec INPUT
// buffer in `layout`. Writes `layout.sizeBytes()` bytes; refuses if
// `dstCapacity` is smaller, if any pointer is null, or if a source
// stride cannot hold its row.
//
// Odd width/height are rounded DOWN to even, as in NV12Pack: 4:2:0 has
// no meaning otherwise, and the caller may pass the frame's real size.
bool packInput(const BufferLayout& layout,
               const uint8_t* srcY, int srcStrideY,
               const uint8_t* srcU, int srcStrideU,
               const uint8_t* srcV, int srcStrideV,
               uint8_t* dst, size_t dstCapacity,
               int width, int height);

// A codec OUTPUT buffer in `layout` -> NV12 planes.
//
// NV12 and not I420 because that is what MacVTDecoder already hands
// QVideoFrame (Format_NV12) and therefore what the QML/QVideoSink
// playout path is known to render. One output pixel format across the
// backends keeps the receive side from growing a per-platform branch.
//
// `dstUV` receives `width` bytes per row over height/2 rows.
bool unpackOutputToNV12(const BufferLayout& layout,
                        const uint8_t* src, size_t srcSize,
                        uint8_t* dstY, int dstStrideY,
                        uint8_t* dstUV, int dstStrideUV,
                        int width, int height);

} // namespace mediacodec
