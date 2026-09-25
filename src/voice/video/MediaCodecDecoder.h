#pragma once

#include "voice/video/MediaCodecLayout.h"
#include "voice/video/VideoDecoder.h"

#include <QByteArray>
#include <QVideoFrame>

#include <deque>

// Android hardware H.264 decoder, on the NDK's AMediaCodec. Mirror of
// MediaCodecEncoder — see that header for why the NDK binding and not
// JNI, and for the pipeline-depth argument, which applies here too:
// MediaCodec decoders hold frames, so the first few access units of a
// stream produce Result::NeedMore before output starts flowing. That is
// already what NeedMore means in VideoDecoder's contract ("consumed,
// nothing to display"), so nothing upstream needs to know.
//
// Structurally this is MacVTDecoder with the platform swapped: parse the
// parameter sets out of the incoming Annex-B, (re)build the session
// whenever they change so a mid-stream resolution shift is handled, and
// emit NV12 QVideoFrames — the same pixel format the VideoToolbox
// decoder emits, so the QVideoSink playout path needs no Android branch.
//
// One instance decodes ONE codec, fixed by init(). A sender that
// switches codec mid-call changes payload type and the receive pipeline
// builds a fresh decoder.
class MediaCodecDecoder : public VideoDecoder {
public:
    MediaCodecDecoder() = default;
    ~MediaCodecDecoder() override;

    bool init(VideoCodecKind kind) override;
    Result decode(const QByteArray& au, QVideoFrame& out) override;
    void reset() override;

    // Is there an H.264 decoder on this device? Asked once and cached.
    // Gates what this build advertises, so a wrong answer is a stream
    // the user cannot watch.
    static bool h264DecodeSupported();

private:
    void destroy();
    // Build the codec from a complete SPS+PPS pair. No-op when the
    // session is already up on the same parameter sets.
    bool ensureSession(const QByteArray& sps, const QByteArray& pps);
    // Move every ready output buffer into m_ready. Returns false on a
    // codec error.
    bool drainOutput(int64_t blockUs);

    void* m_codec = nullptr;            // opaque AMediaCodec*
    // Active parameter sets, start codes included (csd-0 / csd-1 form).
    QByteArray m_sps, m_pps;
    // Resolved from the output format on the first
    // INFO_OUTPUT_FORMAT_CHANGED, and again on every later one.
    mediacodec::BufferLayout m_outputLayout;
    int m_outWidth = 0;
    int m_outHeight = 0;
    std::deque<QVideoFrame> m_ready;
    // Synthetic, per-instance, monotonically increasing input PTS — see
    // the note at the queueInputBuffer call.
    uint64_t m_ptsUs = 0;
};
