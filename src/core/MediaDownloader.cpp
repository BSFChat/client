#include "MediaDownloader.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <algorithm>

MediaDownloader::MediaDownloader(QObject* parent) : QObject(parent)
{
    // One-shot sweep on construction so a previous session's bloated
    // cache gets trimmed before we take on new downloads. Cheap.
    enforceCacheBudget();
}

QString MediaDownloader::cacheDirPath() const
{
    QString base = QStandardPaths::writableLocation(
        QStandardPaths::CacheLocation);
    QString dir = base + "/media";
    QDir().mkpath(dir);
    return dir;
}

QString MediaDownloader::cacheKeyForUrl(const QString& url)
{
    // The query string is not part of the object's identity.
    //
    // This started as a bug fix: media URLs used to carry ?access_token=, so
    // hashing the WHOLE url keyed every cached file to the session token. Every
    // re-login changed the token, every previously downloaded image and video
    // therefore missed, and the whole cache was re-fetched while the old copies
    // sat there consuming the 200 MB budget until eviction reached them.
    //
    // The token is gone — the query now carries a signed media ticket
    // (util/MediaUrl.h) — and the rule matters more than it did, not less. A
    // ticket is replaced every few minutes, so a key that included it would
    // miss on essentially every fetch and the cache would never hit at all.
    const int q = url.indexOf(QLatin1Char('?'));
    return q < 0 ? url : url.left(q);
}

QString MediaDownloader::cachePathFor(const QString& url) const
{
    // Hash the identity (not the credential) so filenames are bounded +
    // collision-safe. Preserve the extension (if any) so platform decoders
    // can sniff — taken from the path for the same reason: with the query
    // string included, "the last dot" was a dot inside the base64 access
    // token, so every authenticated download landed under a meaningless
    // extension and the sniffing this exists for never happened.
    const QString key = cacheKeyForUrl(url);
    QByteArray h = QCryptographicHash::hash(key.toUtf8(),
                                            QCryptographicHash::Sha1)
                       .toHex();
    QString ext;
    int dot = key.lastIndexOf('.');
    int slash = key.lastIndexOf('/');
    if (dot > slash && (key.size() - dot) <= 6)
        ext = key.mid(dot);
    return cacheDirPath() + "/" + QString::fromLatin1(h) + ext;
}

void MediaDownloader::request(const QString& remoteUrl)
{
    if (remoteUrl.isEmpty()) {
        emit failed(remoteUrl, QStringLiteral("empty URL"));
        return;
    }

    // Cache hit — fire `completed` on the next tick so callers can wire
    // the signal before we emit.
    auto it = m_entries.constFind(remoteUrl);
    QString localPath = cachePathFor(remoteUrl);
    if (it != m_entries.constEnd() && it->done
        && QFileInfo::exists(it->path)) {
        QString path = it->path;
        QString url = QUrl::fromLocalFile(path).toString();
        QTimer::singleShot(0, this, [this, remoteUrl, url, path]() {
            emit completed(remoteUrl, url);
            handOffToDesktop(remoteUrl, path);
        });
        return;
    }
    if (QFileInfo::exists(localPath)) {
        Entry e;
        e.path = localPath;
        e.done = true;
        e.progress = 1.0;
        m_entries.insert(remoteUrl, e);
        QString url = QUrl::fromLocalFile(localPath).toString();
        QTimer::singleShot(0, this, [this, remoteUrl, url, localPath]() {
            emit completed(remoteUrl, url);
            handOffToDesktop(remoteUrl, localPath);
        });
        return;
    }

    // De-dupe in-flight requests — a second caller just waits.
    if (it != m_entries.constEnd() && it->reply) {
        return;
    }

    const QUrl url(remoteUrl);
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    // The credential goes in a header, never in the URL. Scheme-gated so a
    // redirect target or a local path can never be handed the token, and so the
    // cache key (which is the URL minus its query) stays free of it.
    if (!m_authToken.isEmpty()
        && (url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http"))) {
        req.setRawHeader("Authorization", ("Bearer " + m_authToken).toUtf8());
    }
    QNetworkReply* reply = m_nam.get(req);

    Entry e;
    e.path = localPath;
    e.reply = reply;
    m_entries.insert(remoteUrl, e);

    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, remoteUrl](qint64 got, qint64 total) {
                if (total <= 0) return;
                double p = double(got) / double(total);
                auto jt = m_entries.find(remoteUrl);
                if (jt != m_entries.end()) jt->progress = p;
                emit progressChanged(remoteUrl, p);
            });

    connect(reply, &QNetworkReply::finished, this,
            [this, remoteUrl, reply]() {
                reply->deleteLater();

                auto jt = m_entries.find(remoteUrl);
                if (jt == m_entries.end()) return;
                jt->reply = nullptr;

                if (reply->error() != QNetworkReply::NoError) {
                    QString msg = reply->errorString();
                    m_entries.remove(remoteUrl);
                    if (m_pendingOpens.remove(remoteUrl)) emit openFailed(remoteUrl, msg);
                    emit failed(remoteUrl, msg);
                    return;
                }

                QFile f(jt->path);
                if (!f.open(QIODevice::WriteOnly)) {
                    QString msg = f.errorString();
                    m_entries.remove(remoteUrl);
                    if (m_pendingOpens.remove(remoteUrl)) emit openFailed(remoteUrl, msg);
                    emit failed(remoteUrl, msg);
                    return;
                }
                f.write(reply->readAll());
                f.close();

                jt->done = true;
                jt->progress = 1.0;
                const QString localPath = jt->path;
                emit completed(remoteUrl,
                               QUrl::fromLocalFile(localPath).toString());
                handOffToDesktop(remoteUrl, localPath);

                // After every successful write, check whether the
                // cache is over budget and evict oldest-touched
                // files until it isn't. Doing it here (rather than
                // on a timer) couples the cost to actual usage:
                // a user who never downloads new media pays nothing.
                enforceCacheBudget();
            });
}

