#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>

#include "voice/PeerCaps.h"

#include <rtc/rtc.hpp>
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
    // 0x05 lossless-video payload (framing stripped by the receiver
    // pipeline, not here). Wired up by the AV1 lossless tier.
    void losslessFrameReceived(const QByteArray& payload);

private:
    void setupCallbacks();
    void setupDataChannel(std::shared_ptr<rtc::DataChannel> dc);
    void flushPendingCandidates();
    // Fires a queued renegotiation once the signaling state is stable.
    void maybeRenegotiateAgain();

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
};
