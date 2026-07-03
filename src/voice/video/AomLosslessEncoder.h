#pragma once

#include "voice/video/VideoEncoder.h"

#include <memory>

struct AomEncoderState;

// libaom AV1 encoder locked to mathematically-lossless mode:
// AV1E_SET_LOSSLESS with identity-matrix full-range I444 input, so the
// decoded output reproduces the source RGB bit-exactly. Bitrate is
// whatever the content costs — the transport (reliable data channel)
// applies admission control instead of rate control.
class AomLosslessEncoder : public VideoEncoder {
public:
    AomLosslessEncoder();
    ~AomLosslessEncoder() override;

    bool init(const EncoderConfig& config) override;
    bool encode(const PlanarFrame& in, bool forceKeyframe,
                EncodedFrame& out) override;
    void setBitrate(int targetKbps, int maxKbps) override;   // no-op
    bool reconfigure(const EncoderConfig& config) override;
    Caps caps() const override { return {false, true, false}; }

private:
    std::unique_ptr<AomEncoderState> m_state;
    EncoderConfig m_config;
};
