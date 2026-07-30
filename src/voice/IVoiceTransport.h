#pragma once

// The seam between the app and whatever moves voice/video bytes.
//
// Two transports must coexist per call, not transitionally but as the
// design (see server/docs/livekit-migration.md §5):
//
//   MESH   — libdatachannel, one rtc::PeerConnection per peer PAIR.
//            Implemented by VoiceEngine. The ONLY option on Android,
//            because livekit/client-sdk-cpp has no mobile support, and
//            the only path that can carry the AV1 mathematically-
//            lossless screen-share tier.
//   SFU    — LiveKit. Desktop only. Not implemented yet.
//
// ---------------------------------------------------------------------
// How this interface was derived
// ---------------------------------------------------------------------
// From what the CALLERS actually invoke, not from what either transport
// happens to offer. Three call sites exist and no others:
//
//   ServerConnection        lifecycle, mute/deafen, per-member state,
//                           receive-side diagnostics
//   ScreenShareController   send-side video for the screen stream
//   CameraController        send-side video for the camera stream
//   AndroidScreenShareController  the JPEG path only
//
// QML and src/model/** never name a transport type — the whole
// QML-facing contract is already mediated by ServerConnection's
// Q_PROPERTY / Q_INVOKABLE surface. Verified: `grep -rn VoiceEngine
// qml/ src/model/` is empty.
//
// ---------------------------------------------------------------------
// What is deliberately NOT here
// ---------------------------------------------------------------------
// Mesh signalling. handleCallInvite / handleCallAnswer /
// handleCallCandidates / handleCallHangup / handleCallNegotiate take raw
// per-peer SDP, per-peer call ids and trickle-ICE candidate batches.
// There is no SFU analogue: LiveKit runs its own signalling over its own
// WebSocket and a client has exactly one connection, not one per peer.
// Those five live on IMeshSignalling below, which a transport offers or
// does not. A caller must ask for it and handle null.
//
// This split is what makes the mixed-mode trap visible in the type
// system rather than at runtime: an SFU participant has no
// IMeshSignalling, so it cannot answer an m.call.invite, and a mesh peer
// waiting for one waits forever. See VoiceTransportSelector.h.

#include "voice/video/VideoCodec.h"

#include <QObject>
#include <QString>
#include <QMap>
#include <QVariantMap>
#include <QJsonArray>
#include <QJsonObject>
#include <QByteArray>
#include <QVideoFrame>

#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------
// IMeshSignalling — mesh-only, obtained via IVoiceTransport::mesh()
// ---------------------------------------------------------------------
// Matrix-timeline call signalling. Every method is a pure sink for an
// inbound event ServerConnection has already parsed and addressed.
class IMeshSignalling {
public:
    virtual ~IMeshSignalling() = default;

    // `caps` is the raw `bsfchat_caps` object from the event content
    // (an empty object for legacy senders — parses to all-off PeerCaps).
    virtual void handleCallInvite(const QString& sender, const QString& callId,
                                 const std::string& sdp, const nlohmann::json& caps) = 0;
    virtual void handleCallAnswer(const QString& sender, const QString& callId,
                                  const std::string& sdp, const nlohmann::json& caps) = 0;
    virtual void handleCallCandidates(
        const QString& sender, const QString& callId,
        const std::vector<std::pair<std::string, std::string>>& candidates) = 0;
    virtual void handleCallHangup(const QString& sender, const QString& callId) = 0;
    // Mid-call renegotiation (bsfchat.call.negotiate). `type` is "offer"
    // or "answer".
    virtual void handleCallNegotiate(const QString& sender, const QString& callId,
                                     const std::string& type, const std::string& sdp) = 0;

    // Mesh reconciliation. The 5 s voice-member poll compares the
    // server's member list against the peers we actually hold and
    // re-offers to anyone missing. Meaningless with an SFU, where the
    // server owns presence.
    virtual bool hasPeer(const QString& userId) const = 0;
    virtual void ensurePeer(const QString& userId) = 0;
};

