#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

// One reading of a Matrix error response: status, errcode, the human message,
// and — the part that had no home before — how long a 429 is asking us to wait.
//
// The client already decoded M_LIMIT_EXCEEDED in exactly one place: an inline
// lambda in ServerConnection's sendMessageError handler, which cannot be
// reached from a test (ServerConnection needs a network and an event loop) and
// which was the only thing in the client that knew `retry_after_ms` existed.
// Every other endpoint's failure went through decodeMatrixError in
// MatrixClient.cpp, which reads `error` and `errcode` and throws the rest away
// — so a rate-limited report or block would have surfaced as the server's bare
// sentence with no wait in it, and a user who pressed the button again would
// have been refused again for a reason nothing on screen explained.
//
// The wait arrives in two places and this reads BOTH, because the server sends
// both (server/src/http/RateLimitResponse.h): `retry_after_ms` in the Matrix
// body, and Retry-After in whole seconds in the header. The body wins when it
// is present — it is the more precise of the two — and the header is the
// fallback for a refusal written by something in front of the server, which is
// the case where there is no Matrix body at all.
//
// Deliberately parse-only. It returns no user-facing sentence, because the
// sentence is not the same for every caller: the composer says "Slow down —
// retry in 5s" about a message and a report dialog must not. Each call site
// writes its own copy from retrySeconds(); what may not differ between them is
// the reading of the wire.
namespace bsfchat::net {

struct MatrixFailure {
    // HTTP status, or 0 when the request never got one (transport failure).
    int status = 0;
    // The Matrix errcode, empty when the body was not a Matrix error object.
    QString errcode;
    // What to show a user: the body's `error`, or the errcode, or the
    // transport's own description — in that order, so something is always said.
    QString message;
    // Milliseconds the server asked us to wait; -1 when it asked for nothing.
    // Set for any refusal that carried a wait, not only a 429 — a proxy may
    // send Retry-After with something else.
    qint64 retryAfterMs = -1;

    bool isRateLimited() const
    {
        return status == 429 || errcode == QLatin1String("M_LIMIT_EXCEEDED");
    }

    // The wait in whole seconds, rounded UP, or 0 when none was given.
    //
    // Rounded up because telling somebody to come back a fraction of a second
    // early just earns them a second refusal — the same rounding the server
    // applies to its own Retry-After header, so the two agree.
    int retrySeconds() const
    {
        if (retryAfterMs <= 0) return 0;
        return int((retryAfterMs + 999) / 1000);
    }
};

// `retryAfterHeader` is the raw Retry-After value, empty when absent. Only the
// delta-seconds form is understood; the HTTP-date form is ignored rather than
// mis-parsed into a wait of zero, which would read as "no wait" and is the one
// answer that is definitely wrong.
//
// `transportError` is QNetworkReply::errorString(), used only when the body
// says nothing — a DNS failure or a dropped connection has no Matrix object in
// it, and "" is not a message.
inline MatrixFailure parseMatrixFailure(int status, const QByteArray& body,
                                        const QByteArray& retryAfterHeader = {},
                                        const QString& transportError = {})
{
    MatrixFailure f;
    f.status = status;

    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isObject()) {
        const QJsonObject o = doc.object();
        f.errcode = o.value(QStringLiteral("errcode")).toString();
        f.message = o.value(QStringLiteral("error")).toString();
        const QJsonValue retry = o.value(QStringLiteral("retry_after_ms"));
        // isDouble(), not toDouble() on whatever is there: a string
        // "5000" would otherwise read as 0, which is indistinguishable
        // from "the server named no wait".
        if (retry.isDouble()) f.retryAfterMs = qint64(retry.toDouble());
    }

    if (f.retryAfterMs < 0 && !retryAfterHeader.isEmpty()) {
        bool ok = false;
        const qint64 seconds = retryAfterHeader.trimmed().toLongLong(&ok);
        if (ok && seconds >= 0) f.retryAfterMs = seconds * 1000;
    }

    if (f.message.isEmpty()) f.message = f.errcode;
    if (f.message.isEmpty()) f.message = transportError;
    return f;
}

} // namespace bsfchat::net

// Registered so the type can cross a queued connection. Every use today is a
// direct connection inside one thread, where this is not needed — it is here
// so that a future move of MatrixClient onto its own thread fails loudly at
// registration rather than silently dropping the signal.
Q_DECLARE_METATYPE(bsfchat::net::MatrixFailure)
