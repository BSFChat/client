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

    void submitAccessUnit(const QByteArray& au);   // thread-safe

    // Cumulative receive counters, read by VoiceEngine's 500 ms
    // receiver-report tick and sent to the remote sender, which
    // derives delivery loss for its rate controller.
    quint64 rxFrames() const { return m_rxFrames.load(); }
    quint64 rxBytes() const { return m_rxBytes.load(); }

signals:
    // Both emitted from the worker thread (queued to receivers).
    void frameDecoded(const QString& userId, int streamId,
                      const QVideoFrame& frame);
    void keyframeNeeded(const QString& userId, int streamId);

private:
    void drainQueue();   // worker thread

    const QString m_userId;
    const VideoStreamId m_streamId;
    const VideoCodecKind m_codec;

    QThread m_thread;
    QObject m_worker;

    QMutex m_mutex;
    QList<QByteArray> m_queue;
    std::atomic<bool> m_drainQueued{false};
    std::atomic<quint64> m_rxFrames{0};
    std::atomic<quint64> m_rxBytes{0};

    // Worker-thread-only state.
    std::unique_ptr<VideoDecoder> m_decoder;
    bool m_waitingForKeyframe = true;   // never decode deltas cold

    static constexpr int kMaxQueuedAus = 16;
};
