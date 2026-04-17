#pragma once

#include <QObject>
#include <QMap>
#include <QTimer>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class MatrixClient;
class AudioEngine;
class PeerConnectionManager;

class VoiceEngine : public QObject {
    Q_OBJECT
public:
    explicit VoiceEngine(MatrixClient* client, QObject* parent = nullptr);
    ~VoiceEngine();

    void start(const QString& roomId, const QJsonArray& members, const QJsonObject& turnConfig);
    void stop();
    bool isRunning() const { return m_running; }

    void handleCallInvite(const QString& sender, const QString& callId, const std::string& sdp);
    void handleCallAnswer(const QString& sender, const QString& callId, const std::string& sdp);
    void handleCallCandidates(const QString& sender, const QString& callId,
                               const std::vector<std::pair<std::string, std::string>>& candidates);
    void handleCallHangup(const QString& sender, const QString& callId);

    void setMuted(bool muted);
    void setDeafened(bool deafened);

    float micLevel() const { return m_micLevel; }

    // Per-peer connection state keyed by user-id. "connected", "connecting",
    // "failed", "new", "disconnected". VoicePanel reads this to show colored
    // indicators per member.
    QMap<QString, QString> peerStates() const;

signals:
    void peerConnected(const QString& userId);
    void peerDisconnected(const QString& userId);
    void peerStateChanged(const QString& userId, const QString& state);
    void error(const QString& message);
    void micLevelChanged(float level);

private:
    void addPeer(const QString& userId, bool isOfferer);
    void removePeer(const QString& userId);
    void onLocalDescription(const QString& peerId, const std::string& type, const std::string& sdp);
    void onLocalCandidate(const QString& peerId, const std::string& candidate, const std::string& mid);
    void flushCandidateBatch();
    void sendCallEvent(const QString& eventType, const nlohmann::json& content);
    rtc::Configuration buildRtcConfig() const;
    QString generateCallId() const;

    MatrixClient* m_client;
    QString m_roomId;
    AudioEngine* m_audioEngine = nullptr;
    QMap<QString, PeerConnectionManager*> m_peers;
    QMap<QString, QString> m_callIds;
    QJsonObject m_turnConfig;
    bool m_running = false;
    bool m_allowP2P = false;

    // ICE candidate batching
    QTimer m_candidateBatchTimer;
    QMap<QString, std::vector<std::pair<std::string, std::string>>> m_pendingCandidates;

    float m_micLevel = 0.0f;
};
