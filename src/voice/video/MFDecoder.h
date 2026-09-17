#pragma once

#include "voice/video/VideoDecoder.h"

struct IMFTransform;

// Media Foundation H.264 / HEVC decoder (Windows). H.264 uses
// Microsoft's software decoder MFT (sync, present on every supported
// Windows; DXVA acceleration happens inside the MFT where available).
// HEVC has no guaranteed MFT and is RUNTIME-detected by enumeration —
// see hevcDecodeSupported(); on a machine without one this class
// refuses to init and the build never advertises h265 decode.
//
// Consumes Annex-B access units, emits NV12 QVideoFrames, and
// renegotiates its output type on mid-stream resolution changes.
class MFDecoder : public VideoDecoder {
public:
    MFDecoder() = default;
    ~MFDecoder() override;

    bool init(VideoCodecKind kind) override;
    Result decode(const QByteArray& au, QVideoFrame& out) override;
    void reset() override;

    // Runtime probe: is an HEVC decoder MFT registered on this machine?
    // Asked once and cached. This is what gates advertising "h265" in
    // bsfchat_caps on Windows.
    static bool hevcDecodeSupported();

private:
    void destroy();
    bool negotiateOutputType();

    IMFTransform* m_mft = nullptr;
    int m_width = 0;
    int m_height = 0;
};
