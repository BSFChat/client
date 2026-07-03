#pragma once

#include "voice/video/VideoCodec.h"

#include <QObject>
#include <QMap>
#include <QTimer>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <QVideoFrame>

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class MatrixClient;
class AudioEngine;
class PeerConnectionManager;
class VideoReceivePipeline;

class VoiceEngine : public QObject {
    Q_OBJECT
public:
    explicit VoiceEngine(MatrixClient* client, QObject* parent = nullptr);
    ~VoiceEngine();

    void start(const QString& roomId, const QJsonArray& members, const QJsonObject& turnConfig);
    void stop();
    bool isRunning() const { return m_running; }

    // Local user id — required for the glare tie-break (deciding
    // deterministically whose offer survives when both sides invite
    // each other at once). Set before start().
    void setLocalUserId(const QString& userId) { m_localUserId = userId; }

    // Mesh-reconciliation accessors: the voice member poll compares
    // the server's member list against the peers we actually hold and
    // re-offers to anyone missing.
    QStringList connectedPeerIds() const { return m_peers.keys(); }
    bool hasPeer(const QString& userId) const { return m_peers.contains(userId); }
    // Offer to `userId` if we don't already hold a peer for them.
    void ensurePeer(const QString& userId);
    // True when at least one peer's data channel is open — i.e.
    // broadcast frames are actually reaching someone.
    bool hasOpenPeers() const;

    // `caps` is the raw `bsfchat_caps` object from the event content
    // (empty object for legacy clients — parses to all-off PeerCaps).
    void handleCallInvite(const QString& sender, const QString& callId,
                          const std::string& sdp, const nlohmann::json& caps);
    void handleCallAnswer(const QString& sender, const QString& callId,
                          const std::string& sdp, const nlohmann::json& caps);
    void handleCallCandidates(const QString& sender, const QString& callId,
                               const std::vector<std::pair<std::string, std::string>>& candidates);
    void handleCallHangup(const QString& sender, const QString& callId);
    // Mid-call renegotiation (bsfchat.call.negotiate). `type` is
    // "offer" or "answer". Glare resolution: the lexicographically
    // LESSER user id is impolite (its re-offer wins) — same tie-break
    // as the invite glare rule above.
    void handleCallNegotiate(const QString& sender, const QString& callId,
                             const std::string& type, const std::string& sdp);

    void setMuted(bool muted);
    void setDeafened(bool deafened);

    float micLevel() const { return m_micLevel; }

    // Per-peer connection state keyed by user-id. "connected", "connecting",
    // "failed", "new", "disconnected". VoicePanel reads this to show colored
    // indicators per member.
    QMap<QString, QString> peerStates() const;

    // Fan out a JPEG-encoded screen frame to every connected LEGACY
    // peer (no video_rtp capability). RTP-capable peers get real video
    // via broadcastEncodedVideo instead — sending them JPEG too would
    // double the bandwidth for nothing.
    void broadcastScreenFrame(const QByteArray& jpegData);
    // Same fan-out as screen share but uses the 0x03 (camera) tag.
    void broadcastCameraFrame(const QByteArray& jpegData);

    // ---- RTP video ----
    // Local user started/stopped producing video. Adds tracks (with
    // renegotiation) toward every capable peer; tracks persist after
    // stop so a share restart costs no renegotiation.
    void prepareVideoSend();
    // Fan an encoded access unit out to every peer with an open track.
    void broadcastEncodedVideo(VideoStreamId stream, const EncodedFrame& frame);
    // Fan an AV1 lossless temporal unit out over the reliable
    // "video-lossless" channels.
    void broadcastLosslessVideo(VideoStreamId stream, const EncodedFrame& frame);
    // True when every current video-capable peer advertises the
    // av1-dc lossless capability — the sender's gate for enabling
    // lossless mode (mixed fleets fall back to H.264 for everyone).
    bool allPeersSupportLossless() const;
    // Admission control: true when any peer's lossless channel has
    // more than `budgetBytes` queued — the sender should drop the
    // next capture frame instead of queueing latency.
    bool losslessBackpressure(qint64 budgetBytes) const;
    // True if any connected peer still needs the legacy JPEG path for
    // `stream` (no open video track for it).
    bool hasLegacyOpenPeers(VideoStreamId stream) const;
    // True if any peer advertises video_rtp (drives encoder startup).
    bool hasVideoCapablePeers() const;
    // Best H.264 profile every current video peer can decode AND we
    // can encode: High when unanimous (platform encoders), else
    // Constrained Baseline. Re-evaluated per share tick so a
    // mid-call CB-only joiner downgrades the stream (the pipeline
    // rebuilds the session and IDRs on the profile flip).
    H264Profile negotiatedH264Profile() const;

signals:
    void peerConnected(const QString& userId);
    void peerDisconnected(const QString& userId);
    void peerStateChanged(const QString& userId, const QString& state);
    void error(const QString& message);
    void micLevelChanged(float level);
    void peerLevelChanged(const QString& userId, float level);
    // One JPEG-encoded screen frame received from a remote peer.
    // Subscribed to by ScreenShareController to fan frames into the
    // per-peer preview surface in VoiceRoom.
    void peerScreenFrameReceived(const QString& userId, const QByteArray& jpegData);
    void peerCameraFrameReceived(const QString& userId, const QByteArray& jpegData);
    // Decoded RTP video frame from a remote peer — ServerConnection
    // routes these into the VideoStreamRegistry.
    void peerVideoFrameDecoded(const QString& userId, int streamId,
                               const QVideoFrame& frame);
    // Some peer needs a keyframe on our outgoing `streamId` (RTCP PLI,
    // app-level "kf" request, or a track that just opened) — the send
    // pipeline reacts by forcing an IDR.
    void videoKeyframeRequested(int streamId);
    // Delivered/sent byte ratio for our outgoing `streamId` toward
    // `userId`, derived from its latest receiver report. Feeds the
    // rate controller ("worst recent peer governs").
    void videoDeliveryRatio(const QString& userId, int streamId, double ratio);

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
    void onLocalDescription(const QString& peerId, const std::string& type, const std::string& sdp);
    void onLocalCandidate(const QString& peerId, const std::string& candidate, const std::string& mid);
    void flushCandidateBatch();
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
    QJsonObject m_turnConfig;
    bool m_running = false;
    bool m_allowP2P = false;

    // ICE candidate batching
    QTimer m_candidateBatchTimer;
    QMap<QString, std::vector<std::pair<std::string, std::string>>> m_pendingCandidates;

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
