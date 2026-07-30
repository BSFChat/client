#include "voice/VoiceTransportSelector.h"

namespace voice {

RosterTransports classifyRoster(const QJsonArray& voiceMembers,
                                const QString& localUserId) {
    RosterTransports out;
    for (const QJsonValue& v : voiceMembers) {
        if (!v.isObject()) {
            continue;
        }
        const QJsonObject row = v.toObject();

        // Ignore rows the reaper has already retired. A crashed client's
        // stale row must not pin the channel to a transport nobody is
        // using. Note the default: a row with NO `active` key counts as
        // inactive, because GET .../voice/members always sets it and a
        // row without it is malformed rather than implicitly live.
        if (!row.value(QStringLiteral("active")).toBool(false)) {
            continue;
        }

        // Never let our own row veto our own join. It may be stale from
        // a previous session, or belong to another of this user's
        // devices — and LiveKit identity is `@user:server|DEVICE`
        // precisely because one user can have several participants.
        const QString uid = row.value(QStringLiteral("user_id")).toString();
        if (!localUserId.isEmpty() && uid == localUserId) {
            continue;
        }

        const QString transport = row.value(QStringLiteral("transport")).toString();
        if (transport == QLatin1String(kTransportLiveKit)) {
            ++out.livekit;
        } else if (transport == QLatin1String(kTransportMesh)) {
            ++out.mesh;
        } else {
            // Absent, empty, or an unrecognised value. Counts as mesh.
            //
            // Every client deployed today is a mesh client and none of
            // them write this field, so "unknown means mesh" is the only
            // choice that keeps existing calls working. It is also the
            // safe direction: mis-classifying a mesh member as LiveKit
            // would let a mesh joiner in alongside SFU participants and
            // produce exactly the silent partial audio this whole file
            // exists to prevent. Mis-classifying the other way only
            // costs an SFU client a fallback to mesh.
            ++out.unlabelled;
        }
    }
    return out;
}

TransportDecision chooseTransport(const TransportInputs& in) {
    const RosterTransports roster = classifyRoster(in.voiceMembers, in.localUserId);

    // Can this client use LiveKit at all for this join? Needs BOTH a
    // build that has the SDK and a server that issued a token. Either
    // one missing means mesh is the only option available to us.
    const bool liveKitAvailable =
        in.clientSupportsLiveKit && in.serverOfferedLiveKitToken;

    if (!liveKitAvailable) {
        // Mesh is all we can do. If the channel is already SFU, joining
        // would be silently half-broken, so refuse instead.
        //
        // Refuse rather than "join anyway and hope": an Android client
        // has no fall-forward. This is the branch that will actually
        // fire in production, because Android stays on mesh while
        // desktop moves to LiveKit.
        if (roster.livekit > 0) {
            return {TransportChoice::Refuse,
                    QStringLiteral(
                        "This voice channel is using the server's SFU, which "
                        "this device can't join. Someone already in the call "
                        "must leave before a mesh client can connect.")};
        }
        return {TransportChoice::Mesh, {}};
    }

    // We could use either. Existing members decide, because they cannot
    // change transport mid-call and we can.
    if (roster.meshTotal() > 0) {
        return {TransportChoice::Mesh, {}};
    }

    // Includes the empty-channel case: nobody is here, so we set the
    // transport for the session. This is the branch the SERVER must
    // serialise — two clients evaluating an empty channel concurrently
    // can reach different answers and both join. The client cannot fix
    // that; see kServerEnforcementNotes rule 3.
    return {TransportChoice::LiveKit, {}};
}

} // namespace voice
