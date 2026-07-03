#pragma once

#include "voice/video/VideoCodec.h"

#include <QStringList>
#include <memory>

// Abstract video encoder. Implementations: OpenH264Encoder (software,
// Linux default + universal fallback), MacVTEncoder (VideoToolbox),
// MFEncoder (Media Foundation), AomLosslessEncoder (AV1 lossless).
//
// Threading: an encoder instance is owned and driven exclusively by
// one VideoSendPipeline worker thread — implementations need no
// internal locking.
class VideoEncoder {
public:
    struct Caps {
        bool hardware = false;
        bool losslessSupported = false;
        bool highProfile = false;   // can emit H.264 High
    };

    virtual ~VideoEncoder() = default;

    virtual bool init(const EncoderConfig& config) = 0;
    // Encode one frame. `out` is only valid when true is returned —
    // encoders may legitimately buffer nothing here (all current
    // backends are zero-delay, one-in-one-out).
    virtual bool encode(const PlanarFrame& in, bool forceKeyframe,
                        EncodedFrame& out) = 0;
    // Live bitrate change, no session rebuild — the adaptive
    // controller's hot path.
    virtual void setBitrate(int targetKbps, int maxKbps) = 0;
    // Resolution/fps/profile change — may rebuild the session
    // internally; the next output frame must be a keyframe.
    virtual bool reconfigure(const EncoderConfig& config) = 0;
    virtual Caps caps() const = 0;

    // Best available backend for `kind` on this platform, hardware
    // preferred (with automatic software fallback if HW init fails —
    // callers should retry create(kind, false) when init() fails).
    static std::unique_ptr<VideoEncoder> create(VideoCodecKind kind,
                                                bool preferHardware = true);
    // Capability probe without instantiating a session — feeds the
    // PeerCaps advertisement.
    static Caps queryCaps(VideoCodecKind kind);
    // H.264 profiles this platform can emit, as PeerCaps strings
    // ("cb", "high"), best first.
    static QStringList h264EncodeProfiles();
};
