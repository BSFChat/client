#pragma once

#include "voice/video/VideoCodec.h"
#include "voice/video/VideoDecoder.h"

struct IMFTransform;

// Media Foundation H.264 / HEVC decoder (Windows). H.264 uses
// Microsoft's software decoder MFT (sync, present on every supported
// Windows; DXVA acceleration happens inside the MFT where available).
// HEVC has no guaranteed MFT and is RUNTIME-detected — see
// hevcDecodeSupported(); on a machine without a workable one this class
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

    // Runtime probe: can this machine actually DECODE HEVC? Asked once
    // and cached. It activates a decoder MFT, sets a described input
    // type and negotiates NV12 output — every step init() takes —
    // because "an HEVC MFT is registered" is a different, weaker claim
    // that once cost two Macs their screen shares: the Windows peer
    // advertised h265 on the strength of enumeration alone, both
    // senders flipped to it, and its decoder then failed to init and
    // painted black tiles for the whole call.
    //
    // This is what gates advertising "h265" in bsfchat_caps, so a false
    // positive here is unwatchable video for everyone in the mesh.
    static bool hevcDecodeSupported();

private:
    void destroy();
    // Select the decoder's NV12 output type and adopt the coded size it
    // reports. `logFailure` is false for the speculative attempts
    // decode() makes while a decoder has yet to describe its output, so
    // a 30-access-unit wait cannot become 30 warnings.
    bool negotiateOutputType(bool logFailure = true);
    // Latch a decoder that cannot be driven, logging once. Without it a
    // broken decoder writes one warning per access unit — 17,000 of
    // them in the incident this class was fixed for.
    void markUnusable(const char* why, long hr);

    IMFTransform* m_mft = nullptr;
    VideoCodecKind m_kind = VideoCodecKind::H264;
    int m_width = 0;
    int m_height = 0;
    int m_stride = 0;              // NV12 source stride of the MFT's output
    bool m_outputTypeSet = false;
    bool m_unusable = false;       // latched; stops the log flood
    int m_deferredNegotiations = 0;
    // Last negotiation result, kept so the latch message can name it.
    // `long` rather than HRESULT so this header stays free of windows.h.
    long m_lastNegotiateHr = 0;
    const char* m_lastNegotiateStep = nullptr;  // static strings only
};
