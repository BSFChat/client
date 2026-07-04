#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>

#include "voice/PeerCaps.h"
#include "voice/video/VideoCodec.h"

#include <QTimer>

#include <rtc/rtc.hpp>
#include <atomic>
#include <memory>
#include <vector>
#include <string>

class PeerConnectionManager : public QObject {
    Q_OBJECT
public:
    PeerConnectionManager(const QString& peerId, const QString& callId,
                          const rtc::Configuration& config, QObject* parent = nullptr);
    ~PeerConnectionManager();

    QString peerId() const { return m_peerId; }
    QString callId() const { return m_callId; }

    // Lifecycle
    void createOffer();
    void applyOffer(const std::string& sdp);
    void applyAnswer(const std::string& sdp);
    void addRemoteCandidate(const std::string& candidate, const std::string& mid);

    // ---- Mid-call renegotiation (bsfchat.call.negotiate) ----
    // The initial offer/answer pair is exchanged exactly once via
    // m.call.invite/answer; every later SDP (adding video m-lines)
    // flows through these. VoiceEngine routes local descriptions by
    // initialNegotiationDone(): false → invite/answer, true → negotiate.
    bool initialNegotiationDone() const { return m_initialNegotiationDone; }
    // True while a locally-initiated re-offer is in flight (sent,
    // no answer yet). VoiceEngine's glare logic reads this.
    bool hasPendingLocalReoffer() const { return m_localReofferPending; }
    // Kick off (or queue) a renegotiation. Safe to call in any
    // signaling state: if we're mid-exchange the re-offer fires as
    // soon as the connection returns to stable.
    void triggerRenegotiation();
    // Incoming negotiate-offer: set remote, produce answer.
    void applyNegotiateOffer(const std::string& sdp);
    // Incoming negotiate-answer to our re-offer.
    void applyNegotiateAnswer(const std::string& sdp);
    // Glare loser path (polite peer): discard our in-flight re-offer,
    // then re-trigger once stable so local changes aren't lost.
    void rollbackLocalReoffer();

    // ---- Peer media capabilities ----
    void setRemoteCaps(const PeerCaps& caps) { m_remoteCaps = caps; m_remoteCapsKnown = true; }
    const PeerCaps& remoteCaps() const { return m_remoteCaps; }
    bool remoteCapsKnown() const { return m_remoteCapsKnown; }
    bool remoteSupportsVideoRtp() const { return m_remoteCapsKnown && m_remoteCaps.videoRtp; }

    // JSON control message over the data channel, tag 0x04
    // ({"t":"caps"|"kf"|"rr", ...}). MUST only be called once the
    // peer's caps prove it's a new client — old clients misparse
    // unknown tags as audio (see onMessage's legacy fallback).
    void sendControl(const QByteArray& json);

    // ---- RTP video tracks ----
    // Add the vscreen/vcamera SendRecv tracks and renegotiate.
    // Idempotent; only call for peers whose caps advertise video_rtp.
    // The answerer side never calls this — it adopts the tracks
    // delivered via onTrack and sends on them in the reverse
    // direction (SendRecv m-lines, so no counter-renegotiation).
    void ensureVideoTracks();
    bool hasVideoTrackOpen(VideoStreamId stream) const;
    // Packetize + send one encoded access unit. No-op while the track
    // isn't open. RTP timestamps derive from EncodedFrame::captureTimeUs.
    void sendVideoFrame(VideoStreamId stream, const EncodedFrame& frame);
    // Ask the remote sender for an IDR on `stream` (0x04 "kf" control
    // message — the guaranteed path; RTCP PLI in v0.24.5 can't be
    // triggered app-side on the receive direction of our chain).
    void requestPeerKeyframe(VideoStreamId stream);
    // Cumulative encoded-video send counters toward this peer —
    // compared against the peer's receiver reports to derive loss.
    quint64 videoTxFrames(VideoStreamId stream) const { return m_txFrames[int(stream)]; }
    quint64 videoTxBytes(VideoStreamId stream) const { return m_txBytes[int(stream)]; }

    // ---- Lossless tier (AV1 over a dedicated reliable channel) ----
    // v0.24.5 has no AV1 RTP depacketizer, and lossless wants reliable
    // delivery anyway (a lossy lossless stream is pointless), so these
    // frames ride a separate ordered data channel "video-lossless",
    // opened in-band (no renegotiation). Framing per message:
    //   [u8 streamId][u8 flags][u32be seq][u32be tsMs][AV1 TU]
    // flags bit0 = keyframe (AV1 keyframes aren't cheaply detectable
    // from the bitstream; the receive pipeline needs the hint for
    // post-error resync).
    void sendLosslessFrame(VideoStreamId stream, const EncodedFrame& frame);
    // Admission-control input: bytes queued but unsent on the channel.
    // The sender drops capture frames (never delays them) while this
    // exceeds its budget.
    qint64 losslessBufferedAmount() const;
    void sendAudioFrame(const QByteArray& frame);
    // Send a JPEG-encoded screen-share frame to this peer over the
    // same SCTP data channel. Wire format: [tag][payload] where
    // tag=0x01 for audio, tag=0x02 for screen JPEG. All clients that
    // support screen share must speak this framing; audio-only
    // peers see the 0x02 frames as garbage and drop them. We keep
    // tag=0x01 on audio too so the wire format is symmetric.
    void sendScreenFrame(const QByteArray& jpegData);
    // Camera JPEG frame — wire format [0x03][jpeg_payload].
    void sendCameraFrame(const QByteArray& jpegData);

