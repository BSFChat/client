#pragma once

#include "voice/video/VideoCodec.h"

#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>

// Per-remote-peer video surface registry — the single render path for
// remote video, fed by BOTH transports (decoded RTP frames and legacy
// JPEG stills), replacing the old per-frame base64 data-URL → QML
// Image pipeline (whose async reloads caused the black-flicker bug).
//
// QML usage:
//   VideoOutput { Component.onCompleted:
//       server.videoRegistry.attachOutput(userId, 0, videoSink) }
// Placeholder logic binds to liveScreenUsers/liveVideoChanged instead
// of per-frame signals, so tiles no longer rebuild on every frame.
//
// Lives on the main thread (owned by ServerConnection). deliverFrame
// is invoked via queued connections from decode workers.
class VideoStreamRegistry : public QObject {
    Q_OBJECT
    // Users with a live (frame within kLiveTimeoutMs) screen stream —
    // drives the remote-share tile list in VoiceRoom.
    Q_PROPERTY(QStringList liveScreenUsers READ liveScreenUsers NOTIFY liveVideoChanged)

public:
    explicit VideoStreamRegistry(QObject* parent = nullptr);
    ~VideoStreamRegistry() override;

    // The internal sink frames land on. Created on demand.
    Q_INVOKABLE QVideoSink* sinkFor(const QString& userId, int streamId);
    // Mirror frames onto a QML VideoOutput's sink (the forwardTo
    // pattern) — the last frame is replayed immediately so a tile that
    // attaches mid-stream isn't blank until the next frame.
    Q_INVOKABLE void attachOutput(const QString& userId, int streamId,
                                  QVideoSink* target);
    Q_INVOKABLE bool hasLiveVideo(const QString& userId, int streamId) const;
    // True when the sender explicitly told us this stream STOPPED and
    // nothing has arrived since (S-7). QML filters these out of the
    // share tile list, so a stopped share can't keep a tile alive —
    // with a "Starting share…" placeholder, of all things — through
    // the roster's announced flag until the next poll.
    Q_INVOKABLE bool streamStopped(const QString& userId, int streamId) const;

    QStringList liveScreenUsers() const { return liveUsers(int(VideoStreamId::Screen)); }
    QStringList liveUsers(int streamId) const;

public slots:
    // RTP path: decoded frames from VideoReceivePipeline workers.
    void deliverFrame(const QString& userId, int streamId,
                      const QVideoFrame& frame);
    // Legacy path: JPEG stills decoded to QImage.
    void deliverImage(const QString& userId, int streamId, const QImage& image);
    // Explicit {"t":"stream"} lifecycle control from the sender (S-7).
    // on=false blanks the surface immediately instead of waiting out
    // kLiveTimeoutMs; on=true clears the stopped flag so the tile (and
    // its placeholder) can come back for a restarted share.
    void setStreamAnnounced(const QString& userId, int streamId, bool on);
    // Legacy JPEG path, decoded OFF the GUI thread (S-10). Callers hand
    // over the compressed bytes; the QImage never touches the main
    // thread until it is ready to display.
    void deliverJpeg(const QString& userId, int streamId,
                     const QByteArray& jpeg);
    // Peer left / stream ended — blank the sink and drop live state.
    void dropUser(const QString& userId);
    void dropStream(const QString& userId, int streamId);
    void clear();

signals:
    void liveVideoChanged(const QString& userId, int streamId);

private:
    struct Entry {
        QVideoSink* sink = nullptr;       // owned (child of this)
        QList<QVideoSink*> outputs;       // attached QML sinks (not owned)
        QVideoFrame lastFrame;
        qint64 lastFrameMs = 0;
        bool live = false;
        // Sender said "stopped" and nothing has arrived since.
        bool stopped = false;
    };
    using Key = QPair<QString, int>;

    Entry& entry(const QString& userId, int streamId);
    void markLive(const QString& userId, int streamId, Entry& e);
    void sweepStale();

    QHash<Key, Entry> m_entries;
    QTimer m_sweepTimer;
    // Decode worker for the legacy JPEG path (S-10), created on first
    // use so builds that never see a legacy peer never start a thread.
    class JpegDecodeWorker;
    JpegDecodeWorker* m_jpegWorker = nullptr;

    static constexpr qint64 kLiveTimeoutMs = 4000;
};
