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

    // Frames this encoder has ACCEPTED but whose access units it has
    // not returned yet.
    //
    // Zero for every backend but Android's: VideoToolbox, Media
    // Foundation and openh264 are all driven one-in-one-out, so a false
    // from encode() there means the frame is gone. MediaCodec is a
    // queue in and a queue out and legitimately holds one to three
    // frames, so a false from IT can mean "accepted, not ready" — which
    // is not the same failure and must not be read as one.
    //
    // This matters because the read feeds back: VideoSendPipeline's
    // `encoded` counter drives videosend::Window::sentFps, sentFps
    // drives the Encode bottleneck, and the controller's remedy for
    // Encode is to shed resolution — which rebuilds the session, which
    // primes the pipeline again, which produces more not-ready returns.
    // A pipelined encoder that cannot say "I am holding it" is a
    // self-feeding downward spiral.
    //
    // Reported rather than acted on for now: on the device run that
    // exposed this, the repeated rebuilds came from a different lie
    // (the camera reporting no capture rate at all — see
    // CameraController's send-window source), and fixing that removes
    // the rebuilds that make priming visible. This is the seam and the
    // instrumentation for settling the remainder on hardware instead of
    // by argument.
    virtual int framesInFlight() const { return 0; }

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
    // Can this build, on THIS machine, encode H.265? Runtime-detected
    // (VideoToolbox media engine / registered MF encoder MFT) and
    // cached. Purely local knowledge — it is never advertised to
    // peers, who only care what we can send them, not what we could.
    static bool h265EncodeSupported();
};
