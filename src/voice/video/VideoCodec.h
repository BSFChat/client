#pragma once

#include <QByteArray>
#include <QtGlobal>

// Shared types for the real-video pipeline (RTP H.264 + AV1 lossless).
// See the migration plan: encoders/decoders are pluggable per-platform
// backends behind VideoEncoder/VideoDecoder; everything upstream of
// them speaks these codec-neutral structs.

// Stream identity inside a call. Doubles as the array index for the
// per-peer track contexts and as the on-wire streamId byte in the
// 0x05 lossless framing.
enum class VideoStreamId : quint8 {
    Screen = 0,
    Camera = 1,
};
inline constexpr int kVideoStreamCount = 2;

enum class VideoCodecKind {
    H264,
    H265,
    Av1Lossless,
};

// The wire/"caps" identifier for a codec, and the inverse. These are
// the strings that appear in `bsfchat_caps.video_codecs`, in the video
// diagnostics overlay, and in the SDP rtpmap (upper-cased there by
// libdatachannel). Kept next to the enum so a new codec cannot be
// added without naming it.
inline const char* videoCodecName(VideoCodecKind k) {
    switch (k) {
    case VideoCodecKind::H264:        return "h264";
    case VideoCodecKind::H265:        return "h265";
    case VideoCodecKind::Av1Lossless: return "av1-lossless";
    }
    return "h264";
}

// True for the codecs that ride RTP video tracks (as opposed to the
// AV1 lossless tier, which rides a reliable data channel).
inline bool isRtpVideoCodec(VideoCodecKind k) {
    return k == VideoCodecKind::H264 || k == VideoCodecKind::H265;
}

// H.264 bitstream profile actually emitted — negotiated app-level via
// PeerCaps (the SDP always advertises Constrained Baseline for
// compatibility; see the capability-handshake design note there).
enum class H264Profile {
    ConstrainedBaseline,
    High,
};

struct EncoderConfig {
    VideoCodecKind codec = VideoCodecKind::H264;
    // Encode dimensions (already scaled/evened by the pipeline).
    int width = 1280;
    int height = 720;
    int fps = 30;
    int targetBitrateKbps = 4000;
    int maxBitrateKbps = 8000;
    // Long GOP on purpose: an IDR costs 10-20× a P-frame, so frequent
    // periodic keyframes pulse the quality and waste budget that
    // could sharpen screen content. Recovery does not rely on this
    // cadence — receivers explicitly request an IDR (PLI /
    // keyframeNeeded) on loss or late join, and new encode sessions
    // always open on one. This is only the background refresh.
    int keyframeIntervalSec = 10;
    bool lossless = false;               // implies codec == Av1Lossless
    H264Profile profile = H264Profile::ConstrainedBaseline;
    // Screen content favors sharpness/text (encoders have dedicated
    // tuning for it); camera content favors motion.
    bool screenContent = true;

    bool sameSessionAs(const EncoderConfig& o) const {
        // Differences that force an encoder session rebuild — bitrate
        // changes deliberately excluded (applied live via setBitrate).
        return codec == o.codec && width == o.width && height == o.height
            && fps == o.fps && lossless == o.lossless
            && profile == o.profile && screenContent == o.screenContent
            && keyframeIntervalSec == o.keyframeIntervalSec;
    }
};

struct EncodedFrame {
    QByteArray data;        // H264/H265: Annex-B access unit (long start codes)
                            // AV1: complete temporal unit
    VideoCodecKind codec = VideoCodecKind::H264;
    bool keyframe = false;
    qint64 captureTimeUs = 0;
    int width = 0;
    int height = 0;
};

// CPU I420 (or I444 for lossless) planar frame handed to encoders —
// FrameConverter produces these from QVideoFrames.
struct PlanarFrame {
    enum class Layout { I420, I444Identity };
    Layout layout = Layout::I420;
    QByteArray y, u, v;
    int width = 0;
    int height = 0;
    int strideY = 0;
    int strideU = 0;
    int strideV = 0;
    qint64 captureTimeUs = 0;

    bool isValid() const { return width > 0 && height > 0 && !y.isEmpty(); }
};
