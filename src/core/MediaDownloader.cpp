#include "MediaDownloader.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
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

QString MediaDownloader::cachePathFor(const QString& url) const
{
    // Hash the full URL so filenames are bounded + collision-safe.
    // Preserve the extension (if any) so platform decoders can sniff.
    QByteArray h = QCryptographicHash::hash(url.toUtf8(),
                                            QCryptographicHash::Sha1)
                       .toHex();
    QString ext;
    int dot = url.lastIndexOf('.');
    int slash = url.lastIndexOf('/');
    if (dot > slash && (url.size() - dot) <= 6)
        ext = url.mid(dot);
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
        QString url = QUrl::fromLocalFile(it->path).toString();
        QTimer::singleShot(0, this, [this, remoteUrl, url]() {
            emit completed(remoteUrl, url);
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
        QTimer::singleShot(0, this, [this, remoteUrl, url]() {
            emit completed(remoteUrl, url);
        });
        return;
    }

    // De-dupe in-flight requests — a second caller just waits.
    if (it != m_entries.constEnd() && it->reply) {
        return;
    }

    QNetworkRequest req((QUrl(remoteUrl)));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
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
                    emit failed(remoteUrl, msg);
                    return;
                }

                QFile f(jt->path);
                if (!f.open(QIODevice::WriteOnly)) {
                    QString msg = f.errorString();
                    m_entries.remove(remoteUrl);
                    emit failed(remoteUrl, msg);
                    return;
                }
                f.write(reply->readAll());
                f.close();

                jt->done = true;
                jt->progress = 1.0;
                emit completed(remoteUrl,
                               QUrl::fromLocalFile(jt->path).toString());

                // After every successful write, check whether the
                // cache is over budget and evict oldest-touched
                // files until it isn't. Doing it here (rather than
                // on a timer) couples the cost to actual usage:
                // a user who never downloads new media pays nothing.
                enforceCacheBudget();
            });
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
