#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

// WHICH ROOMS A MODERATION ACTION HAS TO BE SENT TO.
//
// This exists apart from ServerConnection because the answer is different for
// each action and getting it wrong is invisible in the UI: a ban that fans out
// to forty rooms looks identical to one that does not, right up until the audit
// log has forty rows for one click. ServerConnection cannot be instantiated in
// a unit test, so the rule lives here and the caller is a loop over the result.
//
// The rule, and why it differs per action (authority:
// server/src/api/RoomHandler.cpp, apply_membership_moderation):
//
//   BAN / UNBAN are SERVER-WIDE, server-side, since migration v15. The server
//   keeps a real ban list (`server_bans`), enforced at /join, auto-join, /sync,
//   /invite and registration, and a ban also revokes every session the target
//   holds. POST /rooms/{roomId}/ban projects the membership across EVERY room
//   the target has a row in — including rooms this client has never synced —
//   and writes ONE audit record naming the room the request arrived through.
//   So the room id is audit context, nothing more, and one request is not just
//   sufficient but correct: N requests would be N audit rows describing one act
//   of authority, and the ban list is idempotent anyway.
//
//   KICK is PER-CHANNEL, deliberately, and matches Discord: it removes somebody
//   from one channel and they can walk straight back in. The server takes the
//   single-room branch for it (`store_.set_membership` + one member event) and
//   projects nothing. "Kick from the server" therefore genuinely is N requests,
//   one per channel the user is in, and it can only ever cover the channels
//   this client has synced. That gap is real but it is a kick, not a ban: the
//   user is not being kept out of anything, so a missed channel is a channel
//   they are still in rather than a hole in an enforcement boundary.
//
// This asymmetry used to be a single comment conceding that "rooms the client
// hasn't synced yet fall through the cracks — acceptable for v1", written when
// all three actions looped. That is now true of kick alone.
namespace bsfchat::client {

enum class ModerationAction {
    Kick,   // per-channel; one request per channel the target is in
    Ban,    // server-wide; exactly one request
    Unban,  // server-wide; exactly one request
};

// `membershipByRoom` — the target user's latest m.room.member membership value
// in each room this client has synced ("join", "invite", "leave", "ban", ...).
// Rooms the client has never seen simply are not keys; for ban and unban that
// no longer matters.
//
// Returns the room ids to send the request to, in a deterministic order (QMap
// iterates its keys sorted, so the chosen audit room does not depend on sync
// arrival order). Empty means "send nothing" — for ban/unban that only happens
// when this client knows of no rooms at all, in which case there is no room id
// to name and the request could not be formed.
inline QStringList moderationRooms(ModerationAction action,
                                   const QMap<QString, QString>& membershipByRoom)
{
    QStringList out;
    if (membershipByRoom.isEmpty()) return out;

    if (action == ModerationAction::Kick) {
        // Only channels the user is actually in. Kicking a non-member is a
        // server-side no-op, but it is also an audit row and a 403-shaped
        // round trip per empty channel.
        for (auto it = membershipByRoom.constBegin();
             it != membershipByRoom.constEnd(); ++it) {
            if (it.value() == QStringLiteral("join")
                || it.value() == QStringLiteral("invite")) {
                out << it.key();
            }
        }
        return out;
    }

    // Ban / unban: ONE request. The room only has to be a room, but preferring
    // one the target is actually in makes the audit record read sensibly
    // ("banned in #general") instead of naming whichever channel happens to
    // sort first. For an unban that means preferring a room the ban is visible
    // in; for a ban, any room the user is present in.
    const QString wanted = action == ModerationAction::Unban
        ? QStringLiteral("ban") : QStringLiteral("join");
    QString fallback;
    for (auto it = membershipByRoom.constBegin();
         it != membershipByRoom.constEnd(); ++it) {
        if (it.value() == wanted) return {it.key()};
        if (fallback.isEmpty()) fallback = it.key();
    }
    if (action == ModerationAction::Ban) {
        // Second choice for a ban: somebody who was invited but never joined
        // is still "present" in a way "leave" is not.
        for (auto it = membershipByRoom.constBegin();
             it != membershipByRoom.constEnd(); ++it) {
            if (it.value() == QStringLiteral("invite")) return {it.key()};
        }
    }
    return {fallback};
}

} // namespace bsfchat::client
