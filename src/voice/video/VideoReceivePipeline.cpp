#include "voice/video/VideoReceivePipeline.h"

#include "voice/video/VideoDecoder.h"

#include <QDateTime>
#include <QLoggingCategory>

#include <limits>

Q_LOGGING_CATEGORY(logVideoRecv, "bsfchat.video.recv", QtWarningMsg)

namespace {

// Scan an Annex-B access unit for a NAL that lets a receiver start
// here. Handles both 3- and 4-byte start codes.
//
//   H.264 — an IDR slice, NAL type 5, in the low 5 bits of a one-byte
//           header.
//   HEVC  — any IRAP picture, NAL types 16..23 (BLA_W_LP through
//           CRA_NUT), in bits 1..6 of a TWO-byte header. Checking only
//           IDR_W_RADL/IDR_N_LP would miss the CRA pictures a
//           VideoToolbox HEVC encoder is free to emit for a forced
//           keyframe, and the stream would then sit frozen re-asking
//           for an IDR that had already arrived.
bool containsKeyframeNal(const QByteArray& au, bool hevc) {
    const auto* p = reinterpret_cast<const uint8_t*>(au.constData());
    const int n = au.size();
    for (int i = 0; i + 3 < n; ++i) {
        if (p[i] == 0 && p[i + 1] == 0
            && (p[i + 2] == 1 || (p[i + 2] == 0 && i + 4 < n && p[i + 3] == 1))) {
            const int nalStart = i + (p[i + 2] == 1 ? 3 : 4);
            if (hevc) {
                if (nalStart + 1 < n) {
                    const uint8_t type = (p[nalStart] >> 1) & 0x3F;
                    if (type >= 16 && type <= 23) return true;
                }
            } else if (nalStart < n && (p[nalStart] & 0x1F) == 5) {
                return true;
            }
            i = nalStart - 1;
        }
    }
    return false;
}

} // namespace

VideoReceivePipeline::VideoReceivePipeline(const QString& userId,
                                           VideoStreamId streamId,
                                           VideoCodecKind codec,
                                           QObject* parent)
    : QObject(parent)
    , m_userId(userId)
    , m_streamId(streamId)
    , m_codec(codec)
{
    m_worker.moveToThread(&m_thread);
    m_thread.setObjectName(QStringLiteral("video-dec-%1").arg(userId.left(12)));
    m_thread.start();

    // Presentation timer. PreciseTimer: the default CoarseTimer may fire
    // up to 5% of the interval late, and a buffer that exists to put
    // frames on an even cadence cannot be driven by an uneven clock.
    m_playoutTimer.setSingleShot(true);
    m_playoutTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_playoutTimer, &QTimer::timeout, this,
            &VideoReceivePipeline::presentDue);
    m_playoutElapsed.start();
}

VideoReceivePipeline::~VideoReceivePipeline() {
    QMetaObject::invokeMethod(&m_worker, [this]() {
        m_decoder.reset();
    }, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
}

void VideoReceivePipeline::submitAccessUnit(const QByteArray& au,
                                            bool keyframeHint,
                                            bool lossSuspected,
                                            qint64 mediaTimeUs) {
    m_rxFrames.fetch_add(1);
    m_rxBytes.fetch_add(quint64(au.size()));
    bool overflowed = false;
    {
        QMutexLocker lock(&m_mutex);
        if (m_queue.size() >= kMaxQueuedAus) {
            // Decode can't keep up — drop the whole backlog rather than
            // fall progressively behind. Re-entry at the next keyframe.
            m_droppedAus.fetch_add(quint64(m_queue.size()));
            m_queue.clear();
            overflowed = true;
        }
        m_queue.append({au, keyframeHint, lossSuspected, mediaTimeUs});
    }
    if (overflowed) {
        qCWarning(logVideoRecv, "[%s/%d] decode backlog dropped",
                 qPrintable(m_userId), int(m_streamId));
        QMetaObject::invokeMethod(&m_worker, [this]() {
            m_waitingForKeyframe = true;
        }, Qt::QueuedConnection);
        requestKeyframeThrottled();
    }
    if (!m_drainQueued.exchange(true)) {
        QMetaObject::invokeMethod(&m_worker, [this]() { drainQueue(); },
                                  Qt::QueuedConnection);
    }
}

void VideoReceivePipeline::requestKeyframeThrottled() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 last = m_lastKfRequestMs.load(std::memory_order_relaxed);
    if (now - last < m_kfRequestIntervalMs.load(std::memory_order_relaxed)) return;
    if (!m_lastKfRequestMs.compare_exchange_strong(last, now)) return;
    m_kfRequests.fetch_add(1);
    emit keyframeNeeded(m_userId, int(m_streamId));
}

