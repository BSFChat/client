#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>

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
    void sendAudioFrame(const QByteArray& frame);

    // Connection quality
    enum class PeerState { New, Connecting, Connected, Disconnected, Failed };
    Q_ENUM(PeerState)
    PeerState peerState() const { return m_peerState; }

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

private:
    void setupCallbacks();
    void setupDataChannel(std::shared_ptr<rtc::DataChannel> dc);
    void flushPendingCandidates();

    QString m_peerId;
    QString m_callId;
    std::shared_ptr<rtc::PeerConnection> m_pc;
    std::shared_ptr<rtc::DataChannel> m_dc;
    std::vector<std::pair<std::string, std::string>> m_pendingCandidates;
    bool m_remoteDescriptionSet = false;
    PeerState m_peerState = PeerState::New;
    int m_framesSent = 0;
    int m_framesReceived = 0;
};
