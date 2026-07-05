#include "voice/video/VideoStreamRegistry.h"

#include <QVideoFrameFormat>

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
    if (!e.live) {
        e.live = true;
        emit liveVideoChanged(userId, streamId);
    }
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
