// Download-then-play shim for Android's native MediaPlayer, which
// chokes on plenty of valid HTTP video sources (notably Matroska
// containers served over HTTP/2) and returns "Could not open file"
// without diagnostics. On desktop Qt uses FFmpeg and streams happily;
// on mobile we pay one full download before playback so MediaPlayer
// sees a local file:// URL instead.
//
// Cache is keyed by remote URL, kept under QStandardPaths::CacheLocation.
// Concurrent requests for the same URL share a single download.
#pragma once

#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>

class MediaDownloader : public QObject {
    Q_OBJECT
public:
    explicit MediaDownloader(QObject* parent = nullptr);

    // Returns a file:// URL once the download completes. If already
    // cached, emits `completed` synchronously (next event loop tick)
    // with the existing path.
    Q_INVOKABLE void request(const QString& remoteUrl);

    // 0..1 for in-flight; sticks at 1.0 once complete.
    Q_INVOKABLE double progress(const QString& remoteUrl) const;

    // LRU cap. When a newly-written file pushes the cache over this
    // many bytes, we delete the oldest-touched files until we're
    // under the limit again. Default 200 MB. Settable for tests.
    // Called once at construction; persistence is just ctime-based
    // so we don't need a separate index file.
    void setCacheSizeLimit(qint64 bytes) { m_cacheSizeLimit = bytes; }

signals:
    void completed(QString remoteUrl, QString localFileUrl);
    void failed(QString remoteUrl, QString error);
    void progressChanged(QString remoteUrl, double progress);

private:
    QNetworkAccessManager m_nam;

    struct Entry {
        QString path;         // local file path
        double progress = 0;  // 0..1
        bool done = false;
        QNetworkReply* reply = nullptr;
    };
    QHash<QString, Entry> m_entries;
    qint64 m_cacheSizeLimit = 200LL * 1024 * 1024; // 200 MB default

    QString cacheDirPath() const;
    QString cachePathFor(const QString& url) const;
    // Sweep: tally every file in the cache dir, if total > limit
    // delete oldest-modified until under. Cheap — bounded by the
    // number of cached files, which is capped by the size limit.
    void enforceCacheBudget();
};
