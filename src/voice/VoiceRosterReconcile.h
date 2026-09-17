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

// ---------------------------------------------------------------------
// What a roster row should SAY about a peer
// ---------------------------------------------------------------------
// `liveState` is the PeerConnectionManager's own state while we still
// hold one ("new" / "connecting" / "connected" / "disconnected" /
// "failed"); pass an empty string for a roster member we hold no peer
// connection for.
//
// The case this exists for: VoiceEngine tears a peer DOWN the instant it
// reports Failed, in the same slot that emits the state change. The UI
// therefore saw "failed" and, one statement later, saw the peer vanish
// from the map — after which the roster's lookup missed and fell back to
// the default, "new". The net visible state of a peer whose ICE had
// failed was indistinguishable from one that had only just been added.
//
// That is the wrong way round. ICE failure IS the interesting state: the
// mesh reconciler re-offers every 5 s (and only from the lesser-id side),
// so a peer that keeps failing sits there looking like it is merely still
// connecting, forever, with nothing anywhere saying otherwise. Remember
// that we gave up on it and keep saying so until a peer object exists for
// that user again.
inline QString peerDisplayState(const QString& liveState, bool gaveUp)
{
    if (!liveState.isEmpty()) return liveState;
    return gaveUp ? QStringLiteral("failed") : QStringLiteral("new");
}

} // namespace voice
