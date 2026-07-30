#pragma once

#include <optional>

// The mention-badge decision for one room in one /sync batch.
//
// Extracted from ServerConnection::processSyncResponse so it can be tested
// without a network stack, an event loop, or a server: the interesting part is
// not "does the badge update" but which of three mutually-exclusive things
// happens, and getting that wrong is how the badge ends up doubled on launch or
// stuck under-reporting after a resume.
namespace bsfchat::client {

struct MentionBadgeUpdate {
    enum class Action {
        // Leave the badge alone. Happens when the server said nothing about this
        // room and we witnessed no mentions arriving — re-asserting a value we
        // don't have would either clobber a good count or repaint for nothing.
        None,
        // Overwrite with an absolute count. The server's
        // unread_notifications.highlight_count, which is computed against the
        // SERVER-side read marker and therefore includes mentions this client
        // never saw arrive (the whole point).
        SetAbsolute,
        // Add to the existing count. Legacy fallback for a server that reports
        // no highlight_count; can only ever count witnessed arrivals, so it
        // under-reports for a session that resumed from a persisted token.
        Increment,
    };

    Action action = Action::None;
    int value = 0;

    friend bool operator==(const MentionBadgeUpdate&,
                           const MentionBadgeUpdate&) = default;
};

// `isActiveRoom` — the room is on screen, so everything in it is read by
//   definition and the badge is forced to zero (the read marker sent alongside
//   is what makes the server agree on the next poll).
// `serverHighlightCount` — unread_notifications.highlight_count, absent for a
//   pre-highlight_count server or a cache-hydration replay (the local snapshot
//   doesn't persist it).
// `witnessedMentions` — mentions of the local user seen arriving in this batch,
//   already filtered for hydration/read-marker by the caller.
//
// Note the absolute branch needs NO hydration guard: re-applying the same count
// is idempotent, unlike an increment. That asymmetry is the reason this is one
// function rather than three call sites.
inline MentionBadgeUpdate mentionBadgeUpdate(bool isActiveRoom,
                                             std::optional<int> serverHighlightCount,
                                             int witnessedMentions)
{
    if (isActiveRoom) return {MentionBadgeUpdate::Action::SetAbsolute, 0};
    if (serverHighlightCount.has_value()) {
        int v = *serverHighlightCount;
        if (v < 0) v = 0;
        return {MentionBadgeUpdate::Action::SetAbsolute, v};
    }
    if (witnessedMentions > 0) {
        return {MentionBadgeUpdate::Action::Increment, witnessedMentions};
    }
    return {};
}

} // namespace bsfchat::client
