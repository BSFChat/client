#pragma once

#include "voice/video/VideoDecoder.h"

struct IMFTransform;

// Media Foundation H.264 decoder (Windows) — Microsoft's software
// decoder MFT (sync, present on every supported Windows; DXVA
// acceleration happens inside the MFT where available). Consumes
// Annex-B access units, emits NV12 QVideoFrames, and renegotiates its
// output type on mid-stream resolution changes.
class MFDecoder : public VideoDecoder {
public:
    MFDecoder() = default;
    ~MFDecoder() override;

    bool init(VideoCodecKind kind) override;
    Result decode(const QByteArray& au, QVideoFrame& out) override;
    void reset() override;

private:
    void destroy();
    bool negotiateOutputType();

    IMFTransform* m_mft = nullptr;
    int m_width = 0;
    int m_height = 0;
};
