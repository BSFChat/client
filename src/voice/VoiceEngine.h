#pragma once

// The mesh implementation of IVoiceTransport: one rtc::PeerConnection
// per peer PAIR, signalled over the Matrix timeline
// (m.call.invite/answer/candidates/hangup + bsfchat.call.negotiate).
//
// This is the ONLY transport on Android — livekit/client-sdk-cpp has no
// mobile support — and the only one that can carry the AV1
// mathematically-lossless screen-share tier, which rides a reliable data
// channel rather than a media track. Both reasons are permanent, so this
// class is not transitional and is not scheduled for deletion.
//
// It owns the components the SFU path would retire: AudioWorker,
// AudioMixer, AudioPacketQueue, JitterBuffer (via AudioEngine), the
// video codec backends, VideoSendPipeline/VideoReceivePipeline and
// VideoRateController. Those must keep working.

#include "voice/IVoiceTransport.h"
#include "voice/video/VideoCodec.h"

#include <QObject>
#include <QMap>
#include <QTimer>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <QVariantMap>
#include <QVideoFrame>

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class MatrixClient;
class AudioEngine;
class PeerConnectionManager;
class VideoReceivePipeline;

class VoiceEngine : public IVoiceTransport, public IMeshSignalling {
    Q_OBJECT
public:
    explicit VoiceEngine(MatrixClient* client, QObject* parent = nullptr);
    ~VoiceEngine() override;

    Kind kind() const override { return Kind::Mesh; }
    IMeshSignalling* mesh() override { return this; }

    // Returns false when the session cannot possibly work and the join
    // must be unwound by the caller (currently: relay-only policy with
    // no TURN server configured — ICE would gather zero candidates and
    // every peer would fail silently). A human-readable reason is
    // emitted through error() first. On false NOTHING is started: no
    // audio device, no timers, no peers.
    //
    // `turnConfig` is IVoiceTransport::start()'s `serverConfig`: for
    // mesh it is the GET /voip/turnServer body (allow_p2p, uris,
    // username, password).
    [[nodiscard]] bool start(const QString& roomId, const QJsonArray& members,
                             const QJsonObject& turnConfig) override;
    void stop() override;
    bool isRunning() const { return m_running; }

    // Local user id — required for the glare tie-break (deciding
    // deterministically whose offer survives when both sides invite
    // each other at once). Set before start().
    void setLocalUserId(const QString& userId) override { m_localUserId = userId; }

    // Mesh-reconciliation accessors: the voice member poll compares
    // the server's member list against the peers we actually hold and
    // re-offers to anyone missing.
    QStringList connectedPeerIds() const { return m_peers.keys(); }
    bool hasPeer(const QString& userId) const override { return m_peers.contains(userId); }
    // Offer to `userId` if we don't already hold a peer for them.
    void ensurePeer(const QString& userId) override;
    // True when at least one peer's data channel is open — i.e.
    // broadcast frames are actually reaching someone.
    bool hasOpenPeers() const override;

    // `caps` is the raw `bsfchat_caps` object from the event content
    // (empty object for legacy clients — parses to all-off PeerCaps).
    void handleCallInvite(const QString& sender, const QString& callId,
                          const std::string& sdp, const nlohmann::json& caps) override;
    void handleCallAnswer(const QString& sender, const QString& callId,
                          const std::string& sdp, const nlohmann::json& caps) override;
    void handleCallCandidates(const QString& sender, const QString& callId,
                               const std::vector<std::pair<std::string, std::string>>& candidates) override;
    void handleCallHangup(const QString& sender, const QString& callId) override;
    // Mid-call renegotiation (bsfchat.call.negotiate). `type` is
    // "offer" or "answer". Glare resolution: the lexicographically
    // LESSER user id is impolite (its re-offer wins) — same tie-break
    // as the invite glare rule above.
    void handleCallNegotiate(const QString& sender, const QString& callId,
                             const std::string& type, const std::string& sdp) override;

    void setMuted(bool muted) override;
    void setDeafened(bool deafened) override;

    float micLevel() const { return m_micLevel; }

