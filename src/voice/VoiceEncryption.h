#pragma once

// Media-encryption state for a voice session, and the words we are allowed
// to describe it with.
//
// =====================================================================
// READ THIS BEFORE WRITING ANY STRING ABOUT VOICE SECURITY
// =====================================================================
// The SFU path encrypts media with a shared key minted by the BSFChat
// server and delivered over TLS with the join token. The LiveKit SFU
// never sees the key, so it relays media it cannot read.
//
// That is NOT end-to-end encryption, and it must never be called that —
// not in the UI, not in a tooltip, not in docs, not in a config comment,
// not in a commit message. Two reasons, both structural:
//
//   1. The BSFChat server generates and holds the key. It can decrypt
//      everything. "End-to-end" means the endpoints and nobody else;
//      here the server is a third party that holds the secret.
//   2. Anyone who was ever in the channel keeps the ability to decrypt
//      its media until the key is rotated. LiveKit's ratchet cannot fix
//      this: the next key is derived from the current one with a public
//      salt ("LKFrameEncryptionKey") and no secret input, so a holder of
//      any generation can compute all later ones. Verified in
//      libwebrtc's frame_crypto_transformer.h, RatchetKeyMaterial().
//
// This project has already shipped one overstated security claim — a
// voice badge reading "MLS · 256" when the reality was DTLS+SCTP, which
// had to be corrected. The strings below are centralised here, rather
// than written inline wherever a badge is drawn, so that the accurate
// wording has exactly one home and cannot drift back.
//
// Every string here is deliberately plain. If a phrase makes the
// guarantee sound stronger than the two limitations above, it is wrong.

#include <QJsonObject>
#include <QString>

namespace voice {

// How media is protected on the wire for the current session.
enum class MediaProtection {
    // Mesh: peer to peer, no relay in the media path at all. One DTLS
    // 1.2 handshake per peer PAIR keys everything that rides it, but
    // the two media types ride it differently, and the badge names
    // only the audio half:
    //   audio — Opus frames on an SCTP data channel. No SRTP.
    //   video — H.264/AV1 RTP on rtc::Tracks, which IS SRTP, keyed by
    //           that same DTLS handshake (libdatachannel builds it with
    //           its vendored libsrtp — cmake/Dependencies.cmake:38).
    // So "DTLS" is the honest common denominator, and a flat "there is
    // no SRTP here" is true of audio only.
    //
    // Still not end-to-end in the cryptographic sense people mean by
    // E2EE (no identity verification, no forward secrecy across
    // membership changes), so it is not labelled as such.
    MeshDtls,
    // SFU with a server-minted shared key. The relay cannot read media.
    SfuSharedKey,
    // SFU with no encryption configured. The relay CAN read media.
    SfuNone,
    // Encryption was requested and failed. Never silently downgraded —
    // see encryptionFailureIsFatal() below.
    Failed,
};

// Which transport actually carries this session's media.
//
// Deliberately a small local enum rather than IVoiceTransport::Kind:
// this unit stays free of the transport headers (libdatachannel,
// nlohmann, QVideoFrame) so the labelling — the part that has been
// wrong before — can be unit-tested with nothing but Qt Core.
enum class SessionTransport {
    Mesh,
    Sfu,
};

// The protection state a LIVE session provides. This is the single
// mapping from "what did we actually connect with" to "what are we
// allowed to say about it", so a caller cannot pick a label by hand and
// get it wrong the way the QML badge did.
//
// `sfuHasSharedKey` is the server's answer, not a wish: true only when
// parseLiveKitJoin() returned a key. It is ignored for Mesh, which has
// no shared key at all.
//
// Never returns Failed — that state means "encryption was requested and
// could not be set up", which is a decision the caller makes before a
// session exists, not something derivable from a session that started.
MediaProtection protectionForSession(SessionTransport t, bool sfuHasSharedKey);

// Short badge text. Fits next to a connection indicator.
QString protectionBadge(MediaProtection p);

// The long explanation, for a tooltip or a settings row. States the
// limitation in the same breath as the guarantee, always.
QString protectionDetail(MediaProtection p);

// True when this state must interrupt the user rather than be shown
// quietly. Encryption that fails open is worse than no encryption,
// because the user believes they have a property they do not have.
bool encryptionFailureIsFatal(MediaProtection p);

// ---------------------------------------------------------------------
// Parsing the server's token response
// ---------------------------------------------------------------------
// Shape (POST /_matrix/client/v3/rooms/{roomId}/voice/livekit_token):
//
//   { "url", "token", "room", "identity", "ttl",
//     "encryption": { "mode": "shared_key", "key": <base64>,
//                     "key_generation": <int> } }
//
// The `encryption` object is absent when the server has media encryption
// switched off.
struct LiveKitJoinConfig {
    QString url;
    QString token;
    QString room;
    QString identity;
    int ttlSeconds = 0;

    // Raw key bytes, decoded from base64. Empty when the server sent no
    // encryption block.
    QByteArray key;
    qint64 keyGeneration = 0;

    bool hasKey() const { return !key.isEmpty(); }

    // Everything needed to connect is present.
    bool isValid() const {
        return !url.isEmpty() && !token.isEmpty() && !room.isEmpty();
    }
};

struct LiveKitJoinParse {
    LiveKitJoinConfig config;
    // Empty on success. User-facing.
    QString error;
    bool ok() const { return error.isEmpty(); }
};

// Parses and VALIDATES. Rejects rather than repairs:
//
//   - a malformed or wrong-length key is an error, not a reason to
//     connect unencrypted. A client that asked for encryption and
//     quietly got plaintext is the failure mode this whole design is
//     trying to avoid;
//   - an unknown `mode` is an error, because a future server offering a
//     scheme this build does not implement must not be downgraded to one
//     it does.
//
// AES-GCM-256 needs exactly 32 bytes; livekit::EncryptionType::GCM is
// what the transport requests.
LiveKitJoinParse parseLiveKitJoin(const QJsonObject& response);

// Expected key length in bytes for the only mode we accept.
inline constexpr int kSharedKeyBytes = 32;

// The wire value of the only encryption mode this client understands.
inline constexpr char kEncryptionModeSharedKey[] = "shared_key";

} // namespace voice
