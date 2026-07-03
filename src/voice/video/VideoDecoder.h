#pragma once

#include "voice/video/VideoCodec.h"

#include <QStringList>
#include <QVideoFrame>
#include <memory>

// Abstract video decoder. Mirror of VideoEncoder — one instance per
// remote peer × stream, owned and driven by a VideoReceivePipeline
// worker thread (no internal locking needed).
class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;

    virtual bool init(VideoCodecKind kind) = 0;
    // Decode one access unit / temporal unit. Returns:
    //   Ok        — `out` holds a displayable frame
    //   NeedMore  — consumed, nothing to display (e.g. parameter sets)
    //   Error     — corrupt/undecodable; caller should flush to the
    //               next keyframe and request one from the sender
    enum class Result { Ok, NeedMore, Error };
    virtual Result decode(const QByteArray& au, QVideoFrame& out) = 0;
    // Drop all reference state — called after loss, before resuming at
    // the next keyframe. Mid-stream resolution changes must be handled
    // internally (all current backends re-derive size from the SPS /
    // sequence header).
    virtual void reset() = 0;

    static std::unique_ptr<VideoDecoder> create(VideoCodecKind kind,
                                                bool preferHardware = true);
    // H.264 profiles this platform can decode, as PeerCaps strings.
    static QStringList h264DecodeProfiles();
};
