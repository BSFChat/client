#pragma once

#include "voice/video/VideoEncoder.h"

struct IMFTransform;
struct IMFMediaEventGenerator;
struct ICodecAPI;

// Media Foundation H.264 encoder (Windows). Tries hardware encoder
// MFTs first (async, event-driven — NVENC/AMF/QuickSync surface
// through vendor MFTs), falling back to Microsoft's software H.264
// encoder MFT (sync, present on every Windows 8+ install). Emits
// Annex-B access units.
class MFEncoder : public VideoEncoder {
public:
    explicit MFEncoder(bool preferHardware = true);
    ~MFEncoder() override;

    bool init(const EncoderConfig& config) override;
    bool encode(const PlanarFrame& in, bool forceKeyframe,
                EncodedFrame& out) override;
    void setBitrate(int targetKbps, int maxKbps) override;
    bool reconfigure(const EncoderConfig& config) override;
    Caps caps() const override { return {m_isHardware, false, true}; }

private:
    void destroy();
    bool createTransform(bool hardware, const EncoderConfig& config);
    bool configureTypes(const EncoderConfig& config);
    bool drainOutput(EncodedFrame& out);

    bool m_preferHardware = true;
    bool m_isHardware = false;
    bool m_isAsync = false;
    IMFTransform* m_mft = nullptr;
    IMFMediaEventGenerator* m_events = nullptr;   // async MFTs only
    ICodecAPI* m_codecApi = nullptr;
    EncoderConfig m_config;
    // Reusable NV12 staging buffer (I420 → NV12 interleave).
    QByteArray m_nv12;
};
