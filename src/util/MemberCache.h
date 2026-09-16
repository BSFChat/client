#pragma once

#include <QString>
#include <QVector>

#include <string>

#include <bsfchat/MatrixTypes.h>

// The per-room member cache's storage rule.
//
// ServerConnection keeps m.room.member events per room so the member list, the
// server-wide member union, the ban list and the moderation scope can all be
// answered without a round trip. It used to APPEND every event it saw: a room
// the client stayed in for a day accumulated one entry per join, leave, avatar
// change and nickname edit, forever, and the vector was never pruned (U-M16).
//
// Every reader already reduced that history to one event per user before using
// it — each one wrote its own `latestByUser` pass and then read only the last
// value. None of them looks at the history. So the reduction belongs on the
// write side, where it also bounds the memory: after this, the cache holds at
// most one event per (room, user), which is the roster size.
//
// Because the stored vector is exactly the reduction the readers compute, their
// existing loops keep working untouched — they now simply find a single
// candidate per user where they used to find the newest of several.
//
// Extracted to a header so the rule can be tested without a network stack or
// an event loop. Newest-wins: events arrive in sync order, and a later event
// for a user is by definition their current state.
namespace bsfchat::client {

// Insert `event` into `bucket`, replacing any existing event with the same
// state key (the member's user id). Returns true if an existing entry was
// replaced, false if the event was appended or ignored.
//
// An event with no state key identifies no member. Every reader skips such an
// event, so it is dropped rather than stored — storing it would be pure growth.
inline bool upsertMemberEvent(QVector<bsfchat::RoomEvent>& bucket,
                              const bsfchat::RoomEvent& event)
{
    if (!event.state_key.has_value()) return false;
    const std::string& key = *event.state_key;
    for (auto& existing : bucket) {
        if (existing.state_key.has_value() && *existing.state_key == key) {
            existing = event;
            return true;
        }
    }
    bucket.append(event);
    return false;
}

}  // namespace bsfchat::client
