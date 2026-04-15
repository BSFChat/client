#include "voice/VoiceEngine.h"
#include "voice/AudioEngine.h"
#include "voice/PeerConnectionManager.h"
#include "net/MatrixClient.h"

#include <bsfchat/Constants.h>

#include <QDateTime>
#include <QRandomGenerator>
#include <QDebug>

VoiceEngine::VoiceEngine(MatrixClient* client, QObject* parent)
    : QObject(parent)
    , m_client(client)
{
    m_candidateBatchTimer.setInterval(500);
    m_candidateBatchTimer.setSingleShot(false);
    connect(&m_candidateBatchTimer, &QTimer::timeout, this, &VoiceEngine::flushCandidateBatch);
}

VoiceEngine::~VoiceEngine() {
    stop();
}

void VoiceEngine::start(const QString& roomId, const QJsonArray& members, const QJsonObject& turnConfig) {
    if (m_running) stop();

    m_roomId = roomId;
    m_turnConfig = turnConfig;
    m_allowP2P = turnConfig.value("allow_p2p").toBool(false);
    m_running = true;

    // Start audio engine
    m_audioEngine = new AudioEngine(this);
    if (!m_audioEngine->start()) {
        qWarning() << "Failed to start audio engine";
        emit error("Failed to initialize audio");
    }

    m_candidateBatchTimer.start();

    // Initiate connections to existing members
    for (const auto& memberVal : members) {
        auto member = memberVal.toObject();
        QString userId = member.value("user_id").toString();
        if (!userId.isEmpty()) {
            addPeer(userId, true); // We are the offerer (we just joined)
        }
    }
}

void VoiceEngine::stop() {
    if (!m_running) return;
    m_running = false;

    m_candidateBatchTimer.stop();

    // Send hangup to all peers
    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
        nlohmann::json content = {
            {"call_id", m_callIds.value(it.key()).toStdString()},
            {"reason", "user_hangup"},
            {"version", 1}
        };
        sendCallEvent(QString::fromUtf8(bsfchat::event_type::kCallHangup), content);
    }

    // Clean up peers
    qDeleteAll(m_peers);
    m_peers.clear();
    m_callIds.clear();
    m_pendingCandidates.clear();

    // Stop audio
    if (m_audioEngine) {
        m_audioEngine->stop();
        delete m_audioEngine;
        m_audioEngine = nullptr;
    }
}

void VoiceEngine::addPeer(const QString& userId, bool isOfferer) {
    if (m_peers.contains(userId)) return;

    QString callId = generateCallId();
    m_callIds[userId] = callId;

    auto config = buildRtcConfig();
    auto* peer = new PeerConnectionManager(userId, callId, config, this);
    m_peers[userId] = peer;

    // Connect signaling
    connect(peer, &PeerConnectionManager::localDescriptionReady,
            this, [this, userId](const std::string& type, const std::string& sdp) {
                onLocalDescription(userId, type, sdp);
            });

    connect(peer, &PeerConnectionManager::localCandidateReady,
            this, [this, userId](const std::string& candidate, const std::string& mid) {
                onLocalCandidate(userId, candidate, mid);
            });

    // Connect audio
    if (m_audioEngine) {
        connect(peer, &PeerConnectionManager::audioFrameReceived,
                this, [this, userId](const QByteArray& frame) {
                    if (m_audioEngine) m_audioEngine->receivePeerAudio(userId, frame);
                });

        connect(m_audioEngine, &AudioEngine::audioFrameReady,
                peer, &PeerConnectionManager::sendAudioFrame);
    }

    connect(peer, &PeerConnectionManager::connected,
            this, [this, userId]() { emit peerConnected(userId); });

    connect(peer, &PeerConnectionManager::disconnected,
            this, [this, userId]() { emit peerDisconnected(userId); });

    if (isOfferer) {
        peer->createOffer();
    }
}

void VoiceEngine::removePeer(const QString& userId) {
    if (auto* peer = m_peers.take(userId)) {
        if (m_audioEngine) m_audioEngine->removePeer(userId);
        peer->deleteLater();
    }
    m_callIds.remove(userId);
    m_pendingCandidates.remove(userId);
}

void VoiceEngine::handleCallInvite(const QString& sender, const QString& callId, const std::string& sdp) {
    if (!m_running || m_peers.contains(sender)) return;

    m_callIds[sender] = callId;

    auto config = buildRtcConfig();
    auto* peer = new PeerConnectionManager(sender, callId, config, this);
    m_peers[sender] = peer;

    connect(peer, &PeerConnectionManager::localDescriptionReady,
            this, [this, sender](const std::string& type, const std::string& sdpOut) {
                onLocalDescription(sender, type, sdpOut);
            });

    connect(peer, &PeerConnectionManager::localCandidateReady,
            this, [this, sender](const std::string& candidate, const std::string& mid) {
                onLocalCandidate(sender, candidate, mid);
            });

    if (m_audioEngine) {
        connect(peer, &PeerConnectionManager::audioFrameReceived,
                this, [this, sender](const QByteArray& frame) {
                    if (m_audioEngine) m_audioEngine->receivePeerAudio(sender, frame);
                });

        connect(m_audioEngine, &AudioEngine::audioFrameReady,
                peer, &PeerConnectionManager::sendAudioFrame);
    }

    connect(peer, &PeerConnectionManager::connected,
            this, [this, sender]() { emit peerConnected(sender); });

    connect(peer, &PeerConnectionManager::disconnected,
            this, [this, sender]() { emit peerDisconnected(sender); });

    peer->applyOffer(sdp);
}

