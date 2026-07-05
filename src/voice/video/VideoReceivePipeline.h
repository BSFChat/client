#pragma once

#include "voice/video/VideoCodec.h"

#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QThread>
#include <QVideoFrame>

#include <atomic>
#include <memory>

class VideoDecoder;

// Per-peer × per-stream decode worker. Access units enter through
// submitAccessUnit() (main thread, fed by the track's depacketizer
// output) into a small bounded queue; the worker decodes in order and
// emits displayable QVideoFrames.
//
// "Always smooth" on the receive side means never letting decode lag
// accumulate: when the queue overflows (slow decode / CPU spike) the
// whole backlog is dropped and the stream re-enters at the next
// keyframe, which keyframeNeeded() asks the sender to produce. Same
// recovery path for decode errors after packet loss.
class VideoReceivePipeline : public QObject {
    Q_OBJECT
public:
    VideoReceivePipeline(const QString& userId, VideoStreamId streamId,
                         VideoCodecKind codec, QObject* parent = nullptr);
    ~VideoReceivePipeline() override;

    VideoCodecKind codec() const { return m_codec; }

    // Thread-safe. `keyframeHint` marks the AU as a keyframe when the
    // transport knows (lossless framing carries a flag; H.264 AUs are
    // scanned for IDR NALs instead and can pass false).
    // `lossSuspected` marks the AU as likely incomplete (RTP sequence
    // gap observed while it was being reassembled) — it is dropped
    // without decoding and the stream re-enters at the next keyframe,
    // because decoders (MF especially) error-conceal broken references
    // and report success, which paints smearing corruption on screen.
    void submitAccessUnit(const QByteArray& au, bool keyframeHint = false,
                          bool lossSuspected = false);

    // Cumulative receive counters, read by VoiceEngine's 500 ms
    // receiver-report tick and sent to the remote sender, which
    // derives delivery loss for its rate controller.
    quint64 rxFrames() const { return m_rxFrames.load(); }
    quint64 rxBytes() const { return m_rxBytes.load(); }

    // Diagnostics counters for the video stats overlay. Cumulative;
    // the UI polls and diffs successive snapshots for rates.
    quint64 decodedFrames() const { return m_decodedFrames.load(); }
    quint64 droppedAus() const { return m_droppedAus.load(); }
    int frameWidth() const { return m_lastWidth.load(); }
    int frameHeight() const { return m_lastHeight.load(); }

signals:
    // Both emitted from the worker thread (queued to receivers).
    void frameDecoded(const QString& userId, int streamId,
                      const QVideoFrame& frame);
    void keyframeNeeded(const QString& userId, int streamId);

private:
    void drainQueue();   // worker thread
    // Emit keyframeNeeded at most once per kKfRequestMinIntervalMs.
    // Under sustained loss every gap would otherwise fire a request,
    // and the sender counts request bursts as a congestion signal —
    // spam would keep it in permanent hard back-off. Thread-safe.
    void requestKeyframeThrottled();

    const QString m_userId;
    const VideoStreamId m_streamId;
    const VideoCodecKind m_codec;

    QThread m_thread;
    QObject m_worker;

    QMutex m_mutex;
    struct QueuedAu { QByteArray data; bool keyframe = false; bool loss = false; };
    QList<QueuedAu> m_queue;
    std::atomic<bool> m_drainQueued{false};
    std::atomic<quint64> m_rxFrames{0};
    std::atomic<quint64> m_rxBytes{0};
    std::atomic<quint64> m_decodedFrames{0};
    // Everything received but never displayed: loss-suspect AUs,
    // backlog overflow victims, and deltas skipped while waiting for
    // a keyframe.
    std::atomic<quint64> m_droppedAus{0};
    std::atomic<int> m_lastWidth{0};
    std::atomic<int> m_lastHeight{0};
    std::atomic<qint64> m_lastKfRequestMs{0};

    // Worker-thread-only state.
    std::unique_ptr<VideoDecoder> m_decoder;
    bool m_waitingForKeyframe = true;   // never decode deltas cold

    static constexpr int kMaxQueuedAus = 16;
    static constexpr qint64 kKfRequestMinIntervalMs = 700;
};
