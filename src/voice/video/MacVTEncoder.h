#pragma once

#include "voice/video/VideoEncoder.h"

// VideoToolbox H.264 / HEVC encoder (macOS). Hardware-accelerated on
// every Apple Silicon / recent Intel Mac. H.264 emits High profile;
// HEVC emits Main. Both are progressive with no B-frames —
// AllowFrameReordering off keeps latency flat and the H.264 bitstream
// decodable by openh264 peers.
//
// One class for both codecs rather than two: everything except the
// codec type, the profile-level constant and which parameter-set
// accessor the callback uses is identical, and a second class would be
// 200 lines of copy that could drift.
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

    // Opaque VTCompressionSessionRef (kept void* so this header stays
    // includable from non-ObjC TUs, mirroring MacScreenCapturer).
    void* m_session = nullptr;
    EncoderConfig m_config;
    // Set by the compression callback for the frame being encoded.
    CallbackSlot* m_slot = nullptr;
};
