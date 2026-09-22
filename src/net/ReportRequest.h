#pragma once

#include <QJsonObject>
#include <QString>
#include <QUrl>

#include <optional>

// The two report endpoints, as pure path + body builders.
//
//   POST /_matrix/client/v3/rooms/{roomId}/report/{eventId}   {score, reason}
//   POST /_matrix/client/v3/users/{userId}/report             {reason}
//
// Both are Matrix spec paths, spec-shaped down to the field names. They are
// built here rather than inline in MatrixClient for the reason
// identity/OidcRequest.h and util/MediaUrl.h are: a wrong path or a mis-named
// field is a 400 or a 404 that only a live server can show you, and a builder
// that returns a value can be asserted in tests/test_report_request.cpp with no
// socket at all.
//
// ── What a report does, and what the client must not imply ───────────────
//
// Nothing. Server-side (server/src/api/ReportHandler.h) a report writes one row
// and one audit record. It does not redact the event, mute the sender, hide
// anything, or notify the reported account — every effect happens later, by
// hand, by a server administrator. Copy shown next to these calls must not
// promise otherwise: the control that stops it NOW is the block, and the two
// are offered together for exactly that reason.
namespace bsfchat::net {

// The score range the server accepts. Matrix defines it as "how bad", -100
// being the worst; this client sends no score at all (see reportEventBody), so
// the constants exist for the validation rather than for a control.
inline constexpr int kMinReportScore = -100;
inline constexpr int kMaxReportScore = 0;

// The server's `reason` ceiling, mirrored from server/src/api/InputLimits.h
// (kMaxReasonBytes) so an over-long reason is refused in the composer where it
// can still be edited, rather than by a 400 after the dialog has closed. BYTES,
// not characters — the server measures the UTF-8.
inline constexpr int kMaxReportReasonBytes = 1024;

inline QString reportEventPath(const QString& roomId, const QString& eventId)
{
    return QStringLiteral("/_matrix/client/v3/rooms/")
           + QString::fromUtf8(QUrl::toPercentEncoding(roomId))
           + QStringLiteral("/report/")
           + QString::fromUtf8(QUrl::toPercentEncoding(eventId));
}

inline QString reportUserPath(const QString& userId)
{
    return QStringLiteral("/_matrix/client/v3/users/")
           + QString::fromUtf8(QUrl::toPercentEncoding(userId))
           + QStringLiteral("/report");
}

// Both fields are optional server-side, and an EMPTY reason is sent as an
// absent key rather than as "". The server stores what it is given; a row whose
// reason is the empty string reads, in an administrator's queue, as a reason
// that was typed and then lost, while an absent one reads as what it is — a
// report filed with nothing said. (Same rule as createBot's display name.)
//
// `score` is std::nullopt for every call this client makes. There is no score
// control in the UI and inventing one would be asking a user to quantify
// something they came here to describe; the server's default (0) is then what
// the row carries.
inline QJsonObject reportEventBody(const QString& reason,
                                   std::optional<int> score = std::nullopt)
{
    QJsonObject body;
    const QString trimmed = reason.trimmed();
    if (!trimmed.isEmpty()) body.insert(QStringLiteral("reason"), trimmed);
    if (score.has_value()) body.insert(QStringLiteral("score"), *score);
    return body;
}

// The user-report endpoint carries no score — the server rejects nothing for
// sending one, it simply files every user report at the same score, and a
// client that sent one would be writing a field no reader could act on. Same
// reason the server parses this body with scored=false.
inline QJsonObject reportUserBody(const QString& reason)
{
    QJsonObject body;
    const QString trimmed = reason.trimmed();
    if (!trimmed.isEmpty()) body.insert(QStringLiteral("reason"), trimmed);
    return body;
}

// True when `reason` is within the server's byte ceiling. The UI disables its
// submit button on a false rather than letting the round trip fail.
inline bool reportReasonFits(const QString& reason)
{
    return reason.trimmed().toUtf8().size() <= kMaxReportReasonBytes;
}

} // namespace bsfchat::net