void VideoReceivePipeline::drainQueue() {
    m_drainQueued.store(false);
    for (;;) {
        QueuedAu queued;
        {
            QMutexLocker lock(&m_mutex);
            if (m_queue.isEmpty()) return;
            queued = m_queue.takeFirst();
        }
        const QByteArray& au = queued.data;

        if (queued.loss) {
            // RTP gap while this AU was reassembled — it's likely
            // incomplete. Decoding it "works" (decoders conceal) but
            // paints corruption that compounds until the next IDR.
            // Freeze instead: drop it, resume at the next keyframe.
            if (!m_waitingForKeyframe)
                qCInfo(logVideoRecv, "[%s/%d] loss-suspect AU dropped, "
                       "waiting for keyframe",
                       qPrintable(m_userId), int(m_streamId));
            m_waitingForKeyframe = true;
            m_droppedAus.fetch_add(1);
            requestKeyframeThrottled();
            continue;
        }

        // Keyframe gate BEFORE decoder creation: while we're waiting
        // there is nothing to decode, so a stream that never recovers
        // must not also hold a decoder session open.
        if (m_waitingForKeyframe) {
            // H.264/HEVC AUs are scanned for a random-access NAL; AV1
            // relies on the transport-provided keyframe flag (its
            // keyframes are not cheaply detectable from the bitstream).
            const bool isKey = isRtpVideoCodec(m_codec)
                ? containsKeyframeNal(au, m_codec == VideoCodecKind::H265)
                : queued.keyframe;
            if (!isKey) {
                m_droppedAus.fetch_add(1);
                // S-2: keep asking. The request that put us in this
                // state can be lost (it used to ride an unreliable
                // channel, and even a reliable one loses the race when
                // the sender is mid-teardown), and nothing else re-fires
                // it — the stream then stayed frozen until the periodic
                // IDR, i.e. 10-30 s. Throttled to one request per
                // kKfRequestMinIntervalMs, so a 30 fps P-frame run costs
                // ~1.4 requests/s, not 30.
                requestKeyframeThrottled();
                continue;
            }
            m_waitingForKeyframe = false;
        }

        if (!m_decoder) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (m_decoderRetryAtMs > 0 && now < m_decoderRetryAtMs) {
                // Known broken and the retry isn't due. Drop the unit
                // instead of rebuilding a decoder that just refused —
                // and stay in the keyframe wait, because the gate
                // above has already consumed this one.
                m_droppedAus.fetch_add(1);
                m_waitingForKeyframe = true;
                continue;
            }
            m_decoder = VideoDecoder::create(m_codec);
            if (!m_decoder || !m_decoder->init(m_codec)) {
                m_decoder.reset();
                m_decoderFailures.fetch_add(1);
                m_decoderRetryAtMs =
                    now + m_decoderRetryIntervalMs.load(std::memory_order_relaxed);
                m_waitingForKeyframe = true;
                // Loud once, then once per retry interval. The old code
                // logged this per access unit forever.
                qCWarning(logVideoRecv, "[%s/%d] no %s decoder available%s",
                         qPrintable(m_userId), int(m_streamId),
                         videoCodecName(m_codec),
                         m_decoderFailureAnnounced
                             ? " (still)"
                             : " — this machine advertised a codec it cannot "
                               "decode; correcting our caps");
                // Nothing queued behind this is decodable either, and
                // holding it would only decode into the tile once some
                // later codec change built a working decoder.
                {
                    QMutexLocker lock(&m_mutex);
                    m_droppedAus.fetch_add(quint64(m_queue.size()) + 1);
                    m_queue.clear();
                }
                if (!m_decoderFailureAnnounced) {
                    m_decoderFailureAnnounced = true;
                    // One announcement per pipeline: the subscriber
                    // turns this into a caps message to every peer.
                    emit decoderUnavailable(m_userId, int(m_streamId),
                                            int(m_codec));
                }
                return;
            }
            // Came up, possibly after an earlier refusal (transient
            // hardware contention) — reopen the retry gate.
            // m_decoderFailureAnnounced deliberately stays set: the
            // caps correction has already gone out, and announcing a
            // codec's death twice is noise, not new information.
            m_decoderRetryAtMs = 0;
        }

        QVideoFrame frame;
        switch (m_decoder->decode(au, frame)) {
        case VideoDecoder::Result::Ok:
            m_decodedFrames.fetch_add(1);
            m_lastWidth.store(frame.width());
            m_lastHeight.store(frame.height());
            if (queued.mediaTimeUs >= 0) frame.setStartTime(queued.mediaTimeUs);
            if (m_playoutMaxDelayUs.load(std::memory_order_relaxed) > 0
                && queued.mediaTimeUs >= 0) {
                // Hand the picture to the GUI-thread playout buffer.
                // Queued to `this`, which lives on the GUI thread: if the
                // pipeline is destroyed first, Qt drops the invocation.
                const qint64 media = queued.mediaTimeUs;
                QMetaObject::invokeMethod(this, [this, frame, media]() {
                    enqueueForPlayout(frame, media);
                }, Qt::QueuedConnection);
            } else {
                // Smoothing off (or no timestamp): today's path, unchanged.
                emit frameDecoded(m_userId, int(m_streamId), frame);
            }
            break;
        case VideoDecoder::Result::NeedMore:
            break;
        case VideoDecoder::Result::Error:
            // Corrupt reference chain (packet loss upstream). Flush,
            // resume at the next keyframe, ask the sender for one.
            m_decoder->reset();
            m_waitingForKeyframe = true;
            requestKeyframeThrottled();
            break;
        }
    }
}

