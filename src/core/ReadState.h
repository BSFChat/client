#pragma once

// The channel-list unread dot, as arithmetic.
//
// The dot compares a room's newest message timestamp against a per-room
// "last read" timestamp the client persists. Both are origin_server_ts —
// SERVER clock. Nothing on the read side may ever be Date.now(): client skew
// then either hides a dot (client ahead) or pins one on (client behind).
//
// A room with no stored marker used to mean "never unread", which made the
// dot impossible for any channel the user had not opened yet. The obvious
// fix — treat no marker as "everything is unread" — greets a new user with a
// dot on every channel that has history. So the marker is SEEDED the first
// time /sync shows us a room, and what it is seeded to depends on what that
// batch was:
//
//   baseline  (initial full sync, or the cached snapshot being replayed):
//             the batch is history that predates this client. Seed to its
//             newest message, i.e. "caught up as of now".
//   live      (an incremental sync): the room is new TO US — a channel just
//             created, or one we were just added to. Everything in it is
//             unread, so seed below any real timestamp.
//
// Header-only and Qt-Core-only so tests/test_read_state.cpp can drive it
// without a Settings object or a QSettings file.

#include <QJsonObject>
#include <QJsonValue>
#include <QtGlobal>

namespace bsfchat::client {

// "Seen, nothing read." Positive so it is distinguishable from the absent
// marker (0), and smaller than any origin_server_ts a server will produce.
inline constexpr qint64 kReadSeedNothingRead = 1;

// The value to seed a first-seen room's marker with. newestMessageTs is the
// highest message origin_server_ts in the batch that introduced the room, or
// 0 if it carried none.
inline qint64 readSeedFor(bool baselineBatch, qint64 newestMessageTs)
{
    if (baselineBatch && newestMessageTs > kReadSeedNothingRead)
        return newestMessageTs;
    return kReadSeedNothingRead;
}

// ── The marker the SERVER holds ──────────────────────────────────────────
//
// /sync delivers it as `m.fully_read` room account data (server schema v30,
// server/docs/read-state.md), which is how a room read on the phone clears its
// dot on the desktop. The content is the spec's `event_id` plus this server's
// `bsfchat.origin_server_ts`.
//
// THE TIMESTAMP IS THE FIELD THIS CLIENT CAN USE. Every other read-side number
// here is an origin_server_ts, and the event the marker names is very often one
// this device has never loaded — the phone read the room, this one has a
// sidebar row for it and no timeline. Resolving the id would mean a /messages
// request per room per marker; the server resolved it once, when it wrote the
// document.
//
// Returns 0 for a document from a server that does not send the timestamp, for
// a malformed one, and for a non-positive value. 0 is "nothing to apply": the
// absence of a marker and a marker of zero are different things everywhere
// else in this file, and a caller must not turn one into the other.
inline qint64 serverReadMarkerTs(const QJsonObject& content)
{
    const QJsonValue ts = content.value(QStringLiteral("bsfchat.origin_server_ts"));
    if (!ts.isDouble()) return 0;
    const qint64 ms = static_cast<qint64>(ts.toDouble());
    return ms > 0 ? ms : 0;
}

// lastReadTs == 0 is a room /sync has not introduced yet (or a caller with
// no Settings wired). There is nothing to compare against, so no dot —
// seeding, not this function, is what makes never-opened rooms work.
inline bool isUnread(qint64 lastMessageTs, qint64 lastReadTs)
{
    if (lastMessageTs <= 0 || lastReadTs <= 0) return false;
    return lastMessageTs > lastReadTs;
}

// A READ MARKER ONLY EVER GOES FORWARD.
//
// The server already works this way — SqliteStore::set_read_marker resolves
// its upsert conflict with MAX — and the client did not, which let the two
// disagree about a room in the one direction a user notices.
//
// Several writers hand over a timestamp that is only the newest thing THEY
// can see, not the newest thing the user has read: the /sync handler passes
// the newest message in one batch, the room-switch persist passes the newest
// LOADED row, the view's at-bottom persist passes the newest row in a model
// that may still be filling. Any of those arriving after a further-on marker
// would re-light a dot the user had already cleared. Nothing in this client
// means "mark as unread", so there is no writer this rule can wrong.
//
// It is also what arbitrates between DEVICES, now that the server pushes its
// own marker down (serverReadMarkerTs above). A device that has been asleep
// wakes holding a stale marker and a stale view of the room; whichever of the
// two values is further on wins, so the sleeper cannot re-light a dot the
// phone cleared. The server applies the same rule to the value it stores —
// SqliteStore::set_read_marker resolves its upsert with MAX and reports
// whether it moved — so the two ends cannot disagree about which way forward
// is.
inline bool readMarkerAdvances(qint64 storedTs, qint64 candidateTs)
{
    return candidateTs > storedTs;
}

} // namespace bsfchat::client
