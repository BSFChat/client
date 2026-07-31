#pragma once

#include <QString>
#include <QVector>

#include <algorithm>

// Who is currently in a voice channel, derived from m.call.member room state.
//
// Extracted from ServerConnection/RoomListModel so the membership rule can be
// tested without a network stack, an event loop or a server. The rule is the
// whole defect: the sidebar used to show a count that came from the size of
// whatever the last /voice/members poll happened to return, cached in a
// snapshot that was only rebuilt at the end of a /sync pass. A user who left
// kept their seat in the sidebar because nothing ever recomputed it.
//
// The invariant here is deliberately narrow and load-bearing:
//
//     the roster contains exactly the users whose newest m.call.member
//     carries active == true, and nothing else.
//
// There is no "was here", no tombstone, and no separately-stored count. A
// participant leaving is represented by removal, so the count cannot disagree
// with the list — they are the same fact read two ways.
namespace bsfchat::client {

// One participant's live voice state, as carried by m.call.member content.
// `active` is not stored: an inactive member is simply absent.
struct VoiceParticipant {
    QString userId;
    bool muted = false;
    bool deafened = false;
    bool cameraOn = false;
    bool screenSharing = false;
    // Server-assigned join time (ms). Only used for render order; zero for
    // servers/events that don't set it, which sorts them first by user id.
    qint64 joinedAt = 0;

    friend bool operator==(const VoiceParticipant&,
                           const VoiceParticipant&) = default;
};

using VoiceRoster = QVector<VoiceParticipant>;

// Deterministic render order: join order, ties broken by user id so the list
// never reshuffles between two polls that carry the same set. Stability here
// is what stops the sidebar flickering on the 5 s member poll.
inline void sortVoiceRoster(VoiceRoster& roster)
{
    std::sort(roster.begin(), roster.end(),
              [](const VoiceParticipant& a, const VoiceParticipant& b) {
                  if (a.joinedAt != b.joinedAt) return a.joinedAt < b.joinedAt;
                  return a.userId < b.userId;
              });
}

// Apply ONE m.call.member state event. `active` is the event's active flag —
// false removes the user, true inserts or updates them. Returns true iff the
// roster actually changed, so callers can skip the repaint (and the sidebar
// snapshot rebuild) when a poll or a re-delivered state event says nothing new.
inline bool applyCallMember(VoiceRoster& roster,
                            const VoiceParticipant& participant,
                            bool active)
{
    if (participant.userId.isEmpty()) return false;

    auto it = std::find_if(roster.begin(), roster.end(),
                           [&](const VoiceParticipant& p) {
                               return p.userId == participant.userId;
                           });

    if (!active) {
        if (it == roster.end()) return false;
        roster.erase(it);
        return true;
    }

    if (it != roster.end()) {
        if (*it == participant) return false;
        *it = participant;
        sortVoiceRoster(roster);
        return true;
    }

    roster.append(participant);
    sortVoiceRoster(roster);
    return true;
}

} // namespace bsfchat::client