// ---- Playout (GUI thread) ------------------------------------------

int VideoReceivePipeline::effectivePlayoutCeilingMs(VideoStreamId stream,
                                                    int userMs) {
    const int ms = qMax(0, userMs);
    return stream == VideoStreamId::Camera ? qMin(ms, kCameraPlayoutCapMs) : ms;
}

void VideoReceivePipeline::setPlayoutMaxDelayMs(int ms) {
    const int effective = effectivePlayoutCeilingMs(m_streamId, ms);
    m_playoutMaxDelayUs.store(qint64(effective) * 1000);
    m_playout.setMaxDelayUs(qint64(effective) * 1000);
    if (effective == 0) {
        // Switched off mid-stream: show the newest held picture now and
        // forget the rest — the user asked for the lowest latency, and
        // pictures already superseded are not worth a frame each.
        m_playoutTimer.stop();
        std::optional<QVideoFrame> newest;
        while (auto f = m_playout.popDue(std::numeric_limits<qint64>::max()))
            newest = std::move(f);
        m_playout.clear();
        if (newest) emit frameDecoded(m_userId, int(m_streamId), *newest);
    }
}

qint64 VideoReceivePipeline::playoutNowUs() const {
    return m_playoutClock ? m_playoutClock() : m_playoutElapsed.nsecsElapsed() / 1000;
}

void VideoReceivePipeline::enqueueForPlayout(const QVideoFrame& frame,
                                             qint64 mediaTimeUs) {
    // The ceiling can have dropped to zero while this was in flight from
    // the worker; then it is simply late and goes straight out.
    if (m_playoutMaxDelayUs.load(std::memory_order_relaxed) <= 0) {
        emit frameDecoded(m_userId, int(m_streamId), frame);
        return;
    }
    m_playout.push(frame, mediaTimeUs, playoutNowUs());
    presentDue();
}

void VideoReceivePipeline::presentDue() {
    // A wake-up that is late (timer deferred, GUI thread busy) costs
    // latency, not frames: popDue shows the oldest due picture and
    // re-times the rest, within the ceiling — see "A late wake-up" in
    // VideoPlayoutBuffer.h. Only what the ceiling cannot absorb is
    // skipped.
    if (auto f = m_playout.popDue(playoutNowUs()))
        emit frameDecoded(m_userId, int(m_streamId), *f);
    schedulePlayout();
}

void VideoReceivePipeline::schedulePlayout() {
    const auto due = m_playout.nextDueUs();
    if (!due) {
        m_playoutTimer.stop();
        return;
    }
    // Round UP to the next millisecond: waking a fraction early would
    // find nothing due and cost a second wake-up.
    const qint64 waitUs = qMax<qint64>(0, *due - playoutNowUs());
    m_playoutTimer.start(int((waitUs + 999) / 1000));
}