    // Connection quality
    enum class PeerState { New, Connecting, Connected, Disconnected, Failed };
    Q_ENUM(PeerState)
    PeerState peerState() const { return m_peerState; }

    // True when this side created the offer (i.e. we initiated the
    // connection). VoiceEngine's glare tie-break needs to know which
    // side of a simultaneous-offer collision we're on.
    bool isOfferer() const { return m_isOfferer; }
    // True once the SCTP data channel is open — the only state in
    // which frames actually reach the remote peer.
    bool isChannelOpen() const { return m_dc && m_dc->isOpen(); }

    // Frame counters — useful for diagnostics.
    int framesSent() const { return m_framesSent; }
    int framesReceived() const { return m_framesReceived; }

signals:
    void localDescriptionReady(const std::string& type, const std::string& sdp);
    void localCandidateReady(const std::string& candidate, const std::string& mid);
    void connected();
    void disconnected();
    void peerStateChanged(PeerState state);
    void audioFrameReceived(const QByteArray& frame);
    void screenFrameReceived(const QByteArray& jpegData);
    void cameraFrameReceived(const QByteArray& jpegData);
    // 0x04 control payload (JSON, tag stripped).
    void controlMessageReceived(const QByteArray& json);
    // AV1 temporal unit from the peer's "video-lossless" channel
    // (framing already stripped).
    void losslessFrameReceived(int streamId, const QByteArray& temporalUnit,
                               bool keyframe);
    // Reassembled H.264 access unit from the remote's video track.
    // `lossSuspected` is set when RTP sequence gaps were observed since
    // the previous AU — the unit is likely incomplete and decoding it
    // would display error-concealment garbage; the consumer should drop
    // it and wait for the next keyframe.
    void videoFrameReceived(int streamId, const QByteArray& accessUnit,
                            bool lossSuspected);
    // The send direction of a video track became usable.
    void videoTrackOpen(int streamId);
    // Remote sent RTCP PLI — it needs a keyframe on our send stream.
    void keyframeRequestedByPeer(int streamId);

private:
    void setupCallbacks();
    void setupDataChannel(std::shared_ptr<rtc::DataChannel> dc);
    void flushPendingCandidates();
    // Fires a queued renegotiation once the signaling state is stable.
    void maybeRenegotiateAgain();
    // Build the RTP handler chain (packetizer → SR → NACK → PLI →
    // depacketizer → receiving session) on a track, either one we
    // added (offerer) or one delivered by onTrack (answerer).
    void attachVideoTrack(VideoStreamId stream, std::shared_ptr<rtc::Track> track);
    void setupLosslessChannel(std::shared_ptr<rtc::DataChannel> dc);
    // Size-checked, exception-safe DataChannel send. Returns false if
    // the frame was dropped (oversized or transport error). Callers
    // must have verified m_dc is present and open.
    bool sendOnDataChannel(rtc::binary&& data, const char* what);

    QString m_peerId;
    QString m_callId;
    std::shared_ptr<rtc::PeerConnection> m_pc;
    std::shared_ptr<rtc::DataChannel> m_dc;
    std::vector<std::pair<std::string, std::string>> m_pendingCandidates;
    bool m_remoteDescriptionSet = false;
    bool m_isOfferer = false;
    PeerState m_peerState = PeerState::New;
    int m_framesSent = 0;
    int m_framesReceived = 0;
    int m_screenFramesDropped = 0;  // bumped when bufferedAmount() exceeded
    qint64 m_lastOversizeWarnMs = 0; // rate-limits oversized-frame warnings

    // Renegotiation state. m_initialNegotiationDone flips after the
    // first offer/answer round-trip completes (offerer: answer
    // applied; answerer: local answer emitted) and gates the
    // invite/answer-vs-negotiate signaling split.
    PeerCaps m_remoteCaps;
    bool m_remoteCapsKnown = false;
    bool m_initialNegotiationDone = false;
    bool m_localReofferPending = false;
    // A renegotiation was requested (or rolled back) while another
    // exchange was in flight — re-fire when we return to stable.
    bool m_renegotiateAgain = false;

    // Per-stream RTP track context. rtpConfig is shared between the
    // packetizer and the SR reporter; timestamp advances manually per
    // send (v0.24.5 has no sendFrame()).
    struct VideoTrackCtx {
        std::shared_ptr<rtc::Track> track;
        std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig;
        std::shared_ptr<rtc::RtcpSrReporter> srReporter;
        qint64 startTimeUs = -1;
        bool open = false;
        // Set by the RtpGapDetector (libdatachannel network thread),
        // consumed by onFrame on the same thread; atomic as cheap
        // insurance against future callers.
        std::atomic<bool> lossPending{false};
    };
    VideoTrackCtx m_video[kVideoStreamCount];
    QTimer* m_srTimer = nullptr;   // 1 s sender-report tick, lazily created
    quint64 m_txFrames[kVideoStreamCount] = {};
    quint64 m_txBytes[kVideoStreamCount] = {};

    // Lossless channel (created lazily by the sending side; adopted
    // via onDataChannel label match on the receiving side).
    std::shared_ptr<rtc::DataChannel> m_losslessDc;
    quint32 m_losslessSeq[kVideoStreamCount] = {};
};
