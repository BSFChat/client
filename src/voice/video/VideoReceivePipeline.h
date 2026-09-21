#pragma once

#include "voice/video/VideoCodec.h"
#include "voice/video/VideoPlayoutBuffer.h"

#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <QVideoFrame>

#include <atomic>
#include <functional>
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
//
// PLAYOUT. With a smoothing ceiling above zero (Settings::
// videoSmoothingMs, applied by VoiceEngine), decoded pictures do not go
// straight out: they are handed back to this object's own (GUI) thread,
// held in a VideoPlayoutBuffer and released on the sender's capture
// cadence by a precise timer — see VideoPlayoutBuffer.h for why and how.
// The timer lives on the GUI thread rather than the decode worker on
// purpose: presentation happens there anyway (the registry and the
// video sinks are GUI-thread objects), and a worker-thread timer would
// be held up by every IDR decode it is supposed to be smoothing over.
//
// At ZERO the buffer is bypassed entirely and frameDecoded is emitted
// from the worker the moment a picture decodes, exactly as before the
// buffer existed. Access units without a media timestamp (mediaTimeUs
// < 0) take the same bypass: with nothing to schedule by, holding them
// would only add delay.
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
    // `mediaTimeUs` is the sender's capture time for this unit (the
    // unwrapped RTP timestamp, or the lossless header's clock), on the
    // sender's clock — only differences between units are meaningful.
    // Negative = unknown; the unit is then presented on arrival.
    void submitAccessUnit(const QByteArray& au, bool keyframeHint = false,
                          bool lossSuspected = false,
                          qint64 mediaTimeUs = -1);

    // Playout smoothing ceiling in ms; 0 = off (present on decode, the
    // pre-buffer behaviour). GUI thread. Lowering it applies at once;
    // turning it off releases the newest held picture immediately.
    void setPlayoutMaxDelayMs(int ms);
    int playoutMaxDelayMs() const { return int(m_playoutMaxDelayUs.load() / 1000); }

    // The ceiling a user setting turns into for a given stream. Camera
    // is capped for lip sync — see kCameraPlayoutCapMs.
    static int effectivePlayoutCeilingMs(VideoStreamId stream, int userMs);

    // Diagnostics for the stats overlay (GUI thread). Local only.
    int playoutTargetMs() const { return int(m_playout.targetUs() / 1000); }
    int playoutHeldMs() const { return int(m_playout.lastHeldUs() / 1000); }
    int playoutJitterMs() const { return int(m_playout.jitterUs() / 1000); }
    quint64 playoutSkipped() const {
        return m_playout.stats().skippedLate + m_playout.stats().overflowed;
    }

    // Test seam: replace the playout clock (µs, monotonic). GUI thread,
    // before the first frame.
    void setPlayoutClockForTest(std::function<qint64()> clock) {
        m_playoutClock = std::move(clock);
    }

    // Cumulative receive counters, read by VoiceEngine's 500 ms
    // receiver-report tick and sent to the remote sender, which
    // derives delivery loss for its rate controller.
    quint64 rxFrames() const { return m_rxFrames.load(); }
    quint64 rxBytes() const { return m_rxBytes.load(); }

    // Diagnostics counters for the video stats overlay. Cumulative;
    // the UI polls and diffs successive snapshots for rates.
    quint64 decodedFrames() const { return m_decodedFrames.load(); }
    quint64 droppedAus() const { return m_droppedAus.load(); }
    // Cumulative keyframe requests this pipeline has emitted. Read by
    // the video diagnostics and asserted on by the unit tests.
    quint64 keyframeRequests() const { return m_kfRequests.load(); }
    // How many times a decoder refused to come up for this stream.
    // Counts ATTEMPTS, not access units: the retry is throttled, so a
    // stream that can never be decoded increments this once per
    // kDecoderRetryMs rather than once per frame.
    quint64 decoderFailures() const { return m_decoderFailures.load(); }

    // Spacing between attempts to rebuild a decoder that refused, and
    // therefore also the spacing of the warning it logs. Production
    // leaves the default; tests shorten it. Thread-safe.
    void setDecoderRetryIntervalMs(qint64 ms) {
        m_decoderRetryIntervalMs.store(qMax(qint64(0), ms));
    }

    // Minimum spacing between keyframe requests. Production leaves the
    // default; tests shorten it so a re-request run doesn't cost a
    // second of wall clock per assertion. Thread-safe.
    void setKeyframeRequestIntervalMs(qint64 ms) {
        m_kfRequestIntervalMs.store(qMax(qint64(0), ms));
    }
    int frameWidth() const { return m_lastWidth.load(); }
    int frameHeight() const { return m_lastHeight.load(); }

