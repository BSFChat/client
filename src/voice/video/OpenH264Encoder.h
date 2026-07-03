#pragma once

#include "voice/video/VideoEncoder.h"

class ISVCEncoder;

// Software H.264 encoder over Cisco openh264 (BSD). The Linux default
// and the universal fallback when a platform encoder fails to init.
// openh264's encoder emits Constrained Baseline only — the caps probe
// reports highProfile=false and the profile intersection logic keeps
// peers' decode expectations aligned.
class OpenH264Encoder : public VideoEncoder {
public:
    OpenH264Encoder() = default;
    ~OpenH264Encoder() override;

    bool init(const EncoderConfig& config) override;
    bool encode(const PlanarFrame& in, bool forceKeyframe,
                EncodedFrame& out) override;
    void setBitrate(int targetKbps, int maxKbps) override;
    bool reconfigure(const EncoderConfig& config) override;
    Caps caps() const override { return {false, false, false}; }

private:
    void destroy();

    ISVCEncoder* m_encoder = nullptr;
    EncoderConfig m_config;
};