// ---------------------------------------------------------------------
// IVoiceTransport
// ---------------------------------------------------------------------
// A QObject because every consumer connects to its signals. Qt forbids
// multiple inheritance from QObject, so this is the single QObject base
// and IMeshSignalling is a plain abstract class.
class IVoiceTransport : public QObject {
    Q_OBJECT
public:
    explicit IVoiceTransport(QObject* parent = nullptr) : QObject(parent) {}
    ~IVoiceTransport() override = default;

    // Which transport this is. Must match the `transport` field a client
    // publishes in its m.call.member state so other clients and the
    // server can enforce all-mesh-or-all-SFU per channel.
    enum class Kind { Mesh, LiveKit };
    virtual Kind kind() const = 0;

    // ---- Lifecycle --------------------------------------------------
    //
    // Returns false when the session cannot possibly work and the join
    // must be unwound by the caller. A human-readable reason is emitted
    // through error() FIRST. On false NOTHING has been started: no audio
    // device, no timers, no peers, no connection. ServerConnection
    // depends on that guarantee — it calls teardownVoiceSession() plus
    // MatrixClient::leaveVoice() on a false return.
    //
    // Mesh returns false for relay-only policy with no TURN server: ICE
    // would gather zero candidates and every peer would fail silently.
    //
    // `serverConfig` is whatever the server told us about how to
    // connect, and is the one argument whose CONTENT is transport-
    // specific while its ROLE is not:
    //   mesh    — the GET /voip/turnServer body: allow_p2p, uris,
    //             username, password
    //   livekit — the POST .../voice/livekit_token body: url, token
    // Keeping it a QJsonObject rather than a variant type means adding
    // the SFU transport does not change this signature, and neither
    // transport can accidentally read the other's fields.
    //
    // `members` is the current voice roster from GET .../voice/members.
    // Mesh uses it to decide who to offer to; an SFU transport ignores
    // it (LiveKit reports participants itself) but it is still the
    // roster a transport needs to detect a mixed-mode channel.
    [[nodiscard]] virtual bool start(const QString& roomId, const QJsonArray& members,
                                     const QJsonObject& serverConfig) = 0;
    virtual void stop() = 0;

    // Local user id. Mesh needs it before start() for the glare
    // tie-break; an SFU transport uses it to derive its participant
    // identity. Required by both, so it belongs on the interface.
    virtual void setLocalUserId(const QString& userId) = 0;

    // ---- Audio ------------------------------------------------------
    virtual void setMuted(bool muted) = 0;
    virtual void setDeafened(bool deafened) = 0;

    // ---- Per-member state, for the UI -------------------------------
    //
    // Connection state keyed by user id, as one of the strings
    // "new" / "connecting" / "connected" / "failed" / "disconnected".
    // Consumed verbatim by QML via ServerConnection::voiceMembers()'s
    // "peerState" field, so the vocabulary is FIXED — a transport may
    // not invent new values without a QML change, and QML belongs to
    // someone else.
    //
    // Mesh returns one entry per peer connection. An SFU has a single
    // connection to the server, so it must synthesise the map from
    // participant presence: every subscribed participant is
    // "connected", and everyone in the roster who has not appeared yet
    // is "connecting".
    virtual QMap<QString, QString> peerStates() const = 0;

    // Cumulative receive-side counters for the diagnostics overlay:
    // rxFrames / rxBytes / decoded / dropped / width / height / codec.
    // Empty map when nothing is being received for (userId, streamId).
    // Key names are read by QML — do not rename them.
    virtual QVariantMap videoReceiveStats(const QString& userId, int streamId) const = 0;

    // True when media is actually reaching someone. Mesh: at least one
    // peer's data channel is open. SFU: connected with ≥1 other
    // participant subscribed.
    virtual bool hasOpenPeers() const = 0;

    // ---- Video send -------------------------------------------------
    //
    // Called by ScreenShareController / CameraController. The capture
    // and encode front end is transport-agnostic and survives intact;
    // only the sink differs.
    virtual void prepareVideoSend() = 0;
    virtual void broadcastEncodedVideo(VideoStreamId stream, const EncodedFrame& frame) = 0;