    // Per-peer connection state keyed by user-id. "connected", "connecting",
    // "failed", "new", "disconnected". VoicePanel reads this to show colored
    // indicators per member.
    QMap<QString, QString> peerStates() const override;

    // Cumulative receive-side stats for the diagnostics overlay:
    // rxFrames/rxBytes/decoded/dropped/width/height/codec. Empty map
    // when no receive pipeline exists for (userId, streamId).
    QVariantMap videoReceiveStats(const QString& userId, int streamId) const override;

    // Fan out a JPEG-encoded screen frame to every connected LEGACY
    // peer (no video_rtp capability). RTP-capable peers get real video
    // via broadcastEncodedVideo instead — sending them JPEG too would
    // double the bandwidth for nothing.
    void broadcastScreenFrame(const QByteArray& jpegData) override;
    // Same fan-out as screen share but uses the 0x03 (camera) tag.
    void broadcastCameraFrame(const QByteArray& jpegData) override;

    // ---- RTP video ----
    // Local user started/stopped producing video. Adds tracks (with
    // renegotiation) toward every capable peer; tracks persist after
    // stop so a share restart costs no renegotiation.
    void prepareVideoSend() override;
    // Fan an encoded access unit out to every peer with an open track.
    void broadcastEncodedVideo(VideoStreamId stream, const EncodedFrame& frame) override;
    // Fan an AV1 lossless temporal unit out over the reliable
    // "video-lossless" channels.
    void broadcastLosslessVideo(VideoStreamId stream, const EncodedFrame& frame) override;
    // True when every current video-capable peer advertises the
    // av1-dc lossless capability — the sender's gate for enabling
    // lossless mode (mixed fleets fall back to H.264 for everyone).
    bool allPeersSupportLossless() const override;
    // Admission control: true when any peer's lossless channel has
    // more than `budgetBytes` queued — the sender should drop the
    // next capture frame instead of queueing latency.
    bool losslessBackpressure(qint64 budgetBytes) const override;
    // True if any connected peer still needs the legacy JPEG path for
    // `stream` (no open video track for it).
    bool hasLegacyOpenPeers(VideoStreamId stream) const override;
    // True if any peer advertises video_rtp (drives encoder startup).
    bool hasVideoCapablePeers() const override;
    // Best H.264 profile every current video peer can decode AND we
    // can encode: High when unanimous (platform encoders), else
    // Constrained Baseline. Re-evaluated per share tick so a
    // mid-call CB-only joiner downgrades the stream (the pipeline
    // rebuilds the session and IDRs on the profile flip).
    H264Profile negotiatedH264Profile() const override;

