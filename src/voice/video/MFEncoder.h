#pragma once

#include "voice/video/VideoEncoder.h"

struct IMFTransform;
struct IMFMediaEventGenerator;
struct ICodecAPI;

// Media Foundation H.264 / HEVC encoder (Windows). Tries hardware
// encoder MFTs first (async, event-driven — NVENC/AMF/QuickSync surface
// through vendor MFTs), falling back to Microsoft's software encoder
// MFT (sync; present on every Windows 8+ install for H.264, and NOT
// present at all for HEVC unless the machine has a vendor MFT or the
// Store HEVC extension). Emits Annex-B access units.
//
// HEVC is therefore RUNTIME-detected, never assumed: see
// hevcEncodeSupported(). This class adds no link-time dependency for
// it — the HEVC subtype GUID is in the base SDK.
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

    // Runtime probe: is an HEVC encoder MFT registered on this machine?
    // Asked once and cached. False ⇒ H.265 is never offered or selected.
    static bool hevcEncodeSupported();

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
