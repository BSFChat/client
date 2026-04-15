#include "voice/PeerConnectionManager.h"
#include <QDebug>

PeerConnectionManager::PeerConnectionManager(const QString& peerId, const QString& callId,
                                             const rtc::Configuration& config, QObject* parent)
    : QObject(parent)
    , m_peerId(peerId)
    , m_callId(callId)
{
    m_pc = std::make_shared<rtc::PeerConnection>(config);
    setupCallbacks();
}

PeerConnectionManager::~PeerConnectionManager() {
    if (m_dc) m_dc->close();
    if (m_pc) m_pc->close();
}

void PeerConnectionManager::setupCallbacks() {
    m_pc->onLocalDescription([this](rtc::Description desc) {
        std::string type = desc.typeString();
        std::string sdp = std::string(desc);
        QMetaObject::invokeMethod(this, [this, type, sdp]() {
            emit localDescriptionReady(type, sdp);
        }, Qt::QueuedConnection);
    });

    m_pc->onLocalCandidate([this](rtc::Candidate candidate) {
        std::string cand = std::string(candidate);
        std::string mid = candidate.mid();
        QMetaObject::invokeMethod(this, [this, cand, mid]() {
            emit localCandidateReady(cand, mid);
        }, Qt::QueuedConnection);
    });

    m_pc->onStateChange([this](rtc::PeerConnection::State state) {
        QMetaObject::invokeMethod(this, [this, state]() {
            if (state == rtc::PeerConnection::State::Connected) {
                emit connected();
            } else if (state == rtc::PeerConnection::State::Disconnected ||
                       state == rtc::PeerConnection::State::Failed ||
                       state == rtc::PeerConnection::State::Closed) {
                emit disconnected();
            }
        }, Qt::QueuedConnection);
    });

    m_pc->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
        QMetaObject::invokeMethod(this, [this, dc]() {
            setupDataChannel(dc);
        }, Qt::QueuedConnection);
    });
}

void PeerConnectionManager::setupDataChannel(std::shared_ptr<rtc::DataChannel> dc) {
    m_dc = dc;

    m_dc->onMessage([this](rtc::message_variant msg) {
        if (std::holds_alternative<rtc::binary>(msg)) {
            auto& data = std::get<rtc::binary>(msg);
            QByteArray frame(reinterpret_cast<const char*>(data.data()),
                           static_cast<int>(data.size()));
            QMetaObject::invokeMethod(this, [this, frame]() {
                emit audioFrameReceived(frame);
            }, Qt::QueuedConnection);
        }
    });
}

void PeerConnectionManager::createOffer() {
    // Create unreliable DataChannel for audio
    rtc::DataChannelInit dcInit;
    dcInit.reliability.unordered = true;
    dcInit.reliability.maxRetransmits = 0;

    auto dc = m_pc->createDataChannel("audio", dcInit);
    setupDataChannel(dc);

    m_pc->setLocalDescription(rtc::Description::Type::Offer);
}

void PeerConnectionManager::applyOffer(const std::string& sdp) {
    rtc::Description desc(sdp, rtc::Description::Type::Offer);
    m_pc->setRemoteDescription(desc);
    m_remoteDescriptionSet = true;
    flushPendingCandidates();

    m_pc->setLocalDescription(rtc::Description::Type::Answer);
}

void PeerConnectionManager::applyAnswer(const std::string& sdp) {
    rtc::Description desc(sdp, rtc::Description::Type::Answer);
    m_pc->setRemoteDescription(desc);
    m_remoteDescriptionSet = true;
    flushPendingCandidates();
}

void PeerConnectionManager::addRemoteCandidate(const std::string& candidate, const std::string& mid) {
    if (m_remoteDescriptionSet) {
        m_pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
    } else {
        m_pendingCandidates.emplace_back(candidate, mid);
    }
}

void PeerConnectionManager::flushPendingCandidates() {
    for (const auto& [cand, mid] : m_pendingCandidates) {
        m_pc->addRemoteCandidate(rtc::Candidate(cand, mid));
    }
    m_pendingCandidates.clear();
}

void PeerConnectionManager::sendAudioFrame(const QByteArray& frame) {
    if (m_dc && m_dc->isOpen()) {
        auto* raw = reinterpret_cast<const std::byte*>(frame.constData());
        rtc::binary data(raw, raw + frame.size());
        m_dc->send(data);
    }
}