void VoiceEngine::handleCallAnswer(const QString& sender, const QString& callId, const std::string& sdp) {
    if (auto* peer = m_peers.value(sender)) {
        peer->applyAnswer(sdp);
    }
}

void VoiceEngine::handleCallCandidates(const QString& sender, const QString& callId,
                                        const std::vector<std::pair<std::string, std::string>>& candidates) {
    if (auto* peer = m_peers.value(sender)) {
        for (const auto& [cand, mid] : candidates) {
            peer->addRemoteCandidate(cand, mid);
        }
    }
}

void VoiceEngine::handleCallHangup(const QString& sender, const QString& callId) {
    removePeer(sender);
}

void VoiceEngine::setMuted(bool muted) {
    if (m_audioEngine) m_audioEngine->setMuted(muted);
}

void VoiceEngine::setDeafened(bool deafened) {
    if (m_audioEngine) m_audioEngine->setDeafened(deafened);
}

void VoiceEngine::onLocalDescription(const QString& peerId, const std::string& type, const std::string& sdp) {
    auto callId = m_callIds.value(peerId);

    if (type == "offer") {
        nlohmann::json content = {
            {"call_id", callId.toStdString()},
            {"lifetime", 60000},
            {"offer", {{"type", "offer"}, {"sdp", sdp}}},
            {"version", 1}
        };
        sendCallEvent(QString::fromUtf8(bsfchat::event_type::kCallInvite), content);
    } else if (type == "answer") {
        nlohmann::json content = {
            {"call_id", callId.toStdString()},
            {"answer", {{"type", "answer"}, {"sdp", sdp}}},
            {"version", 1}
        };
        sendCallEvent(QString::fromUtf8(bsfchat::event_type::kCallAnswer), content);
    }
}

void VoiceEngine::onLocalCandidate(const QString& peerId, const std::string& candidate, const std::string& mid) {
    m_pendingCandidates[peerId].emplace_back(candidate, mid);
}

void VoiceEngine::flushCandidateBatch() {
    for (auto it = m_pendingCandidates.begin(); it != m_pendingCandidates.end(); ) {
        if (it.value().empty()) {
            it = m_pendingCandidates.erase(it);
            continue;
        }

        auto callId = m_callIds.value(it.key());
        nlohmann::json candidates = nlohmann::json::array();
        for (const auto& [cand, mid] : it.value()) {
            candidates.push_back({
                {"candidate", cand},
                {"sdpMid", mid},
                {"sdpMLineIndex", 0}
            });
        }

        nlohmann::json content = {
            {"call_id", callId.toStdString()},
            {"candidates", candidates},
            {"version", 1}
        };
        sendCallEvent(QString::fromUtf8(bsfchat::event_type::kCallCandidates), content);

        it.value().clear();
        ++it;
    }
}

void VoiceEngine::sendCallEvent(const QString& eventType, const nlohmann::json& content) {
    m_client->sendRoomEvent(m_roomId, eventType,
                            QByteArray::fromStdString(content.dump()));
}

rtc::Configuration VoiceEngine::buildRtcConfig() const {
    rtc::Configuration config;

    // Add TURN server
    QString turnUri = m_turnConfig.value("uris").toArray().isEmpty()
        ? QString() : m_turnConfig.value("uris").toArray().first().toString();

    if (!turnUri.isEmpty() && turnUri.startsWith("turn:")) {
        // Embed credentials in URL: turn:user:pass@host:port
        auto user = m_turnConfig.value("username").toString();
        auto pass = m_turnConfig.value("password").toString();
        QString turnUrl = turnUri;
        if (!user.isEmpty()) {
            // turn:host:port -> turn:user:pass@host:port
            turnUrl = QString("turn:%1:%2@%3").arg(user, pass, turnUri.mid(5));
        }
        config.iceServers.emplace_back(turnUrl.toStdString());
    }

    // Add STUN servers from uris
    for (const auto& uri : m_turnConfig.value("uris").toArray()) {
        QString u = uri.toString();
        if (u.startsWith("stun:")) {
            rtc::IceServer stun(u.toStdString());
            config.iceServers.push_back(std::move(stun));
        }
    }

    // No default STUN/TURN — server admin must configure their own.
    // On LAN with P2P enabled, direct connections work without STUN.

    if (!m_allowP2P) {
        config.iceTransportPolicy = rtc::TransportPolicy::Relay;
    }

    return config;
}

QString VoiceEngine::generateCallId() const {
    return QString("call-%1-%2")
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(QRandomGenerator::global()->generate(), 8, 16, QChar('0'));
}
