#pragma once

#include <QByteArray>

// Minimal H.264 Annex-B inspection, shared by the two Android MediaCodec
// backends and — like MediaCodecLayout.h — kept free of the NDK so it
// compiles and is tested on a desktop host.
//
// Both backends need to look inside the bitstream, for reasons the Apple
// backends did not have:
//
//   * The ENCODER must know whether an access unit the codec just handed
//     it already carries its parameter sets, because MediaCodec delivers
//     SPS+PPS once, in a BUFFER_FLAG_CODEC_CONFIG buffer at the head of
//     the stream, and then emits bare IDRs — while some devices inline
//     them before every IDR instead. A receiver joining mid-call needs
//     self-contained keyframes; a receiver handed two copies of the same
//     SPS needs nothing, but it is still sloppy on the wire.
//
//   * The DECODER must pull SPS and PPS out to hand them to
//     AMediaCodec_configure as csd-0/csd-1. MediaCodec can be configured
//     from in-band parameter sets on most devices, but "most" is not a
//     contract, and the codec has to be configured with a size before it
//     will start at all. This mirrors MacVTDecoder::ensureSession, which
//     does exactly the same parse for the same reason.
//
// This is NOT a bitstream parser. It reads NAL headers and nothing else
// — no emulation-prevention unescaping, no exp-Golomb, no SPS fields.
// Everything above wants to know which NALs are present and where, and
// that is answerable from one byte each.

namespace annexb {

// What an access unit contains, by NAL type.
struct Summary {
    bool hasSps = false;   // type 7
    bool hasPps = false;   // type 8
    bool hasIdr = false;   // type 5 — an IDR slice, i.e. a real keyframe
    bool hasSlice = false; // type 1 or 5 — any coded picture data at all
};

Summary scan(const QByteArray& au);

// Copy out the LAST SPS and PPS in `au`, each including its 4-byte
// start code, which is the form MediaCodec's csd-0/csd-1 take.
//
// The last and not the first: a stream that changes resolution
// mid-session emits the new SPS ahead of the new IDR, and if both ended
// up in one access unit the newer one is the one that describes the
// picture that follows.
//
// Returns false (and leaves the outputs untouched) unless BOTH were
// found — a half-configured decoder is worse than no decoder.
bool parameterSets(const QByteArray& au, QByteArray& sps, QByteArray& pps);

} // namespace annexb
