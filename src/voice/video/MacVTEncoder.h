#pragma once

#include "voice/video/VideoEncoder.h"

// VideoToolbox H.264 / HEVC encoder — macOS AND iOS. Hardware-accelerated
// on every Apple Silicon / recent Intel Mac and on every supported
// iPhone. H.264 emits High profile; HEVC emits Main. Both are progressive
// with no B-frames — AllowFrameReordering off keeps latency flat and the
// H.264 bitstream decodable by openh264 peers.
//
// One class for both codecs rather than two: everything except the
// codec type, the profile-level constant and which parameter-set
// accessor the callback uses is identical, and a second class would be
// 200 lines of copy that could drift.
//
// The same file serves both Apple platforms for the same reason. The two
// differences are contained and named where they occur:
//
//   1. The hardware-acceleration encoder specification is macOS-only
//      (see BSFCHAT_VT_WANT_HW_SPEC in the .mm). On iOS there is no
//      software encoder to ask it not to use.
//   2. iOS tears the compression session down when the app is
//      backgrounded, and every later call returns kVTInvalidSessionErr.
//      encode() rebuilds and retries once rather than going dark for the
//      rest of the call — see the note there. macOS never produces this,
//      so on macOS the branch is dead code that costs one comparison.
//
// Keeping iOS on the same code path as the Mac is deliberate: the Mac is
// where this gets exercised daily, and a separate iOS encoder would be a
// second implementation tested only by a person holding a phone.
class MacVTEncoder : public VideoEncoder {
public:
    MacVTEncoder() = default;
    ~MacVTEncoder() override;

    bool init(const EncoderConfig& config) override;
    bool encode(const PlanarFrame& in, bool forceKeyframe,
                EncodedFrame& out) override;
    void setBitrate(int targetKbps, int maxKbps) override;
    bool reconfigure(const EncoderConfig& config) override;
    Caps caps() const override { return {true, false, true}; }

    // Runtime probe: can this Mac ENCODE HEVC? Asked once and cached.
    // Every Apple Silicon Mac can; older Intel Macs without a suitable
    // media engine cannot, and there the answer must be "no" rather
    // than a session that fails at share time.
    static bool hevcEncodeSupported();

    // Public: the C compression callback (file-scope, not a member)
    // fills this slot.
    struct CallbackSlot;

private:
    void destroy();

    // Take an NV12 CVPixelBuffer from the session's own pool (or, if it
    // has none, allocate one IOSurface-backed) and fill it from `in`.
    // Returns an opaque CVPixelBufferRef the caller must release, or
    // nullptr. Declared here rather than file-static because it needs
    // m_session.
    void* acquirePixelBuffer(const PlanarFrame& in);

    // One submit/collect cycle. `outStatus` receives the OSStatus so
    // encode() can tell "the session died" apart from "the frame was
    // bad", which are the same `false` from the caller's point of view
    // but need opposite responses.
    bool encodeAttempt(const PlanarFrame& in, bool forceKeyframe,
                       EncodedFrame& out, int* outStatus);

    // Opaque VTCompressionSessionRef (kept void* so this header stays
    // includable from non-ObjC TUs, mirroring MacScreenCapturer).
    void* m_session = nullptr;
    EncoderConfig m_config;
    // Set by the compression callback for the frame being encoded.
    CallbackSlot* m_slot = nullptr;
};