signals:
    // Both emitted from the worker thread (queued to receivers).
    void frameDecoded(const QString& userId, int streamId,
                      const QVideoFrame& frame);
    void keyframeNeeded(const QString& userId, int streamId);
    // No decoder could be brought up for this stream's codec on this
    // machine — the capability probe that made us advertise it was
    // wrong, and every access unit from here on is undecodable.
    //
    // Emitted at most ONCE per pipeline, on the first failure, because
    // its only subscriber puts a caps correction on the wire. `codec`
    // is a VideoCodecKind (int so the signal crosses threads without a
    // registered metatype).
    void decoderUnavailable(const QString& userId, int streamId, int codec);

private:
    void drainQueue();   // worker thread
    // GUI thread: a decoded picture arriving for playout, and the timer
    // that releases held pictures when they fall due.
    void enqueueForPlayout(const QVideoFrame& frame, qint64 mediaTimeUs);
    void presentDue();
    void schedulePlayout();
    qint64 playoutNowUs() const;
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
    struct QueuedAu {
        QByteArray data;
        bool keyframe = false;
        bool loss = false;
        qint64 mediaTimeUs = -1;
    };
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
    std::atomic<quint64> m_kfRequests{0};
    std::atomic<qint64> m_kfRequestIntervalMs{kKfRequestMinIntervalMs};
    std::atomic<quint64> m_decoderFailures{0};
    std::atomic<qint64> m_decoderRetryIntervalMs{kDecoderRetryMs};

    // Worker-thread-only state.
    std::unique_ptr<VideoDecoder> m_decoder;
    bool m_waitingForKeyframe = true;   // never decode deltas cold
    // A decoder refused to come up; don't try again before this time.
    // 0 = never failed (or recovered). Without it the old code rebuilt
    // a known-broken decoder once per access unit and logged once per
    // access unit — 60 lines a second across two streams, which is how
    // the field log that prompted this work was found and also why it
    // was unreadable.
    qint64 m_decoderRetryAtMs = 0;
    // The decoderUnavailable edge has been spent for this pipeline.
    bool m_decoderFailureAnnounced = false;

    // Playout. The ceiling is atomic because the worker reads it to
    // choose between the bypass and the buffer; everything else is
    // GUI-thread only.
    std::atomic<qint64> m_playoutMaxDelayUs{0};
    VideoPlayoutBuffer<QVideoFrame> m_playout;
    QTimer m_playoutTimer;
    QElapsedTimer m_playoutElapsed;
    std::function<qint64()> m_playoutClock;

    static constexpr int kMaxQueuedAus = 16;
    static constexpr qint64 kKfRequestMinIntervalMs = 700;
    static constexpr qint64 kDecoderRetryMs = 5000;

public:
    // Lip-sync cap on the CAMERA stream's smoothing ceiling.
    //
    // Audio and video are not synchronised in this client: audio rides
    // its own data channel with a 20 ms sequence counter and no capture
    // clock, video rides RTP with one, and nothing relates the two. What
    // CAN be reasoned about is the direction of the error. The audio
    // JitterBuffer holds 40-200 ms (60 ms initially) and video, before
    // this buffer, held nothing — so camera video already ran AHEAD of
    // the voice it belongs to by roughly the audio buffer's depth. Delay
    // added to video first cancels that lead and then turns into audio
    // leading video, which is the direction people notice first (ITU-R
    // BT.1359: audio early by more than ~45 ms is detectable; late by up
    // to ~125 ms is not). 60 ms of audio depth + 45 ms of tolerance =
    // ~100 ms. Beyond that the smoother camera would visibly break lip
    // sync, so the camera ceiling stops there whatever the setting says.
    // Screen share has no lips; it takes the user's full ceiling.
    static constexpr int kCameraPlayoutCapMs = 100;
};