    // NOTE: every signal this class emits is now declared on
    // IVoiceTransport. Re-declaring them here would shadow the base
    // versions and silently break any connect() made through an
    // IVoiceTransport*, so do not add a `signals:` block back.

private:
    void addPeer(const QString& userId, bool isOfferer);
    void removePeer(const QString& userId);
    // Common signal wiring shared by addPeer and handleCallInvite.
    void wirePeer(PeerConnectionManager* peer, const QString& userId);
    // Dead-peer cleanup: Disconnected gets a grace period (transient
    // ICE blips recover on their own); Failed/Closed is torn down
    // immediately.
    void startDisconnectGrace(const QString& userId);
    void cancelDisconnectGrace(const QString& userId);
    // Setup watchdog: a peer that never reaches Connected (lost answer,
    // wedged DTLS, offer to a user with no live client) would otherwise
    // sit in New/Connecting forever — nothing else times that out.
    void startConnectWatchdog(const QString& userId);
    void cancelConnectWatchdog(const QString& userId);
    void onLocalDescription(const QString& peerId, const std::string& type, const std::string& sdp);
    void onLocalCandidate(const QString& peerId, const std::string& candidate, const std::string& mid);
    void flushCandidateBatch();
    // ---- Inbound-candidate buffering (engine level) ----
    // PeerConnectionManager also buffers candidates that arrive before
    // its remote description is set, but that buffer dies with the
    // object — and every replacement path (glare loss, dead-peer
    // replace, new-callId replace, watchdog teardown) destroys it. Worse,
    // candidates for a call id we don't hold yet used to be dropped on
    // the floor, and they are sent exactly once (flushCandidateBatch
    // clears its queue). Hold them here, keyed by the (sender, callId)
    // they belong to, and replay them when a matching peer appears.
    void bufferInboundCandidates(
        const QString& sender, const QString& callId,
        const std::vector<std::pair<std::string, std::string>>& candidates);
    void replayInboundCandidates(const QString& sender, const QString& callId);
    // Drop the buffer for one specific call (hangup / peer teardown).
    // Scoped by call id on purpose: a peer being REPLACED by a newer
    // call id must not lose the candidates already buffered for that
    // newer call.
    void dropInboundCandidates(const QString& sender, const QString& callId);
    void pruneInboundCandidates();
    void sendCallEvent(const QString& eventType, const nlohmann::json& content);
    rtc::Configuration buildRtcConfig() const;
    QString generateCallId() const;
    // This client's media capabilities, advertised in every
    // invite/answer we send. Codec/profile lists come from the video
    // backends' capability probes (an empty intersection between two
    // peers safely means "no video").
    static nlohmann::json localCapsJson();
    // Dispatch a 0x04 control message ({"t": ...}) from `userId`.
    void onControlMessage(const QString& userId, const QByteArray& json);
    // Add video tracks toward `userId` when we're actively sending
    // and its caps allow — called wherever caps become known.
    void maybeSetupVideoFor(const QString& userId);
    // Lazily create the per-peer decode pipeline for a stream. A
    // codec switch (H.264 ↔ AV1 lossless mid-call) tears the old
    // pipeline down and builds a fresh one.
    VideoReceivePipeline* recvPipeline(const QString& userId, int streamId,
                                       VideoCodecKind codec);
    void dropRecvPipelines(const QString& userId);

    MatrixClient* m_client;
    QString m_roomId;
    QString m_localUserId;
    AudioEngine* m_audioEngine = nullptr;
    QMap<QString, PeerConnectionManager*> m_peers;
    QMap<QString, QString> m_callIds;
    // Per-peer single-shot grace timers for the Disconnected state.
    QMap<QString, QTimer*> m_disconnectTimers;
    // Per-peer single-shot watchdogs for the initial setup phase.
    QMap<QString, QTimer*> m_connectWatchdogs;
    QJsonObject m_turnConfig;
    bool m_running = false;
    bool m_allowP2P = false;

    // ICE candidate batching (OUTBOUND — ours, awaiting the next flush)
    QTimer m_candidateBatchTimer;
    QMap<QString, std::vector<std::pair<std::string, std::string>>> m_pendingCandidates;

    // INBOUND candidates parked until a peer for their (sender, callId)
    // exists. Bounded and TTL'd so a peer that never materialises (or a
    // hostile sender) can't grow this unbounded.
    struct InboundCandidateBucket {
        qint64 firstSeenMs = 0;
        std::vector<std::pair<std::string, std::string>> items;
    };
    QMap<QPair<QString, QString>, InboundCandidateBucket> m_inboundCandidates;
    static constexpr qint64 kInboundCandidateTtlMs = 30000;
    static constexpr int kMaxInboundCandidatesPerCall = 128;
    static constexpr int kMaxInboundCandidateCalls = 32;

    float m_micLevel = 0.0f;

    // RTP video state. m_videoSendActive latches while the local user
    // produces video (screen or camera) so peers that join mid-share
    // get tracks as soon as their caps arrive.
    bool m_videoSendActive = false;
    QMap<QPair<QString, int>, VideoReceivePipeline*> m_recvPipelines;

    // Receiver reports: every 500 ms each receive pipeline's cumulative
    // counters go to its sender ({"t":"rr"}); on the send side, the
    // last-seen (rx, tx) snapshots per peer×stream turn the next report
    // into a windowed delivery ratio.
    QTimer m_rrTimer;
    struct RrSnapshot { quint64 rxBytes = 0; quint64 txBytes = 0; };
    QMap<QPair<QString, int>, RrSnapshot> m_rrSnapshots;
    void sendReceiverReports();
};
