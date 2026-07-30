#pragma once

#include <QString>
#include <QUrl>

#include <bsfchat/Constants.h>

namespace bsfchat::client {

// Single builder for mxc:// -> HTTP download URLs. Two callers used to
// assemble this by hand (MessageModel::resolveMediaUrl and
// MatrixClient::mediaDownloadUrl); they share this so the credential can't
// end up on one and not the other.
//
// The access token rides in the query string rather than an Authorization
// header because these URLs are handed straight to QML Image.source, which
// has no way to set headers. The server takes either form — see
// MediaHandler::authenticate_media.
//
// Caveat: a token in a query string can be recorded by server access logs
// and any intervening proxy. The durable fix is a short-lived signed media
// token scoped to one object, not the session's raw access token.
//
// Returns an empty string for a non-mxc URI or an unset homeserver. An empty
// token yields the old unauthenticated URL, which still works against a
// server with require_media_auth off.
inline QString buildMediaDownloadUrl(const QString& homeserver,
                                     const QString& accessToken,
                                     const QString& mxcUri)
{
    if (!mxcUri.startsWith(QStringLiteral("mxc://")) || homeserver.isEmpty())
        return {};

    QString url = homeserver
                  + QString::fromUtf8(api_path::kMediaDownload)
                  + mxcUri.mid(6); // strip "mxc://"

    if (!accessToken.isEmpty()) {
        url += QStringLiteral("?access_token=")
               + QString::fromUtf8(QUrl::toPercentEncoding(accessToken));
    }
    return url;
}

} // namespace bsfchat::client
