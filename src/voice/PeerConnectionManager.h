#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>

#include "voice/PeerCaps.h"
#include "voice/video/VideoCodec.h"

#include <QTimer>

#include <rtc/rtc.hpp>
#include <atomic>
#include <type_traits>
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
    // S-1: can this peer actually DECODE the H.264 we would send it?
    // remoteSupportsVideoRtp() only says it understands the protocol —
    // a build with no H.264 decoder (Android without openh264) answers
    // true there and still cannot show a single frame.
    bool remoteCanReceiveRtpVideo() const {
        return peerCanReceiveRtpVideo(m_remoteCaps, m_remoteCapsKnown,
                                      videoCodecIdH264());
    }
    // S-1: must this peer be served the legacy JPEG stills for `stream`
    // (no decoder, or decoder but the track hasn't opened yet)?
    bool needsLegacyJpeg(VideoStreamId stream) const {
        return peerNeedsLegacyJpeg(m_remoteCaps, m_remoteCapsKnown,
                                   hasVideoTrackOpen(stream),
                                   videoCodecIdH264());
    }

    // JSON control message, tag 0x04
    // ({"t":"caps"|"kf"|"rr"|"stream", ...}). MUST only be called once
    // the peer's caps prove it's a new client — old clients misparse
    // unknown tags as audio (see onMessage's legacy fallback).
    //
    // Prefers the RELIABLE, ORDERED "control" channel and falls back to
    // the audio channel when the peer never opened one (S-2). The audio
    // channel is unordered with maxRetransmits=0, so a keyframe request
    // or an end-of-stream notice sent on it is simply gone if its one
    // datagram is lost — which is how a viewer stayed frozen until the
    // next periodic IDR, 10-30 s later.
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
    // Raise/lower the RTP pacer's ceiling for `stream` so it always
    // sits above what the encoder may emit (S-15). Thread-safe.
    void setVideoPacingCeilingKbps(VideoStreamId stream, int maxKbps);
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
    // The lossless channel rejected sends (throttled to one emission
    // per few seconds) — the share should fall back to H.264 rather
    // than keep pushing frames that never arrive.
    void losslessSendStalled();
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
    // Reliable+ordered channel carrying only 0x04 control JSON.
    void setupControlChannel(std::shared_ptr<rtc::DataChannel> dc);
    // Opens that channel if — and only if — the peer's caps say it
    // understands the label. Idempotent; safe to call per message.
    void ensureControlChannel();
    // The actual write half of sendControl(). False = no open channel.
    bool deliverControl(const QByteArray& json);
    // Replays m_pendingControl; called from every channel's onOpen.
    void flushPendingControl();
    void flushPendingCandidates();
    // Fires a queued renegotiation once the signaling state is stable.
    void maybeRenegotiateAgain();
    // Build the RTP handler chain (packetizer → SR → NACK → PLI →
    // depacketizer → receiving session) on a track, either one we
    // added (offerer) or one delivered by onTrack (answerer).
    void attachVideoTrack(VideoStreamId stream, std::shared_ptr<rtc::Track> track,
                          bool adopted);
    void setupLosslessChannel(std::shared_ptr<rtc::DataChannel> dc);
    // Size-checked, exception-safe DataChannel send. Returns false if
    // the frame was dropped (oversized or transport error). Callers
    // must have verified m_dc is present and open.
    bool sendOnDataChannel(rtc::binary&& data, const char* what);
    // Uniform degradation for a libdatachannel entry point that threw.
    // EVERY rtc:: signaling call in this class runs synchronously inside
    // a Qt slot (ServerConnection::processSyncResponse → VoiceEngine →
    // here), so an escaped exception unwinds the event loop and
    // fail-fasts the whole app — the same trap sendOnDataChannel()
    // already guards on the media path. setRemoteDescription throws
    // std::logic_error for a type/signaling-state mismatch (a duplicated
    // m.call.answer is enough) and std::invalid_argument for malformed
    // SDP, and both are attacker- or bug-reachable from the timeline.
    // Log, mark the peer Failed, and let VoiceEngine's dead-peer cleanup
    // plus the 5 s mesh reconciler rebuild the connection.
    //
    // MUST be the last statement in its catch block: the state change is
    // emitted synchronously and VoiceEngine reacts by removing (and
    // deleteLater-ing) this peer.
    void failPeer(const char* where, const std::exception& e);
    // THE list of libdatachannel objects this class owns. Every teardown
    // step walks it, so a new channel or track is added in ONE place and
    // is then covered by both the callback detach and the close. If you
    // add an rtc handle member, add it here — that is the whole contract.
    template <typename Fn>
    void forEachRtcHandle(Fn&& fn)
    {
        for (auto& ctx : m_video) if (ctx.track) fn(ctx.track);
        if (m_losslessDc) fn(m_losslessDc);
        if (m_controlDc) fn(m_controlDc);
        if (m_dc) fn(m_dc);
        if (m_pc) fn(m_pc);
    }
    // Detach every libdatachannel callback and mark this object dead.
    // Runs FIRST in the destructor, before any close(). See the definition
    // for why the order matters — V-C2/S-6.
    void resetAllCallbacks() noexcept;
    // close() every handle, swallowing the exceptions close() can raise on
    // a teardown race (a throwing destructor is std::terminate).
    void closeAllHandles() noexcept;
    // True when the vscreen/vcamera m-lines already exist in either
    // negotiated description. Guards ensureVideoTracks() against adding
    // a duplicate m-line — see the comment there.
    bool videoMidsAlreadyDeclared() const;

    QString m_peerId;
    QString m_callId;
    // Cleared by resetAllCallbacks() before this object is destroyed.
    // Every libdatachannel callback captures a copy and returns
    // immediately once it is false, so a callback that was already past
    // the dispatcher's lock cannot touch a freed `this`.
    std::shared_ptr<std::atomic_bool> m_alive =
        std::make_shared<std::atomic_bool>(true);
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
        // PacedRtpSender (file-local type, so held as the base).
        std::shared_ptr<rtc::MediaHandler> pacer;
        qint64 startTimeUs = -1;
        bool open = false;
        // Set by the RtpGapDetector (libdatachannel network thread),
        // consumed by onFrame on the same thread; atomic as cheap
        // insurance against future callers.
        std::atomic<bool> lossPending{false};
        // First-traffic markers so logs positively show media moving
        // (or not) in each direction — "tracks open, then silence"
        // debugging without these cost a whole evening.
        bool txLogged = false;
        std::atomic<bool> rxLogged{false};
        qint64 txSkipLogMs = 0;
    };
    VideoTrackCtx m_video[kVideoStreamCount];
    // Pacer ceiling per stream, remembered so a track attached later
    // (onTrack on the answerer side) starts at the right budget.
    int m_pacerCeilingKbps[kVideoStreamCount] = {};
    // Matches the old fixed rtc::PacingHandler budget, used until the
    // share controller reports the encoder's real ceiling.
    static constexpr int kDefaultPacerCeilingKbps = 20000;
    QTimer* m_srTimer = nullptr;   // 1 s sender-report tick, lazily created
    quint64 m_txFrames[kVideoStreamCount] = {};
    quint64 m_txBytes[kVideoStreamCount] = {};

    // Reliable, ordered control channel ("control"). Opened lazily by
    // the OFFERER once the peer's caps advertise control_dc; the
    // answerer adopts it via the onDataChannel label match. Null
    // against a peer running a build that predates it — sendControl()
    // then falls back to m_dc, which is what every build did before.
    //
    // NOTE for the teardown work (S-6/V-C2): this channel's onMessage
    // callback captures `this` exactly like m_dc's, so whatever
    // ~PeerConnectionManager ends up doing to m_dc (close +
    // resetCallbacks) MUST also be done to m_controlDc.
    std::shared_ptr<rtc::DataChannel> m_controlDc;

    // Control messages that arrived before any channel could carry
    // them. sendControl() is driven by the caps handshake, which
    // completes one SDP round trip BEFORE the audio channel opens — so
    // the "a share is already running" replay to a peer joining
    // mid-share was, without this, written to a closed channel and
    // lost. Touched only on the Qt main thread (sendControl runs there,
    // and every onOpen hops through a queued invocation).
    QList<QByteArray> m_pendingControl;
    static constexpr int kMaxPendingControl = 32;

    // Lossless channel (created lazily by the sending side; adopted
    // via onDataChannel label match on the receiving side).
    std::shared_ptr<rtc::DataChannel> m_losslessDc;
    quint32 m_losslessSeq[kVideoStreamCount] = {};
    // Receive-side reassembly for chunked lossless frames (flags bit
    // 0x2). The channel is reliable+ordered, so chunks of one frame
    // arrive contiguously; state is touched only on the channel's
    // callback thread.
    QByteArray m_losslessAsm[kVideoStreamCount];
    quint32 m_losslessAsmSeq[kVideoStreamCount] = {};
    quint16 m_losslessAsmNext[kVideoStreamCount] = {};
    // Throttle for losslessSendStalled (ms epoch of last emission).
    qint64 m_losslessStallEmitMs = 0;
};
