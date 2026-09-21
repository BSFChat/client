#pragma once

#include "voice/video/VideoCodec.h"
#include "voice/video/VideoSendStats.h"

#include <QMutex>
#include <QObject>
#include <QThread>
#include <QVideoFrame>

#include <atomic>
#include <memory>

class VideoEncoder;

Q_DECLARE_METATYPE(EncodedFrame)

// Per-source (screen / camera) encode worker. Owns a QThread; capture
// frames enter through submitFrame() from the main thread into a
// depth-1 latest-wins slot, so a slow encoder can never build a
// backlog — it always encodes the newest frame and silently skips the
// ones it couldn't keep up with. That property is the foundation of
// the "always smooth" guarantee: latency stays flat and the rate
// controller (P4) reacts to the skip rate instead of a growing queue.
class VideoSendPipeline : public QObject {
    Q_OBJECT
public:
    explicit VideoSendPipeline(VideoStreamId streamId, QObject* parent = nullptr);
    ~VideoSendPipeline() override;

    VideoStreamId streamId() const { return m_streamId; }

    // All thread-safe; work happens on the internal worker thread.
    void configure(const EncoderConfig& config);
    void submitFrame(const QVideoFrame& frame, qint64 captureTimeUs);
    void forceKeyframe() { m_forceKeyframe.store(true); }
    void setBitrate(int targetKbps, int maxKbps);

    // Cumulative encode-side counters for videosend::Accumulator. The
    // capture-side fields are left zero for the owner to fill. Reading
    // resets the per-window worst-frame time, so call it once per
    // evaluation. Thread-safe (atomics; no lock taken).
    videosend::Counters takeCounters();

signals:
    // Emitted from the worker thread (queued to receivers).
    void encodedFrameReady(int streamId, const EncodedFrame& frame);

private:
    void processPending();   // runs on worker thread

    const VideoStreamId m_streamId;
    QThread m_thread;
    // Worker-side QObject the queued invocations land on.
    QObject m_worker;

    QMutex m_mutex;
    QVideoFrame m_pendingFrame;
    qint64 m_pendingTimeUs = 0;
    EncoderConfig m_config;
    bool m_configDirty = true;
    std::atomic<bool> m_processQueued{false};
    std::atomic<bool> m_forceKeyframe{false};

    // Send-side instrumentation (VideoSendStats.h). `superseded` is the
    // latest-wins slot overflowing — a frame the worker never got to,
    // the most direct evidence that encode, not capture or network, is
    // what is costing frames.
    std::atomic<quint64> m_submitted{0};
    std::atomic<quint64> m_superseded{0};
    std::atomic<quint64> m_encoded{0};
    std::atomic<quint64> m_bytes{0};
    std::atomic<quint64> m_packets{0};
    std::atomic<quint64> m_keyframes{0};
    std::atomic<quint64> m_workUs{0};
    std::atomic<quint32> m_maxWorkUs{0};

    // Worker-thread-only state.
    std::unique_ptr<VideoEncoder> m_encoder;
    EncoderConfig m_sessionConfig;
    bool m_sessionValid = false;
};
