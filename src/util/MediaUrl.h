#pragma once

#include <QString>
#include <QUrl>

#include <bsfchat/Constants.h>

namespace bsfchat::client {

// THE SESSION TOKEN IS NO LONGER IN MEDIA URLS.
//
// It used to be. `buildMediaDownloadUrl` appended `?access_token=<the viewer's
// 90-day token>` because QML's Image.source and MediaPlayer.source take a URL
// and cannot set an Authorization header. That single decision put a live
// credential in nginx's access log on every media fetch, in the system
// browser's address bar and history whenever the file card was clicked, and in
// the clipboard whenever anyone copied a media link — and it was the
// exfiltration channel of the one-click account takeover in audit finding A1.
//
// What replaced it: net/MediaTicketCache. The client asks the server for a
// short-lived signature scoped to one object and one user
// (POST /_matrix/media/v3/ticket), and puts THAT in the URL as
// `?mt=…&exp=…`. The server re-runs its own permission check at fetch time, so
// the ticket points at an authorization rather than being one. A ticket
// recovered from a log is worth one object, to the user it already belonged to,
// for five minutes.
//
// The endpoint path is spelled here and in the server's Server.cpp rather than
// in the protocol library's api_path:: constants, because adding a constant
// would put this change in the protocol repo — which has to merge first, and
// would redden CI on both other repos until it did. Fold it into api_path the
// next time protocol changes for another reason.
inline constexpr char kMediaTicketPath[] = "/_matrix/media/v3/ticket";

// The bare, unauthenticated download URL for an mxc URI: no credential, no
// ticket, no query string at all.
//
// This is the object's identity, and that is all it is. It is what a ticket
// gets appended to (MediaTicketCache::composeUrl), what MediaDownloader keys
// its on-disk cache by, and what a server with `[media] require_auth = false`
// will serve directly. On a server with require_auth on — the default, and the
// only sane setting — fetching this on its own gets a 401.
//
// DO NOT ADD A CREDENTIAL PARAMETER BACK TO THIS FUNCTION. If a caller needs a
// URL that will actually fetch, it needs MediaTicketCache, which is
// asynchronous on purpose. A synchronous "just give me a working URL" helper is
// precisely the shape that put a token in a query string for three releases.
//
// True for exactly `mxc://<server>/<mediaId>`: a server name of host
// characters (letters, digits, '.', '-', ':' and the brackets of an IPv6
// literal) and a media id of [A-Za-z0-9_-], 1-255 characters, with nothing
// after it — no further path segment, no '?', no '#', no percent-escape.
//
// The mxc URI comes from the sender's event. Before this was strict,
// `mxc://server/id/evil.bat` or `mxc://server/id#.bat` reached the download
// route (which accepts a trailing filename segment) and, through the cache
// file's extension, decided which program the OS opened the download with.
inline bool isValidMxcUri(const QString& mxcUri)
{
    if (!mxcUri.startsWith(QStringLiteral("mxc://"))) return false;
    const QStringView rest = QStringView(mxcUri).mid(6);
    const qsizetype slash = rest.indexOf(QLatin1Char('/'));
    if (slash <= 0) return false;
    const QStringView server = rest.left(slash);
    const QStringView id = rest.mid(slash + 1);
    if (server.size() > 255 || id.isEmpty() || id.size() > 255) return false;
    for (const QChar c : server) {
        const char16_t u = c.unicode();
        const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')
            || (u >= '0' && u <= '9') || u == '.' || u == '-' || u == ':'
            || u == '[' || u == ']';
        if (!ok) return false;
    }
    for (const QChar c : id) {
        const char16_t u = c.unicode();
        const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')
            || (u >= '0' && u <= '9') || u == '_' || u == '-' || u == '.';
        if (!ok) return false;
    }
    // '.' is allowed because the server accepts it in legacy media ids, and
    // nothing derives a file extension from the URL any more. A dot-only id
    // could still name a path segment, so refuse those.
    if (id.startsWith(QLatin1Char('.'))) return false;
    return true;
}

// Returns an empty string for anything isValidMxcUri() refuses, or an unset
// homeserver.
inline QString buildMediaDownloadUrl(const QString& homeserver, const QString& mxcUri)
{
    if (!isValidMxcUri(mxcUri) || homeserver.isEmpty())
        return {};

    return homeserver
         + QString::fromUtf8(api_path::kMediaDownload)
         + mxcUri.mid(6); // strip "mxc://"
}

} // namespace bsfchat::client
