#pragma once

#include "voice/video/VideoDecoder.h"

// VideoToolbox H.264 decoder (macOS). Parses SPS/PPS out of incoming
// Annex-B access units, (re)builds the decompression session whenever
// the parameter sets change (mid-stream resolution shifts), and emits
// NV12 QVideoFrames.
class MacVTDecoder : public VideoDecoder {
public:
    MacVTDecoder() = default;
    ~MacVTDecoder() override;

    bool init(VideoCodecKind kind) override;
    Result decode(const QByteArray& au, QVideoFrame& out) override;
    void reset() override;

    // Public: the C decompression callback (file-scope) fills this.
    struct OutputSlot;

private:
    void destroy();
    bool ensureSession(const QByteArray& sps, const QByteArray& pps);

    // Opaque VTDecompressionSessionRef / CMVideoFormatDescriptionRef.
    void* m_session = nullptr;
    void* m_format = nullptr;
    QByteArray m_sps, m_pps;   // active parameter sets
    OutputSlot* m_slot = nullptr;
};
