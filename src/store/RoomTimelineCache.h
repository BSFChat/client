#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <bsfchat/MatrixTypes.h>

namespace bsfchat::client {

// The newest slice of every room's timeline, kept so that opening a channel
// does not have to ask the network what the client already knows.
//
// ── Why it exists ────────────────────────────────────────────────────────
//
// There is ONE MessageModel per connection and ServerConnection::setActiveRoom
// clears it, so switching channels threw away everything and re-fetched from
// zero — re-opening a channel you were reading one second ago cost exactly
// what opening it for the first time did. Measured against production
// (2026-09-23) that is 117 ms at p50 and 679 ms at p90 per switch, and more
// on a phone. store/LocalCache.h could not help: it holds state events only,
// never timeline.
//
// ── The freshness rule, which is the whole correctness argument ──────────
//
// A cached window may only be replayed when it is current at the HEAD of the
// room, because MessageModel::ingestHistoryEvents prepends a fetched page
// under rows that are already loaded. Replay a window that is missing newer
// messages and the next /messages page — which holds the newest events —
// would be prepended ABOVE them: week-old messages at the bottom, which is
// the pre-v0.0.37 bug the prepend was introduced to fix.
//
// Head-currency is a property of the SYNC, not of any one room, which is why
// freshness here is one flag and not one per room:
//
//   * An incremental /sync returns every event since the token, in stream
//     order, for every room the user can see, and when its scan is cut short
//     by the server's limit it deliberately does NOT advance the token past
//     what it returned (SyncEngine::build_incremental_sync). So an unbroken
//     incremental sync cannot leave a hole in any room.
//   * An initial /sync marks each room's timeline `limited` when more history
//     exists — truncated BACKWARDS, never forwards. Head-currency holds.
//
// So: every held window is current the moment a sync response has been
// processed, and stale until the first one has. A client that resumes from
// disk therefore comes up stale, serves the first channel open from the
// network as it always did, and is warm from then on — the first sync lands
// in a few hundred milliseconds, long before a user has picked a channel.
// Anything that breaks the chain (a token the server rejects, a sign-out)
// calls markStale() and the windows go unused rather than wrong.
//
// Best-effort throughout: losing a window costs a round trip, which is what
// the client used to pay every single time.
class RoomTimelineCache {
public:
    // Events kept per room. A screenful is ~20 rows and the open fill targets
    // 30, so this is several screens of scroll-back for a normal channel —
    // and for a channel that is mostly voice-membership churn it is the raw
    // events, most of which render nothing, which is exactly the case the
    // cache is most valuable in.
    static constexpr int kMaxEventsPerRoom = 400;

    // One event straight off a /sync timeline. Newest-last, deduplicated by
    // event id; the oldest are dropped once the room is at its cap.
    void appendLive(const QString& roomId, const bsfchat::RoomEvent& event)
    {
        Room& room = m_rooms[roomId];
        const QString id = QString::fromStdString(event.event_id);
        if (id.isEmpty() || room.ids.contains(id)) return;
        room.ids.insert(id);
        room.events.append(event);
        trim(room);
    }

    // One /messages page, oldest-first, for a room whose window we may
    // already hold. Pages arrive newest-first across a fill, so each one goes
    // in front of the last.
    void prependHistory(const QString& roomId,
                        const QVector<bsfchat::RoomEvent>& chronological)
    {
        if (chronological.isEmpty()) return;
        Room& room = m_rooms[roomId];
        // A window we cannot vouch for the head of cannot have older events
        // ordered against it — the page might hold messages NEWER than
        // everything in it. Start the window over from this page instead.
        if (!m_fresh) {
            room.events.clear();
            room.ids.clear();
        }
        QVector<bsfchat::RoomEvent> fresh;
        fresh.reserve(chronological.size());
        for (const auto& event : chronological) {
            const QString id = QString::fromStdString(event.event_id);
            if (id.isEmpty() || room.ids.contains(id)) continue;
            room.ids.insert(id);
            fresh.append(event);
        }
        if (fresh.isEmpty()) return;
        room.events = fresh + room.events;
        trim(room);
    }

    // A window loaded from disk. Replaces whatever is held for the room.
    void adopt(const QString& roomId, QVector<bsfchat::RoomEvent> chronological)
    {
        if (chronological.isEmpty()) return;
        Room room;
        room.events = std::move(chronological);
        for (const auto& event : room.events)
            room.ids.insert(QString::fromStdString(event.event_id));
        trim(room);
        m_rooms.insert(roomId, std::move(room));
    }

    // The room's window, oldest-first, or nullptr when none is held. Callers
    // must check isFresh() before replaying it into a model — see the header.
    const QVector<bsfchat::RoomEvent>* window(const QString& roomId) const
    {
        auto it = m_rooms.constFind(roomId);
        if (it == m_rooms.constEnd() || it->events.isEmpty()) return nullptr;
        return &it->events;
    }

    // May the held windows be replayed? False until a sync response has been
    // processed, and again after anything that could have left a hole.
    bool isFresh() const { return m_fresh; }
    void markFresh() { m_fresh = true; }
    void markStale() { m_fresh = false; }

    QStringList roomIds() const { return m_rooms.keys(); }
    int eventCount(const QString& roomId) const
    {
        auto it = m_rooms.constFind(roomId);
        return it == m_rooms.constEnd() ? 0 : static_cast<int>(it->events.size());
    }

    void forget(const QString& roomId) { m_rooms.remove(roomId); }
    void clear()
    {
        m_rooms.clear();
        m_fresh = false;
    }

private:
    struct Room {
        QVector<bsfchat::RoomEvent> events;  // oldest first
        QSet<QString> ids;
    };

    static void trim(Room& room)
    {
        const int excess = static_cast<int>(room.events.size()) - kMaxEventsPerRoom;
        if (excess <= 0) return;
        // The OLDEST go: a window is only ever replayed as "the newest slice",
        // and its head-currency is the property the whole class rests on.
        for (int i = 0; i < excess; ++i)
            room.ids.remove(QString::fromStdString(room.events[i].event_id));
        room.events.remove(0, excess);
    }

    QHash<QString, Room> m_rooms;
    bool m_fresh = false;
};

} // namespace bsfchat::client
