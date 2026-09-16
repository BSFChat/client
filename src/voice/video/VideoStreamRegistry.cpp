#include "voice/video/VideoStreamRegistry.h"

#include <QMutex>
#include <QThread>
#include <QVideoFrameFormat>

#include <atomic>

// Off-thread JPEG decode for the legacy stills path (S-10).
//
// Decoding a 1600 px JPEG costs single-digit milliseconds; doing it in
// the GUI thread's slot, once per frame per sharing peer, is a visible
// hitch in scrolling and typing while anyone shares. The queue is
// LATEST-WINS PER STREAM: a decode that falls behind drops the frames
// it was overtaken by rather than growing a backlog, which is the same
// rule the send and receive pipelines follow.
class VideoStreamRegistry::JpegDecodeWorker {
public:
    explicit JpegDecodeWorker(VideoStreamRegistry* owner) : m_owner(owner) {
        m_worker.moveToThread(&m_thread);
        m_thread.setObjectName(QStringLiteral("video-jpeg-dec"));
        m_thread.start();
    }
    ~JpegDecodeWorker() {
        m_thread.quit();
        m_thread.wait();
    }

    void submit(const QString& userId, int streamId, const QByteArray& jpeg) {
        {
            QMutexLocker lock(&m_mutex);
            m_pending[{userId, streamId}] = jpeg;   // drop-oldest
        }
        if (!m_drainQueued.exchange(true)) {
            QMetaObject::invokeMethod(&m_worker, [this]() { drain(); },
                                      Qt::QueuedConnection);
        }
    }

private:
    void drain() {
        QHash<QPair<QString, int>, QByteArray> batch;
        {
            QMutexLocker lock(&m_mutex);
            batch.swap(m_pending);
            m_drainQueued.store(false);
        }
        for (auto it = batch.constBegin(); it != batch.constEnd(); ++it) {
            QImage img;
            if (!img.loadFromData(it.value(), "JPEG")) continue;
            const QString userId = it.key().first;
            const int streamId = it.key().second;
            auto* owner = m_owner;
            QMetaObject::invokeMethod(owner, [owner, userId, streamId, img]() {
                owner->deliverImage(userId, streamId, img);
            }, Qt::QueuedConnection);
        }
    }

    VideoStreamRegistry* const m_owner;
    QThread m_thread;
    QObject m_worker;
    QMutex m_mutex;
    QHash<QPair<QString, int>, QByteArray> m_pending;
    std::atomic<bool> m_drainQueued{false};
};

VideoStreamRegistry::VideoStreamRegistry(QObject* parent)
    : QObject(parent)
{
    // Live-state decay: a stream with no frames for kLiveTimeoutMs is
    // no longer "live" (sender stopped without an explicit signal —
    // e.g. crashed mid-share). Drives placeholder re-appearance.
    m_sweepTimer.setInterval(1000);
    connect(&m_sweepTimer, &QTimer::timeout, this, &VideoStreamRegistry::sweepStale);
    m_sweepTimer.start();
}

VideoStreamRegistry::~VideoStreamRegistry() {
    delete m_jpegWorker;   // joins the decode thread
}

VideoStreamRegistry::Entry& VideoStreamRegistry::entry(const QString& userId,
                                                       int streamId) {
    Entry& e = m_entries[{userId, streamId}];
    if (!e.sink) e.sink = new QVideoSink(this);
    return e;
}

QVideoSink* VideoStreamRegistry::sinkFor(const QString& userId, int streamId) {
    return entry(userId, streamId).sink;
}

void VideoStreamRegistry::attachOutput(const QString& userId, int streamId,
                                       QVideoSink* target) {
    if (!target) return;
    Entry& e = entry(userId, streamId);
    if (e.outputs.contains(target)) return;
    e.outputs.append(target);
    // Detach automatically when the QML item dies.
    connect(target, &QObject::destroyed, this,
            [this, userId, streamId](QObject* obj) {
        auto it = m_entries.find({userId, streamId});
        if (it != m_entries.end())
            it->outputs.removeAll(static_cast<QVideoSink*>(obj));
    });
    if (e.lastFrame.isValid()) target->setVideoFrame(e.lastFrame);
}

bool VideoStreamRegistry::hasLiveVideo(const QString& userId, int streamId) const {
    auto it = m_entries.constFind({userId, streamId});
    return it != m_entries.constEnd() && it->live;
}

QStringList VideoStreamRegistry::liveUsers(int streamId) const {
    QStringList out;
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        if (it.key().second == streamId && it->live) out << it.key().first;
    }
    out.sort();   // stable tile order
    return out;
}

