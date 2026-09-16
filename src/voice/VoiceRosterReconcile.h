#pragma once

// Mesh roster reconciliation (not to be confused with util/VoiceRoster.h,
// which is the SIDEBAR's occupancy model derived from m.call.member): the difference between the members the
// SERVER says are in the voice channel and the peer connections this
// client actually holds.
//
// The add side existed already (inline in ServerConnection's voice-member
// poll handler) but the remove side did not — V-M5. A peer that crashed,
// slept, or was reaped keeps its tile showing "connected" until ICE gives
// up on its own, which can be minutes. Both directions belong to the same
// comparison, so they are computed together here, as pure logic that a
// test can drive without a server or a peer connection.

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

namespace voice {

struct RosterReconcile {
    // Members on the roster we hold no peer for, and whose id sorts
    // ABOVE ours — the offer-direction rule (see VoiceEngine::start): for
    // any pair exactly one side offers, so re-offering from both sides
    // would manufacture glare on every poll tick.
    QStringList toOffer;
    // Peers we hold that the roster no longer lists. Dropping them is
    // what stops a crashed peer rendering as connected.
    QStringList toDrop;
};

// `members` is the GET /voice/members array. Rows carrying `active:false`
// are treated as absent: those are members the ghost reaper has already
// retired, and holding a peer for one is exactly the stale connection
// this prunes.
inline RosterReconcile reconcileRoster(const QJsonArray& members,
                                       const QStringList& heldPeers,
                                       const QString& localUserId)
{
    RosterReconcile out;
    QSet<QString> present;
    for (const auto& value : members) {
        const QJsonObject row = value.toObject();
        const QString uid = row.value(QStringLiteral("user_id")).toString();
        if (uid.isEmpty() || uid == localUserId) continue;
        // Absent `active` means an older server that only ever returns
        // live rows — treat it as active rather than pruning everyone.
        if (!row.value(QStringLiteral("active")).toBool(true)) continue;
        present.insert(uid);
        if (uid > localUserId && !heldPeers.contains(uid)) out.toOffer.append(uid);
    }
    for (const QString& held : heldPeers) {
        if (held.isEmpty() || held == localUserId) continue;
        if (!present.contains(held)) out.toDrop.append(held);
    }
    out.toOffer.sort();
    out.toDrop.sort();
    return out;
}

} // namespace voice
