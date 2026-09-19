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
// THIS IS A CREDENTIAL IN A URL, AND IT IS NOT A LOG-HYGIENE PROBLEM.
//
// This comment used to describe the risk as "can be recorded by server access
// logs and any intervening proxy". That framing is what kept it open. What it
// actually is:
//
//   * On production, nginx's default `combined` access_log writes `$request`,
//     including the query string, so EVERY user's live session token is on
//     disk in /var/log/nginx/access.log and in every logrotate archive and
//     backup. Tokens renew on use, so a week-old log line is a working one.
//     (Tracked as audit finding B4; the nginx side belongs to work package P6.)
//
//   * It is the exfiltration channel for the media takeover. The file card
//     below hands this URL to Qt.openUrlExternally, so the user's SYSTEM
//     BROWSER opens the chat origin with the token in location.search. Until
//     the September 2026 media hardening, an attacker could upload HTML, have
//     it served inline with the type they chose, and read that token from
//     their own page. The server side of that is now closed — uploads are
//     re-typed against an allowlist and anything not inline-safe is served
//     `Content-Disposition: attachment` with nosniff and a restrictive CSP —
//     so the page no longer executes. The token is still in the URL.
//
// The durable fix is a short-lived signed media ticket scoped to one object
// and one viewer, minted by the server and carried as `?mt=`, replacing the
// session bearer entirely. It is not built yet: Image.source, MediaPlayer's
// source and the external-open paths all consume this one string, so it needs
// a mint-and-cache layer in front of all three plus a ticket endpoint on the
// server. Until then, do not add new call sites, and do not hand the result of
// this function to anything that shows a URL to the user or to another process.
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
