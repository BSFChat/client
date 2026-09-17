#pragma once

// "Hide my IP address" — what it decides, and what the UI is allowed to say
// about it.
//
// ── The leak this addresses ───────────────────────────────────────────────
// Voice and video are a peer-to-peer mesh. A peer connection is established by
// exchanging ICE candidates, and a candidate is an address: the host ones are
// this machine's LAN address, the server-reflexive ones its public IP. So being
// in a call with somebody means telling them where you are. That is inherent to
// P2P and cannot be fixed by hiding a field — the packets have to go somewhere.
//
// What CAN be fixed is making it a choice. With `iceTransportPolicy = Relay`
// libdatachannel gathers relay candidates only: the client never learns, offers
// or sends a host or server-reflexive candidate, every packet goes through the
// TURN server, and the far side sees the TURN server's address instead of ours.
//
// ── Why the decision lives here and not in VoiceEngine ────────────────────
// It is a five-input truth table — user setting, per-share override, the
// server's allow_p2p, whether any TURN server exists, and whether the caller is
// starting or already running — whose wrong answers are all silent. Pick All
// when the user asked for Relay and their address goes out with nothing said.
// Pick Relay with no TURN and ICE gathers nothing at all: every peer sits in
// Connecting until the 30 s watchdog reaps it, and the user is told nothing
// either. Neither shows up in a log anyone reads.
//
// Pure logic, no Qt beyond value types, no libdatachannel — so every row of
// that table is asserted in tests/test_ip_privacy.cpp without an audio device,
// a network or a TURN server.

#include "voice/VoiceStartPolicy.h"

#include <QString>

namespace voice {

// The user's standing preference (Settings::voiceRelayMode).
enum class RelayMode {
    // Follow the server. P2P when it allows it, relay when it does not.
    // This is the default and the one that gives the best media quality.
    Auto,
    // Always relay, whatever the server allows. Unilateral: it works even
    // when the peer on the other side stays on P2P, because it constrains
    // what WE gather and send. Their candidates remain theirs to expose.
    RelayOnly,
};

inline RelayMode relayModeFromString(const QString& s) {
    return s == QLatin1String("relayOnly") ? RelayMode::RelayOnly : RelayMode::Auto;
}

inline QString relayModeToString(RelayMode mode) {
    return mode == RelayMode::RelayOnly ? QStringLiteral("relayOnly")
                                        : QStringLiteral("auto");
}

// What rtc::Configuration::iceTransportPolicy ends up as. Mirrored rather than
// using rtc::TransportPolicy directly so this header — and its tests — do not
// need libdatachannel.
enum class IcePolicy { All, Relay };

// Who asked for relay-only. This is not bookkeeping: it decides which sentence
// the user is shown when relay-only cannot be honoured, and "your server
// requires this and is misconfigured" versus "the setting you turned on cannot
// work here" send people to completely different places.
enum class RelaySource {
    None,      // not relay-only
    Server,    // allow_p2p = false
    UserSetting, // Settings → "Hide my IP address"
    ShareOption, // the per-share "Hide my IP while sharing" option
};

struct IcePolicyInputs {
    RelayMode userMode = RelayMode::Auto;
    // True while a share (screen or camera) that asked to hide the IP is
    // active. One PeerConnection carries voice AND both video m-lines, so
    // honouring a per-share option means the whole connection is relay-only for
    // as long as that share lasts — there is no way to relay the video and keep
    // the voice direct.
    bool shareHidesIp = false;
    // The server's `allow_p2p` from GET /voip/turnServer.
    bool serverAllowsP2P = true;
    // Whether the built ICE configuration contains at least one TURN server.
    // Checked against the BUILT configuration, not the raw JSON, so it reflects
    // what libdatachannel would actually receive (bad URI schemes are dropped
    // on the way).
    bool hasTurn = false;
};

struct IcePolicyDecision {
    IcePolicy policy = IcePolicy::All;
    // Non-None means the session must be refused. Relay-only with nothing to
    // relay through is a guaranteed, silent, total failure, and falling back to
    // P2P instead would publish the address the user just asked to hide — the
    // one outcome that is worse than not connecting.
    StartRefusal refusal = StartRefusal::None;
    // Which input forced Relay. `RelaySource::None` when the policy is All.
    RelaySource source = RelaySource::None;

