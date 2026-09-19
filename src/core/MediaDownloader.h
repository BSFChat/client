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
#include <QSet>
#include <QString>

class MediaDownloader : public QObject {
    Q_OBJECT
public:
    explicit MediaDownloader(QObject* parent = nullptr);

    // Returns a file:// URL once the download completes. If already
    // cached, emits `completed` synchronously (next event loop tick)
    // with the existing path.
    Q_INVOKABLE void request(const QString& remoteUrl);

    // Bearer token for media fetches, or empty for none.
    //
    // Sent as an Authorization header. C++ can do what QML's Image.source
    // cannot, which is why the external-open path (openDownloaded below) needs
    // neither a ticket nor a credential in the URL. Only ever attached to an
    // http(s) URL — a file:// or qrc: source gets nothing.
    void setAuthToken(const QString& token) { m_authToken = token; }

    // Download (or reuse the cached copy) and hand THE LOCAL FILE to the
    // desktop with QDesktopServices::openUrl(QUrl::fromLocalFile(...)).
    //
    // This exists so that "open this attachment" never involves a browser.
    // Qt.openUrlExternally on a media URL put that URL — and, before signed
    // tickets, the viewer's 90-day session token inside it — into the system
    // browser's address bar, its history, and the clipboard, and that click is
    // what completed the account-takeover chain in audit finding A1. Passing a
    // remote URL to the OS is now not something any media call site can do.
    //
    // Emits `openFailed` when the download fails or the OS refuses the file.
    Q_INVOKABLE void openDownloaded(const QString& remoteUrl);

    // 0..1 for in-flight; sticks at 1.0 once complete.
    Q_INVOKABLE double progress(const QString& remoteUrl) const;

    // LRU cap. When a newly-written file pushes the cache over this
    // many bytes, we delete the oldest-touched files until we're
    // under the limit again. Default 200 MB. Settable for tests.
    // Called once at construction; persistence is just ctime-based
    // so we don't need a separate index file.
    void setCacheSizeLimit(qint64 bytes) { m_cacheSizeLimit = bytes; }

    // The part of a media URL that identifies the OBJECT — everything before
    // the query string, which carries the session access token. Public and
    // static so the property can be tested without a cache directory.
    static QString cacheKeyForUrl(const QString& url);

signals:
    void completed(QString remoteUrl, QString localFileUrl);
    void failed(QString remoteUrl, QString error);
    void progressChanged(QString remoteUrl, double progress);
    // openDownloaded() could not put a local file in front of the user.
    void openFailed(QString remoteUrl, QString error);

private:
    QNetworkAccessManager m_nam;
    QString m_authToken;
    // Remote URLs whose completion should be handed to the desktop. A set, not
    // a flag on the entry: a video already being streamed by the player can be
    // asked to open while its download is in flight.
    QSet<QString> m_pendingOpens;

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
    // Opens `localPath` with the desktop if `remoteUrl` was requested through
    // openDownloaded(). No-op otherwise, so an ordinary request() never pops a
    // window open.
    void handOffToDesktop(const QString& remoteUrl, const QString& localPath);
    void enforceCacheBudget();
};