void VideoStreamRegistry::markLive(const QString& userId, int streamId, Entry& e) {
    e.lastFrameMs = QDateTime::currentMSecsSinceEpoch();
    // Frames are the ground truth: anything arriving means the stream
    // is not stopped, whatever the last control message claimed.
    const bool wasStopped = e.stopped;
    e.stopped = false;
    if (!e.live) {
        e.live = true;
        emit liveVideoChanged(userId, streamId);
    } else if (wasStopped) {
        emit liveVideoChanged(userId, streamId);
    }
}

bool VideoStreamRegistry::streamStopped(const QString& userId,
                                        int streamId) const {
    auto it = m_entries.constFind({userId, streamId});
    return it != m_entries.constEnd() && it->stopped && !it->live;
}

void VideoStreamRegistry::setStreamAnnounced(const QString& userId,
                                             int streamId, bool on) {
    Entry& e = entry(userId, streamId);
    if (on) {
        if (!e.stopped) return;
        e.stopped = false;
        emit liveVideoChanged(userId, streamId);
        return;
    }
    e.stopped = true;
    // dropStream only signals when the stream WAS live; a share that
    // stopped before its first frame landed still has to clear the
    // announced-but-never-started tile, so signal unconditionally here.
    const bool wasLive = e.live;
    dropStream(userId, streamId);
    if (!wasLive) emit liveVideoChanged(userId, streamId);
}

void VideoStreamRegistry::deliverJpeg(const QString& userId, int streamId,
                                      const QByteArray& jpeg) {
    if (jpeg.isEmpty()) return;
    if (!m_jpegWorker) m_jpegWorker = new JpegDecodeWorker(this);
    m_jpegWorker->submit(userId, streamId, jpeg);
}

void VideoStreamRegistry::deliverFrame(const QString& userId, int streamId,
                                       const QVideoFrame& frame) {
    if (!frame.isValid()) return;
    Entry& e = entry(userId, streamId);
    e.lastFrame = frame;
    e.sink->setVideoFrame(frame);
    for (auto* out : e.outputs) out->setVideoFrame(frame);
    markLive(userId, streamId, e);
}

void VideoStreamRegistry::deliverImage(const QString& userId, int streamId,
                                       const QImage& image) {
    if (image.isNull()) return;
    QImage img = image;
    if (img.format() != QImage::Format_ARGB32
        && img.format() != QImage::Format_ARGB32_Premultiplied
        && img.format() != QImage::Format_RGB32) {
        img = img.convertToFormat(QImage::Format_ARGB32);
    }
    QVideoFrameFormat fmt(img.size(),
        QVideoFrameFormat::pixelFormatFromImageFormat(img.format()));
    QVideoFrame frame(fmt);
    if (!frame.map(QVideoFrame::WriteOnly)) return;
    const int rows = img.height();
    const int srcStride = int(img.bytesPerLine());
    const int dstStride = frame.bytesPerLine(0);
    for (int row = 0; row < rows; ++row) {
        memcpy(frame.bits(0) + row * dstStride,
               img.constScanLine(row), size_t(qMin(srcStride, dstStride)));
    }
    frame.unmap();
    deliverFrame(userId, streamId, frame);
}

void VideoStreamRegistry::dropStream(const QString& userId, int streamId) {
    auto it = m_entries.find({userId, streamId});
    if (it == m_entries.end()) return;
    // Blank attached outputs so tiles don't freeze on the last frame.
    it->sink->setVideoFrame(QVideoFrame());
    for (auto* out : it->outputs) out->setVideoFrame(QVideoFrame());
    const bool wasLive = it->live;
    it->live = false;
    it->lastFrame = QVideoFrame();
    if (wasLive) emit liveVideoChanged(userId, streamId);
}

void VideoStreamRegistry::dropUser(const QString& userId) {
    for (int s = 0; s < kVideoStreamCount; ++s) dropStream(userId, s);
}

void VideoStreamRegistry::clear() {
    const auto keys = m_entries.keys();
    for (const auto& key : keys) dropStream(key.first, key.second);
    // Entries (and their sinks) stay allocated — cheap, and QML items
    // may still hold attachments that re-bind on the next session.
}

void VideoStreamRegistry::sweepStale() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it->live && now - it->lastFrameMs > kLiveTimeoutMs) {
            it->live = false;
            it->sink->setVideoFrame(QVideoFrame());
            for (auto* out : it->outputs) out->setVideoFrame(QVideoFrame());
            emit liveVideoChanged(it.key().first, it.key().second);
        }
    }
}