    bool relayOnly() const { return policy == IcePolicy::Relay; }
    bool refused() const { return refusal != StartRefusal::None; }
    // True when relay was OUR choice rather than the server's. The shield
    // indicator is shown for this; a server-wide relay policy is not a personal
    // privacy setting and must not be presented as one.
    bool localChoice() const {
        return source == RelaySource::UserSetting || source == RelaySource::ShareOption;
    }
};

inline IcePolicyDecision decideIcePolicy(const IcePolicyInputs& in) {
    IcePolicyDecision out;

    // Order matters for the MESSAGE, not for the policy. All three sources
    // produce the same Relay policy; which one is named in a refusal is chosen
    // most-specific-first, because the per-share option is the thing the user
    // just clicked and the user setting is the thing they can go and change.
    if (in.shareHidesIp) {
        out.source = RelaySource::ShareOption;
    } else if (in.userMode == RelayMode::RelayOnly) {
        out.source = RelaySource::UserSetting;
    } else if (!in.serverAllowsP2P) {
        out.source = RelaySource::Server;
    }

    if (out.source == RelaySource::None) return out; // P2P, nothing to check

    out.policy = IcePolicy::Relay;
    if (!in.hasTurn) {
        // Same refusal the server-wide relay policy has always produced. The
        // cause is identical — Relay with no relay — and only the wording
        // differs, which refusalMessage() handles from `source`.
        out.refusal = StartRefusal::RelayOnlyNoTurn;
    }
    return out;
}

// The refusal text, specialised by who asked. The one-argument
// refusalMessage() in VoiceStartPolicy.h keeps the server wording, which is
// what every existing caller means.
QString relayRefusalMessage(RelaySource source);

// ── What the UI may claim ────────────────────────────────────────────────
//
// The truthfulness rule, stated once: the interface may say an address is
// hidden only when the local policy is Relay AND the selected candidate pair
// really is relayed. Those are two different facts. The first is a request, the
// second is an outcome, and between them sit gathering, the TURN allocation and
// the connectivity checks — several seconds in which the first is true and the
// second is not yet.
//
// This project has already shipped a media badge that claimed more than the
// transport delivered (it read "MLS · 256" over DTLS+SCTP) and had to correct
// it. tests/test_voice_encryption.cpp exists because of that, and these strings
// are placed here, behind a Q_PROPERTY, for the same reason: a label composed in
// QML is a label nothing can check.
enum class IpExposure {
    // Policy is All. Peers in the call learn this machine's addresses, and the
    // UI says so rather than saying nothing.
    Shared,
    // Policy is Relay but not every live peer connection has been confirmed to
    // have selected a relayed path yet — still gathering, or a pair that
    // somehow did not. Claims nothing.
    Pending,
    // Policy is Relay and every live peer connection selected a relayed local
    // candidate. This is the only state in which "hidden" may be said.
    Hidden,
};

// `peersTotal` counts peer connections currently held; `peersRelayed` those
// whose SELECTED LOCAL candidate is of type Relayed. With no peers at all the
// answer is Pending, not Hidden: there is nothing to be hidden from yet, and a
// shield that lights up before a single connection exists teaches people to
// trust it when it means nothing.
inline IpExposure ipExposure(bool localPolicyIsRelay, int peersTotal, int peersRelayed) {
    if (!localPolicyIsRelay) return IpExposure::Shared;
    if (peersTotal > 0 && peersRelayed >= peersTotal) return IpExposure::Hidden;
    return IpExposure::Pending;
}

// Short label for the voice-dock shield. Empty for Shared — the dock shows no
// indicator at all rather than an "exposed" badge, because the default state is
// not a warning.
QString ipPrivacyBadge(IpExposure exposure);
// The sentence behind it, including the cost. Never empty.
QString ipPrivacyDetail(IpExposure exposure);

// ── Per-peer path, for the diagnostics overlay ───────────────────────────
//
// Read from libdatachannel's getSelectedCandidatePair. A path is relayed when
// EITHER end of the selected pair is a Relayed candidate — that is what "is
// this call going through the server" means, and it is a different question
// from "is MY address hidden", which depends only on the local end.
enum class PeerPath { Unknown, Direct, Relayed };

// The vocabulary QML renders verbatim, alongside peerState's. Fixed, like
// peerState's: "" / "direct" / "relayed".
inline QString peerPathName(PeerPath path) {
    switch (path) {
    case PeerPath::Direct:  return QStringLiteral("direct");
    case PeerPath::Relayed: return QStringLiteral("relayed");
    case PeerPath::Unknown: break;
    }
    return QString();
}

} // namespace voice
