#pragma once

#include "voice/video/VideoEncoder.h"

// VideoToolbox H.264 encoder (macOS). Hardware-accelerated on every
// Apple Silicon / recent Intel Mac; emits High profile (progressive,
// no B-frames — AllowFrameReordering off keeps latency flat and the
// bitstream decodable by openh264 peers).
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
