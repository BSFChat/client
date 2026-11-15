#pragma once

// Who the mesh may connect to (V-8). An m.call.invite is just a room event:
// anyone in the room can send one, and answering it wires our microphone
// (and any video) to the sender. Without a gate, a room member who never
// joined the voice channel could listen in, hidden from the roster, by
// re-inviting every few seconds.
//
// The gate is the voice channel's m.call.member roster: an invite is only
// answered when its sender is on it. Joins race — a newcomer's invite can
// reach us before the sync that carries their m.call.member — so an invite
// from someone not (yet) on the roster is held for a short grace period and
// answered if they appear, rejected if not. And when someone we hold a
// peer for leaves the roster, that peer is torn down at once rather than on
// the next 5 s /voice/members reconcile.
//
// Pure logic, testable without a server, event loop or peer connection.

#include <QSet>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace voice {

inline constexpr qint64 kInviteRosterGraceMs = 3000;

enum class InviteGate {
    Accept,  // sender is on the roster
    Hold,    // not yet; keep the invite until the grace period ends
    Reject,  // grace period over and still not on the roster
};

// `firstHeldMs` is when the invite (or the first held invite from this
// sender) arrived; pass `nowMs` for a fresh one.
inline InviteGate gateInvite(const QSet<QString>& roster, const QString& sender,
                             qint64 firstHeldMs, qint64 nowMs,
                             qint64 graceMs = kInviteRosterGraceMs)
{
    if (!sender.isEmpty() && roster.contains(sender)) return InviteGate::Accept;
    return nowMs - firstHeldMs < graceMs ? InviteGate::Hold : InviteGate::Reject;
}

struct RosterGateUpdate {
    // Peers to tear down: on the previous roster, gone from the new one.
    // Only that transition counts — a peer we offered to because the
    // /voice/members poll listed them, whose m.call.member has not synced
    // yet, is left to the reconciler rather than dropped and re-offered in
    // a loop.
    QStringList toDrop;
    // Held inviters who are now on the roster: answer them.
    QStringList toAdmit;
};

inline RosterGateUpdate applyRosterUpdate(const QSet<QString>& previous,
                                          const QSet<QString>& next,
                                          const QStringList& heldPeers,
                                          const QStringList& heldInviters)
{
    RosterGateUpdate out;
    for (const QString& peer : heldPeers)
        if (previous.contains(peer) && !next.contains(peer)) out.toDrop.append(peer);
    for (const QString& inviter : heldInviters)
        if (next.contains(inviter)) out.toAdmit.append(inviter);
    out.toDrop.sort();
    out.toAdmit.sort();
    return out;
}

} // namespace voice
