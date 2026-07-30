#pragma once

// Which transport does this client use for THIS voice join?
//
// Pure logic: no Qt objects beyond value types, no network, no I/O, no
// transport instances. It exists as its own unit precisely so the rule
// that prevents a permanently-broken call can be tested exhaustively
// without a server, an SFU or a second client.
//
// ---------------------------------------------------------------------
// The trap this exists to close
// ---------------------------------------------------------------------
// A mesh client joining a channel expects an m.call.invite from every
// other member, and offers to every member whose user id sorts above its
// own. An SFU participant never sends one — it talks to the LiveKit
// server, not to peers. So a channel containing both:
//
//   - the mesh client sits in "connecting" forever, for that member;
//   - the SFU participant hears the other SFU participants and is
//     simply deaf to the mesh ones;
//   - nothing errors, nothing times out into a useful message, and the
//     voice-member list shows everyone as present.
//
// Silent partial audio in a group call is about the worst failure shape
// available, so the rule is: **a voice channel is all-mesh or all-SFU.**
//
// And this is NOT a rare edge case. Android has no LiveKit SDK and stays
// on mesh permanently, while desktop moves to LiveKit. One Android user
// and one desktop user in the same voice channel is the ORDINARY case,
// so this decision runs on every join and has to be right by default.
//
// ---------------------------------------------------------------------
// Client-side is necessary but NOT sufficient
// ---------------------------------------------------------------------
// This class implements the client's half. It cannot be the enforcement
// point on its own, because two clients can evaluate it concurrently
// against the same empty channel and reach different answers — a
// desktop client sees an empty channel and picks LiveKit, an Android
// client sees the same empty channel and picks mesh, and both join.
// Only the server can serialise that. See kServerEnforcementNotes.

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace voice {

// Values for the `transport` field on m.call.member content. Wire
// strings: they appear in state events and in server policy, so they are
// API and must not be renamed.
inline constexpr char kTransportMesh[] = "mesh";
inline constexpr char kTransportLiveKit[] = "livekit";

enum class TransportChoice {
    Mesh,
    LiveKit,
    // The channel already holds members on the transport this client
    // cannot use. Joining would produce the silent-partial-audio
    // failure, so the join must be REFUSED with an explanation rather
    // than downgraded — a desktop client can fall back to mesh, but an
    // Android client cannot fall forward to LiveKit, and pretending
    // otherwise is how you ship the trap.
    Refuse,
};

struct TransportDecision {
    TransportChoice choice = TransportChoice::Mesh;
    // Populated for Refuse; empty otherwise. User-facing.
    QString refusalReason;

    bool isMesh() const { return choice == TransportChoice::Mesh; }
    bool isLiveKit() const { return choice == TransportChoice::LiveKit; }
    bool refused() const { return choice == TransportChoice::Refuse; }
};

struct TransportInputs {
    // This build can run a LiveKit transport at all. False on Android
    // and iOS (no SDK for those platforms) and false in any build
    // configured without BSFCHAT_ENABLE_LIVEKIT.
    bool clientSupportsLiveKit = false;

    // The server issued a LiveKit token for this channel. A 404 from
    // POST .../voice/livekit_token means `[voice.livekit]` is
    // unconfigured, which IS the capability probe — no separate
    // discovery endpoint and no version negotiation.
    bool serverOfferedLiveKitToken = false;

    // Current roster from GET .../voice/members. Rows carry `user_id`,
    // `active`, and (once the server writes it) `transport`.
    QJsonArray voiceMembers;

    // This client's own user id, so its own row — which may be stale
    // from a previous session that was reaped, or from another device —
    // does not veto its own join.
    QString localUserId;
};

// Counts of ACTIVE members per transport, excluding `localUserId`.
struct RosterTransports {
    int mesh = 0;
    int livekit = 0;
    // Active members whose row carries no `transport` field. Treated as
    // mesh: every client that exists today is a mesh client, and the
    // field is being added now. Guessing "livekit" for an unlabelled row
    // would break every already-deployed client on the first join.
    int unlabelled = 0;

    int meshTotal() const { return mesh + unlabelled; }
    bool empty() const { return meshTotal() == 0 && livekit == 0; }
};

// Classifies the roster. `active: false` rows are ignored — those are
// members the ghost reaper has already retired, and a stale row must not
// pin a channel to a transport nobody is using.
RosterTransports classifyRoster(const QJsonArray& voiceMembers,
                                const QString& localUserId);

// The decision. Deterministic and total: every input combination
// produces exactly one of the three outcomes.
TransportDecision chooseTransport(const TransportInputs& in);

// What the SERVER must enforce for this to actually hold. Kept next to
// the client rule so the two cannot drift, and written out because the
// server half is a separate task.
//
// 1. Persist the transport. Add `transport` to VoiceMemberContent in
//    protocol/include/bsfchat/MatrixTypes.h (currently: active, muted,
//    deafened, screen_sharing, camera_on, device_id, joined_at — no
//    transport field). Absent or empty MUST deserialise to "mesh", so
//    existing state events and already-deployed clients keep working.
//
// 2. Make the join declare its transport, and validate it. The client
//    tells the server which transport it is using at join time; the
//    server writes that into m.call.member rather than inferring it.
//    Reject any value that is not "mesh" or "livekit".
//
// 3. Serialise the decision server-side. This is the part the client
//    cannot do. Under the same lock that writes m.call.member:
//      - if the channel has ≥1 active member with transport "livekit",
//        a "mesh" join is REFUSED;
//      - if the channel has ≥1 active member with transport "mesh" (or
//        unlabelled), a "livekit" join is REFUSED;
//      - if the channel has no active members, the joiner's declared
//        transport becomes the channel's transport for the session.
//    Without the lock, two simultaneous joins into an empty channel
//    each see "empty" and establish conflicting transports.
//
// 4. Refuse the token, not just the join. handle_livekit_token must 404
//    (or 409) when the channel already has active mesh members, so a
//    client that ignores the join refusal still cannot connect to the
//    SFU. The token endpoint is the real capability gate and must carry
//    the same rule.
//
// 5. Keep the reaper per-transport, not global. The ghost reaper
//    (kHeartbeatTtl 30 s / kReapInterval 10 s) is what makes rule 3
//    self-healing: without it a crashed mesh client's stale `active:
//    true` row would pin the channel to mesh indefinitely and no
//    desktop client could ever use the SFU there. It must keep running
//    for mesh channels after LiveKit webhooks take over SFU liveness.
//
// 6. Report refusals distinguishably. A refusal is not "voice is
//    broken" — it is "this channel is currently mesh-only because
//    someone is on Android". The client needs to say that, so the error
//    must be distinguishable from a permission denial or an outage.
inline constexpr char kServerEnforcementNotes[] =
    "see VoiceTransportSelector.h for the six server-side rules";

} // namespace voice
