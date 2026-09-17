#pragma once

#include "voice/video/VideoDecoder.h"

// VideoToolbox H.264 / HEVC decoder (macOS). Parses the parameter sets
// out of incoming Annex-B access units (SPS+PPS for H.264, VPS+SPS+PPS
// for HEVC), (re)builds the decompression session whenever they change
// (mid-stream resolution shifts), and emits NV12 QVideoFrames.
//
// One instance decodes ONE codec, fixed by init(): a sender that
// switches codec mid-call changes payload type, and the receive
// pipeline builds a fresh decoder for the new one rather than trying
// to re-point this session.
class MacVTDecoder : public VideoDecoder {
public:
    MacVTDecoder() = default;
    ~MacVTDecoder() override;

    bool init(VideoCodecKind kind) override;
    Result decode(const QByteArray& au, QVideoFrame& out) override;
    void reset() override;

    // Runtime probe: can this Mac DECODE HEVC? Asked once and cached.
    // This is what gates advertising "h265" in bsfchat_caps, so a
    // wrong answer here is a stream a viewer cannot watch.
    static bool hevcDecodeSupported();

    // Public: the C decompression callback (file-scope) fills this.
    struct OutputSlot;

private:
    void destroy();
    bool ensureSession(const QByteArray& vps, const QByteArray& sps,
                       const QByteArray& pps);

    // Opaque VTDecompressionSessionRef / CMVideoFormatDescriptionRef.
    void* m_session = nullptr;
    void* m_format = nullptr;
    VideoCodecKind m_kind = VideoCodecKind::H264;
    // Active parameter sets. m_vps stays empty for H.264.
    QByteArray m_vps, m_sps, m_pps;
    OutputSlot* m_slot = nullptr;
};
