#pragma once

#include "voice/video/VideoDecoder.h"

#include <memory>

struct AomDecoderState;

// libaom AV1 decoder for the lossless tier. Expects identity-matrix
// full-range I444 temporal units and reconstructs BGRA frames that
// match the sender's source RGB bit-exactly.
class AomLosslessDecoder : public VideoDecoder {
public:
    AomLosslessDecoder();
    ~AomLosslessDecoder() override;

    bool init(VideoCodecKind kind) override;
    Result decode(const QByteArray& tu, QVideoFrame& out) override;
    void reset() override;

private:
    std::unique_ptr<AomDecoderState> m_state;
};
