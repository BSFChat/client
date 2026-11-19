#pragma once

#include "util/JsonSafe.h"

#include <bsfchat/Constants.h>
#include <bsfchat/MatrixTypes.h>

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

// Invites the reader has not answered yet, as the sidebar shows them
// (audit 3, #6).
//
// /sync's rooms.invite restates the COMPLETE pending set on every delivered
// response (see SyncResponse in the protocol header), so the list is rebuilt
// from each response rather than patched. Each entry carries the stripped
// invite_state the server allows an invitee to see: the room's own name/type
// and the member events of the invitee and the inviter.
//
// Header-only and free of ServerConnection so it can be tested without a
// network or an event loop.
namespace bsfchat::client {

struct PendingInvite {
    QString roomId;
    QString roomName;    // m.room.name, may be empty
    QString roomType;    // bsfchat.room.type "type", e.g. "text" / "voice"
    QString inviterId;   // sender of the invitee's member event
    QString inviterName; // inviter's displayname from their member event
    bool isDirect = false;
};

inline PendingInvite pendingInviteFrom(const QString& roomId,
                                       const bsfchat::InvitedRoom& room,
                                       const QString& selfId)
{
    namespace js = bsfchat::jsonsafe;
    PendingInvite out;
    out.roomId = roomId;
    const std::string self = selfId.toStdString();
    // Pass 1: the room-level events and our own membership (whose sender is
    // the inviter and which carries is_direct).
    for (const auto& ev : room.invite_state.events) {
        const auto& c = ev.content.data;
        if (!c.is_object()) continue;
        if (ev.type == bsfchat::event_type::kRoomName) {
            out.roomName = js::qstr(c, "name");
        } else if (ev.type == bsfchat::event_type::kRoomType) {
            out.roomType = js::qstr(c, "type");
        } else if (ev.type == bsfchat::event_type::kRoomMember && ev.state_key
                   && *ev.state_key == self
                   && js::str(c, "membership") == "invite") {
            out.inviterId = QString::fromStdString(ev.sender);
            out.isDirect = js::boolean(c, "is_direct", false);
        }
    }
    // Pass 2: the inviter's own member event, for their display name.
    if (!out.inviterId.isEmpty()) {
        const std::string inviter = out.inviterId.toStdString();
        for (const auto& ev : room.invite_state.events) {
            if (ev.type != bsfchat::event_type::kRoomMember || !ev.state_key
                || *ev.state_key != inviter || !ev.content.data.is_object())
                continue;
            out.inviterName = js::qstr(ev.content.data, "displayname");
        }
    }
    return out;
}

// The ids in `now` that were not in `before`: the invites to announce. An
// invite that disappears and comes back (declined elsewhere, re-sent) is
// announced again, which is what the inviter meant.
inline QStringList newlyInvited(const QSet<QString>& before, const QVector<PendingInvite>& now)
{
    QStringList out;
    for (const auto& inv : now)
        if (!before.contains(inv.roomId)) out.append(inv.roomId);
    return out;
}

} // namespace bsfchat::client
