#include "voice/video/VideoReceivePipeline.h"

#include "voice/video/VideoDecoder.h"

#include <QDateTime>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(logVideoRecv, "bsfchat.video.recv", QtWarningMsg)

namespace {

// Scan an Annex-B access unit for an IDR slice (NAL type 5). Handles
// both 3- and 4-byte start codes.
bool containsIdr(const QByteArray& au) {
    const auto* p = reinterpret_cast<const uint8_t*>(au.constData());
    const int n = au.size();
    for (int i = 0; i + 3 < n; ++i) {
        if (p[i] == 0 && p[i + 1] == 0
            && (p[i + 2] == 1 || (p[i + 2] == 0 && i + 4 < n && p[i + 3] == 1))) {
            const int nalStart = i + (p[i + 2] == 1 ? 3 : 4);
            if (nalStart < n && (p[nalStart] & 0x1F) == 5) return true;
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
                                            bool lossSuspected) {
    m_rxFrames.fetch_add(1);
    m_rxBytes.fetch_add(quint64(au.size()));
    bool overflowed = false;
    {
        QMutexLocker lock(&m_mutex);
        if (m_queue.size() >= kMaxQueuedAus) {
            // Decode can't keep up — drop the whole backlog rather than
            // fall progressively behind. Re-entry at the next keyframe.
            m_queue.clear();
            overflowed = true;
        }
        m_queue.append({au, keyframeHint, lossSuspected});
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
    if (now - last < kKfRequestMinIntervalMs) return;
    if (!m_lastKfRequestMs.compare_exchange_strong(last, now)) return;
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
            requestKeyframeThrottled();
            continue;
        }

        if (!m_decoder) {
            m_decoder = VideoDecoder::create(m_codec);
            if (!m_decoder || !m_decoder->init(m_codec)) {
                qCWarning(logVideoRecv, "[%s/%d] no decoder available",
                         qPrintable(m_userId), int(m_streamId));
                m_decoder.reset();
                QMutexLocker lock(&m_mutex);
                m_queue.clear();
                return;
            }
        }

        if (m_waitingForKeyframe) {
            // H.264 AUs are scanned for an IDR NAL; other codecs rely
            // on the transport-provided keyframe flag.
            const bool isKey = m_codec == VideoCodecKind::H264
                ? containsIdr(au) : queued.keyframe;
            if (!isKey) continue;
            m_waitingForKeyframe = false;
        }

        QVideoFrame frame;
        switch (m_decoder->decode(au, frame)) {
        case VideoDecoder::Result::Ok:
            emit frameDecoded(m_userId, int(m_streamId), frame);
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
