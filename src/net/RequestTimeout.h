#pragma once

#include <QByteArray>
#include <QCoreApplication>
#include <QNetworkReply>
#include <QString>

// How long a request may go without moving a byte before it is given up on,
// and what to say when one is (audit C-6).
//
// Only /sync used to have a timeout. Everything else waited for the OS to
// notice a dead TCP connection, which on a phone that changed networks or a
// laptop that slept can be never: a hung /messages left the history spinner up
// for the life of the session, a hung send left the bubble on "sending", and a
// hung re-auth step left SessionAuth in Reauthenticating — where "Reconnect"
// and "Sign in again" are both refused as "a login is already running".
//
// QNetworkRequest::setTransferTimeout is an INACTIVITY timer, not a deadline:
// Qt restarts it whenever bytes move in either direction. So a 2 GB upload on a
// slow link that keeps making progress is never cut off by it; what it catches
// is a transfer that has stopped. Media gets a longer allowance anyway, because
// after the last byte of an upload goes out the server may still be hashing,
// thumbnailing or scanning it before it says anything back.
namespace bsfchat::net {

// Ordinary API calls: small bodies, answered by one handler.
inline constexpr int kRequestTimeoutMs = 30 * 1000;
// Media upload / download: same rule, more patience for server-side work.
inline constexpr int kMediaTransferTimeoutMs = 2 * 60 * 1000;

// Did this reply end because the transfer timer fired? Qt reports that as an
// abort (OperationCanceledError); TimeoutError is the socket-level variant.
// Callers that abort replies on purpose (the /sync supersede path) filter
// those out before they get here.
inline bool replyTimedOut(const QNetworkReply* reply)
{
    if (!reply) return false;
    const auto e = reply->error();
    return e == QNetworkReply::OperationCanceledError
        || e == QNetworkReply::TimeoutError;
}

// The text an error signal carries for a failed reply: the server's own body
// when it sent one (every existing consumer parses that), otherwise something
// a person can read. A timed-out or dropped request has no body at all, and an
// empty string reaches the UI as an error with nothing in it — a dialog that
// stops spinning and says nothing, which reads exactly like the hang it is
// reporting.
inline QString replyFailureText(const QNetworkReply* reply, const QByteArray& body)
{
    if (!body.isEmpty()) return QString::fromUtf8(body);
    if (replyTimedOut(reply)) {
        return QCoreApplication::translate(
            "RequestTimeout",
            "The server didn't answer in time. Check your connection and try again.");
    }
    return reply ? reply->errorString() : QString();
}

} // namespace bsfchat::net