void MediaDownloader::openDownloaded(const QString& remoteUrl)
{
    if (remoteUrl.isEmpty()) {
        emit openFailed(remoteUrl, QStringLiteral("empty URL"));
        return;
    }
    // Already local (a file the player downloaded earlier, say) — nothing to
    // fetch, just show it.
    const QUrl url(remoteUrl);
    if (url.isLocalFile()) {
        if (!QDesktopServices::openUrl(url)) {
            emit openFailed(remoteUrl, QStringLiteral("the system could not open this file"));
        }
        return;
    }
    m_pendingOpens.insert(remoteUrl);
    request(remoteUrl);
}

void MediaDownloader::handOffToDesktop(const QString& remoteUrl, const QString& localPath)
{
    if (!m_pendingOpens.remove(remoteUrl)) return;

    // fromLocalFile, not fromUserInput or a re-parse of a string: the URL the
    // OS is handed is constructed from a path this process wrote into its own
    // cache directory, so there is no way for a remote URL to reach the
    // browser through here.
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(localPath))) {
        emit openFailed(remoteUrl, QStringLiteral("the system could not open this file"));
    }
}

void MediaDownloader::enforceCacheBudget()
{
    QDir dir(cacheDirPath());
    if (!dir.exists()) return;

    auto entries = dir.entryInfoList(
        QDir::Files | QDir::NoSymLinks,
        QDir::Time | QDir::Reversed); // oldest first by mtime

    qint64 total = 0;
    for (const auto& info : entries) total += info.size();
    if (total <= m_cacheSizeLimit) return;

    qInfo() << "[MediaDownloader] cache" << total / (1024 * 1024)
            << "MB exceeds limit"
            << m_cacheSizeLimit / (1024 * 1024)
            << "MB — evicting";

    for (const auto& info : entries) {
        if (total <= m_cacheSizeLimit) break;
        // Skip files we're actively writing to — the current map
        // keeps in-flight entries pointing at their paths.
        bool inFlight = false;
        for (const auto& entry : m_entries) {
            if (!entry.done && entry.path == info.filePath()) {
                inFlight = true;
                break;
            }
        }
        if (inFlight) continue;

        if (QFile::remove(info.filePath())) {
            total -= info.size();
            // Drop the in-memory entry so a fresh `request` for the
            // same URL triggers a re-download instead of returning
            // a file:// URL that no longer points to anything.
            for (auto it = m_entries.begin(); it != m_entries.end(); ) {
                if (it->path == info.filePath()) it = m_entries.erase(it);
                else ++it;
            }
        }
    }
}

double MediaDownloader::progress(const QString& remoteUrl) const
{
    auto it = m_entries.constFind(remoteUrl);
    if (it == m_entries.constEnd()) return 0.0;
    return it->progress;
}
