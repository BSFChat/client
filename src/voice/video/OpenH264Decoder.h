#pragma once

#include "voice/video/VideoDecoder.h"

class ISVCDecoder;

// Software H.264 decoder over Cisco openh264 (BSD). Decodes CB/Main/
// High progressive — the full profile set our encoders emit.
class OpenH264Decoder : public VideoDecoder {
public:
    OpenH264Decoder() = default;
    ~OpenH264Decoder() override;

    bool init(VideoCodecKind kind) override;
    Result decode(const QByteArray& au, QVideoFrame& out) override;
    void reset() override;

private:
    void destroy();

    ISVCDecoder* m_decoder = nullptr;
};