    // Legacy JPEG fan-out, for peers with no video_rtp capability.
    // NOT removable: AndroidScreenShareController has ONLY this path,
    // and Android stays on mesh, so deleting it deletes Android screen
    // share. An SFU transport can no-op it — LiveKit clients would not
    // understand the tag protocol and there is no per-peer data channel
    // to fan out over.
    virtual void broadcastScreenFrame(const QByteArray& jpegData) = 0;
    virtual void broadcastCameraFrame(const QByteArray& jpegData) = 0;

    // ---- Send-side capability queries -------------------------------
    //
    // These read as questions about the transport but are really
    // questions about the RECEIVING FLEET, which is why they are the
    // awkward part of this interface and worth naming as such.
    //
    // Over a mesh the sender knows every receiver's advertised caps
    // (from `bsfchat_caps` on invite/answer) and can intersect them.
    // Behind an SFU it cannot: the server chooses what each subscriber
    // gets, per subscriber. An SFU implementation therefore answers
    // conservatively rather than accurately, and the honest consequence
    // is that the lossless tier is mesh-only (decision 4) rather than
    // "degraded on SFU".
    virtual bool hasVideoCapablePeers() const = 0;
    virtual bool hasLegacyOpenPeers(VideoStreamId stream) const = 0;
    virtual H264Profile negotiatedH264Profile() const = 0;

    // ---- AV1 mathematically-lossless tier (mesh-only) ---------------
    //
    // Kept alive per decision 4. It rides the reliable "video-lossless"
    // DATA CHANNEL, not a media track, and there is no raw-frame-ingest
    // equivalent: the SDK picks the encoder and its settings, and
    // exposes no "encode this losslessly" control. An SFU
    // implementation must return false from allPeersSupportLossless()
    // so the sender never enters lossless mode, and no-op
    // broadcastLosslessVideo().
    virtual bool allPeersSupportLossless() const = 0;
    virtual void broadcastLosslessVideo(VideoStreamId stream, const EncodedFrame& frame) = 0;
    // Admission control: true when a peer's lossless channel has more
    // than `budgetBytes` queued, so the sender should drop the next
    // capture frame rather than queue latency.
    virtual bool losslessBackpressure(qint64 budgetBytes) const = 0;

    // ---- Mesh signalling escape hatch -------------------------------
    //
    // Returns nullptr for transports that do not do Matrix-timeline call
    // signalling. Callers MUST check. This is the type-level statement
    // of the mixed-mode problem: an SFU participant has no way to
    // answer an m.call.invite.
    virtual IMeshSignalling* mesh() { return nullptr; }
    const IMeshSignalling* mesh() const {
        return const_cast<IVoiceTransport*>(this)->mesh();
    }

signals:
    // ---- Membership / state ----
    void peerConnected(const QString& userId);
    void peerDisconnected(const QString& userId);
    void peerStateChanged(const QString& userId, const QString& state);
    void error(const QString& message);

    // ---- Audio levels (drive the QML speaking indicators) ----
    void micLevelChanged(float level);
    void peerLevelChanged(const QString& userId, float level);

    // ---- Video receive ----
    // Decoded frame from a remote participant. ServerConnection routes
    // these into VideoStreamRegistry, which both transports share.
    void peerVideoFrameDecoded(const QString& userId, int streamId,
                               const QVideoFrame& frame);
    // One JPEG-encoded frame from a legacy peer.
    void peerScreenFrameReceived(const QString& userId, const QByteArray& jpegData);
    void peerCameraFrameReceived(const QString& userId, const QByteArray& jpegData);

    // ---- Video send feedback ----
    // Someone needs a keyframe on our outgoing `streamId`.
    void videoKeyframeRequested(int streamId);
    // Delivered/sent byte ratio for our outgoing `streamId` toward
    // `userId`. Mesh derives this from hand-rolled app-level receiver
    // reports; it feeds VideoRateController's AIMD loop. LiveKit has
    // real RTCP and server-side bandwidth estimation, so an SFU
    // transport should simply never emit this and let
    // VideoRateController sit idle rather than feed it worse data.
    void videoDeliveryRatio(const QString& userId, int streamId, double ratio);
    // A peer's lossless channel is rejecting sends — the share
    // controller falls back to H.264 for the rest of the share.
    void losslessSendUnavailable();
};
