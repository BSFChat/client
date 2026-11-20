#pragma once

// What "Share to BSFChat" will accept from another app (audit M-4).
//
// The share target is exported, so ANY installed app can hand us an
// ACTION_SEND. It used to pass EXTRA_STREAM straight to sendMediaMessage and
// upload it at once, and sendMediaMessage opens file:// as a local path — so a
// malicious app could share `file:///data/user/0/<us>/files/.../BSFChat.conf`
// and have BSFChat post its own settings, access token included, into
// whatever channel was open. The sender never needed to read the file itself.
//
// The rules, in order:
//   1. Only content:// with a non-empty authority. A sharing app that wants
//      to give us a file grants it through its own ContentProvider; file://
//      is how an app points us at a path it cannot read but we can.
//   2. Not one of OUR providers (authority == our package, or under it).
//   3. If the provider reports a filesystem path (the legacy `_data`
//      column), that path, canonicalised, must not be inside any of our own
//      data/cache/config directories.
// and then the user confirms before anything is uploaded (MobileMain).
//
// Pure, so it is testable on the desktop (tests/test_share_policy.cpp).

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace bsfchat::client::share {

// Fallback cap when the server's limit is unknown (it serves no
// /_matrix/media/v3/config). The server's own default max_upload_size_mb is
// 50; claiming 100 here let a 70 MB file upload in full and then 413.
inline constexpr qint64 kDefaultMaxUploadBytes = 50LL * 1024 * 1024;

enum class UriVerdict { Accept, NotContent, OwnProvider };

inline UriVerdict classifyUri(const QString& uri, const QString& ownPackage)
{
    const QUrl url(uri, QUrl::StrictMode);
    if (!url.isValid() || url.scheme().compare(QLatin1String("content"), Qt::CaseInsensitive) != 0
        || url.host().isEmpty())
        return UriVerdict::NotContent;
    const QString authority = url.host().toLower();
    const QString pkg = ownPackage.toLower();
    if (!pkg.isEmpty() && (authority == pkg || authority.startsWith(pkg + u'.')))
        return UriVerdict::OwnProvider;
    return UriVerdict::Accept;
}

// Canonical form for a containment test. A path that does not exist (yet) is
// cleaned instead, so `..` segments can't walk out of the comparison.
inline QString canonicalOrClean(const QString& path)
{
    const QString canon = QFileInfo(path).canonicalFilePath();
    return canon.isEmpty() ? QDir::cleanPath(path) : canon;
}

// True when `path` is `dir` itself or anything beneath one of `dirs`.
inline bool isInsideAny(const QString& path, const QStringList& dirs)
{
    if (path.isEmpty()) return false;
    const QString p = canonicalOrClean(path);
    for (const QString& d : dirs) {
        if (d.isEmpty()) continue;
        QString base = canonicalOrClean(d);
        if (base.isEmpty() || base == QLatin1String("/")) continue;
        if (p == base) return true;
        if (!base.endsWith(u'/')) base += u'/';
        if (p.startsWith(base)) return true;
    }
    return false;
}

} // namespace bsfchat::client::share
