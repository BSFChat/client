#pragma once

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <bsfchat/MatrixTypes.h>
#include <nlohmann/json.hpp>

// "This room is gone for me" — the decisions behind it, kept free of the
// network, the models and the event loop so they can be tested on their own.
//
// A room leaves this client's sidebar for one of three reasons that the
// client cannot see happen: the channel was deleted, the account left / was
// kicked / was banned on another device, or a role change took VIEW_CHANNEL
// away. For a long time none of those reached other devices at all, and the
// room sat in the sidebar answering 403 forever (audit R-1). Two signals now
// cover it, and both are decided here:
//
//   * `rooms.leave` in an incremental /sync — the server naming the rooms the
//     reader must drop. The protocol library's SyncResponse has no field for
//     it (and the protocol repo cannot change in this release), so the client
//     reads it out of the raw body: parseSyncLeaves().
//   * GET /joined_rooms — the reconciliation for servers that send nothing.
//     roomsToDropAfterReconcile() turns "what the sidebar holds" and "what the
//     server says I am in" into the list to drop.
namespace bsfchat::client {

// One member event carried in a rooms.leave entry's timeline. Only the parts
// the join-vs-leave ordering needs.
struct SyncLeaveMember {
    QString userId;      // state_key
    QString membership;  // "leave", "ban", …
    qint64 ts = -1;      // origin_server_ts, -1 when absent
};

// One key of `rooms.leave`. The key is the whole instruction; everything else
// is informational and optional.
struct SyncLeave {
    QString roomId;
    // bsfchat.reason: "leave" | "kick" | "ban" | "deleted" | "no_access", or
    // empty when the server gave none.
    QString reason;
    QVector<SyncLeaveMember> members;
};

// Reads `rooms.leave` from a raw /sync body. Never throws: anything that is
// not the expected shape is skipped, entry by entry, so a malformed leave
// section costs at most the entries it garbled and never the sync itself
// (the caller parses the rest of the body separately and must not see this
// fail). An absent section — every server older than the one that added it —
// yields an empty list.
inline QVector<SyncLeave> parseSyncLeaves(const nlohmann::json& body) noexcept
{
    QVector<SyncLeave> out;
    try {
        if (!body.is_object()) return out;
        const auto rooms = body.find("rooms");
        if (rooms == body.end() || !rooms->is_object()) return out;
        const auto leave = rooms->find("leave");
        if (leave == rooms->end() || !leave->is_object()) return out;
        for (auto it = leave->begin(); it != leave->end(); ++it) {
            SyncLeave entry;
            entry.roomId = QString::fromStdString(it.key());
            if (entry.roomId.isEmpty()) continue;
            const auto& value = it.value();
            if (value.is_object()) {
                const auto reason = value.find("bsfchat.reason");
                if (reason != value.end() && reason->is_string())
                    entry.reason = QString::fromStdString(reason->get<std::string>());
                const auto timeline = value.find("timeline");
                if (timeline != value.end() && timeline->is_object()) {
                    const auto events = timeline->find("events");
                    if (events != timeline->end() && events->is_array()) {
                        for (const auto& ev : *events) {
                            if (!ev.is_object()) continue;
                            if (ev.value("type", std::string()) != "m.room.member") continue;
                            const auto sk = ev.find("state_key");
                            if (sk == ev.end() || !sk->is_string()) continue;
                            SyncLeaveMember m;
                            m.userId = QString::fromStdString(sk->get<std::string>());
                            const auto content = ev.find("content");
                            if (content != ev.end() && content->is_object()) {
                                const auto ms = content->find("membership");
                                if (ms != content->end() && ms->is_string())
                                    m.membership = QString::fromStdString(ms->get<std::string>());
                            }
                            const auto ts = ev.find("origin_server_ts");
                            if (ts != ev.end() && ts->is_number_integer())
                                m.ts = ts->get<qint64>();
                            entry.members.append(m);
                        }
                    }
                }
            }
            out.append(std::move(entry));
        }
    } catch (...) {
        // Shape checks above should make this unreachable; the guarantee to
        // the caller is "never throws", so it is kept here too.
    }
    return out;
}

// Which rooms named in `rooms.leave` really have to go, given the rest of the
// same response.
//
// A batch can carry a leave AND a re-join of one room (left on the phone,
// rejoined a second later). The rule: a room in both sections is kept only if
// rooms.join carries an own m.room.member "join" provably LATER than the own
// leave event in rooms.leave. When the leave carries no own member event —
// deletions and lost access usually don't — nothing can be proven later, and
// the server's statement that the room is gone wins.
inline QStringList roomsToDropForSyncLeaves(const QVector<SyncLeave>& leaves,
                                            const bsfchat::SyncResponse& response,
                                            const QString& ownUserId)
{
    QStringList out;
    QSet<QString> seen;
    const std::string own = ownUserId.toStdString();
    for (const auto& leave : leaves) {
        if (leave.roomId.isEmpty() || seen.contains(leave.roomId)) continue;
        seen.insert(leave.roomId);

        const auto joined = response.rooms.join.find(leave.roomId.toStdString());
        if (joined == response.rooms.join.end()) {
            out.append(leave.roomId);
            continue;
        }

        qint64 ownLeaveTs = -1;
        for (const auto& m : leave.members) {
            if (m.userId != ownUserId || m.membership == QLatin1String("join")) continue;
            if (m.ts > ownLeaveTs) ownLeaveTs = m.ts;
        }

        qint64 ownJoinTs = -1;
        auto scan = [&](const std::vector<bsfchat::RoomEvent>& events) {
            for (const auto& ev : events) {
                if (ev.type != "m.room.member" || !ev.state_key || *ev.state_key != own)
                    continue;
                if (!ev.content.data.is_object()) continue;
                const auto ms = ev.content.data.find("membership");
                if (ms == ev.content.data.end() || !ms->is_string()
                    || ms->get<std::string>() != "join")
                    continue;
                if (ev.origin_server_ts > ownJoinTs) ownJoinTs = ev.origin_server_ts;
            }
        };
        scan(joined->second.state.events);
        scan(joined->second.timeline.events);

        const bool rejoinedLater = ownLeaveTs >= 0 && ownJoinTs > ownLeaveTs;
        if (!rejoinedLater) out.append(leave.roomId);
    }
    return out;
}

// One sidebar row, as much of it as reconciliation needs.
struct SidebarRoom {
    QString roomId;
    bool isCategory = false;
    QString parentId;
};

// Which sidebar rooms to drop after GET /joined_rooms answered `joined`.
//
//   * A channel or DM that is not in `joined`, is not a pending invite and is
//     not `protectedIds` is gone.
//   * `protectedIds` are rooms that may have arrived AFTER the request was
//     issued (the caller passes every row it did not hold at request time).
//     An answer computed before a join cannot speak for that join.
//   * A category goes only when the server does not list it either AND no
//     channel that survives this pass still files under it. The server does
//     list joined categories today (they pass its visibility filter as
//     stubs), but an older or differently configured one may not, and a
//     category that vanished would re-file its visible children under
//     "uncategorised" — so the conservative reading is the only safe one.
inline QStringList roomsToDropAfterReconcile(const QVector<SidebarRoom>& sidebar,
                                             const QSet<QString>& joined,
                                             const QSet<QString>& pendingInvites,
                                             const QSet<QString>& protectedIds)
{
    auto exempt = [&](const QString& id) {
        return id.isEmpty() || joined.contains(id) || pendingInvites.contains(id)
            || protectedIds.contains(id);
    };

    QStringList out;
    QSet<QString> dropped;
    for (const auto& room : sidebar) {
        if (room.isCategory || exempt(room.roomId)) continue;
        out.append(room.roomId);
        dropped.insert(room.roomId);
    }

    QSet<QString> stillParenting;
    for (const auto& room : sidebar) {
        if (room.isCategory || dropped.contains(room.roomId)) continue;
        if (!room.parentId.isEmpty()) stillParenting.insert(room.parentId);
    }
    for (const auto& room : sidebar) {
        if (!room.isCategory || exempt(room.roomId)) continue;
        if (stillParenting.contains(room.roomId)) continue;
        out.append(room.roomId);
    }
    return out;
}

// Whether a failed /messages says the room is gone for us, as opposed to any
// other failure. Only these two errcodes trigger a /joined_rooms
// reconciliation; the reconciliation, not this, decides what is dropped.
inline bool messagesErrorSuggestsRoomGone(const QString& errorBody)
{
    const auto doc = QJsonDocument::fromJson(errorBody.toUtf8());
    if (!doc.isObject()) return false;
    const QString errcode = doc.object().value(QStringLiteral("errcode")).toString();
    return errcode == QLatin1String("M_FORBIDDEN") || errcode == QLatin1String("M_NOT_FOUND");
}

} // namespace bsfchat::client
