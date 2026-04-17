#include "voice/PeerConnectionManager.h"
#include <QDebug>

namespace {
const char* stateStr(rtc::PeerConnection::State s) {
    switch (s) {
    case rtc::PeerConnection::State::New:          return "New";
    case rtc::PeerConnection::State::Connecting:   return "Connecting";
    case rtc::PeerConnection::State::Connected:    return "Connected";
    case rtc::PeerConnection::State::Disconnected: return "Disconnected";
    case rtc::PeerConnection::State::Failed:       return "Failed";
    case rtc::PeerConnection::State::Closed:       return "Closed";
    }
    return "?";
}
const char* iceStr(rtc::PeerConnection::IceState s) {
    switch (s) {
    case rtc::PeerConnection::IceState::New:          return "New";
    case rtc::PeerConnection::IceState::Checking:     return "Checking";
    case rtc::PeerConnection::IceState::Connected:    return "Connected";
    case rtc::PeerConnection::IceState::Completed:    return "Completed";
    case rtc::PeerConnection::IceState::Failed:       return "Failed";
    case rtc::PeerConnection::IceState::Disconnected: return "Disconnected";
    case rtc::PeerConnection::IceState::Closed:       return "Closed";
    }
    return "?";
}
const char* gatherStr(rtc::PeerConnection::GatheringState s) {
    switch (s) {
    case rtc::PeerConnection::GatheringState::New:      return "New";
    case rtc::PeerConnection::GatheringState::InProgress: return "InProgress";
    case rtc::PeerConnection::GatheringState::Complete: return "Complete";
    }
    return "?";
}
} // namespace

PeerConnectionManager::PeerConnectionManager(const QString& peerId, const QString& callId,
                                             const rtc::Configuration& config, QObject* parent)
    : QObject(parent)
    , m_peerId(peerId)
    , m_callId(callId)
{
    qInfo("[voice] Creating peer connection → %s (call %s)",
          qPrintable(peerId), qPrintable(callId));
    m_pc = std::make_shared<rtc::PeerConnection>(config);
    setupCallbacks();
}

PeerConnectionManager::~PeerConnectionManager() {
    qInfo("[voice] Destroying peer connection → %s (sent=%d recv=%d)",
          qPrintable(m_peerId), m_framesSent, m_framesReceived);
    if (m_dc) m_dc->close();
    if (m_pc) m_pc->close();
}

void PeerConnectionManager::setupCallbacks() {
    m_pc->onLocalDescription([this](rtc::Description desc) {
        std::string type = desc.typeString();
        std::string sdp = std::string(desc);
        QMetaObject::invokeMethod(this, [this, type, sdp]() {
            qInfo("[voice] [%s] Local SDP %s ready",
                  qPrintable(m_peerId), type.c_str());
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
            qInfo("[voice] [%s] PeerConnection state: %s",
                  qPrintable(m_peerId), stateStr(state));

            PeerState newState = m_peerState;
            switch (state) {
            case rtc::PeerConnection::State::New:
                newState = PeerState::New; break;
            case rtc::PeerConnection::State::Connecting:
                newState = PeerState::Connecting; break;
            case rtc::PeerConnection::State::Connected:
                newState = PeerState::Connected; break;
            case rtc::PeerConnection::State::Disconnected:
                newState = PeerState::Disconnected; break;
            case rtc::PeerConnection::State::Failed:
            case rtc::PeerConnection::State::Closed:
                newState = PeerState::Failed; break;
            }

            if (newState != m_peerState) {
                m_peerState = newState;
                emit peerStateChanged(newState);
            }

            if (state == rtc::PeerConnection::State::Connected) {
                emit connected();
            } else if (state == rtc::PeerConnection::State::Disconnected ||
                       state == rtc::PeerConnection::State::Failed ||
                       state == rtc::PeerConnection::State::Closed) {
                emit disconnected();
            }
        }, Qt::QueuedConnection);
    });

    m_pc->onIceStateChange([this](rtc::PeerConnection::IceState state) {
        QMetaObject::invokeMethod(this, [this, state]() {
            qInfo("[voice] [%s] ICE state: %s",
                  qPrintable(m_peerId), iceStr(state));
        }, Qt::QueuedConnection);
    });

    m_pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
        QMetaObject::invokeMethod(this, [this, state]() {
            qInfo("[voice] [%s] ICE gathering: %s",
                  qPrintable(m_peerId), gatherStr(state));
        }, Qt::QueuedConnection);
    });

    m_pc->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
        QMetaObject::invokeMethod(this, [this, dc]() {
            qInfo("[voice] [%s] Incoming data channel",
                  qPrintable(m_peerId));
            setupDataChannel(dc);
        }, Qt::QueuedConnection);
    });
}

void PeerConnectionManager::setupDataChannel(std::shared_ptr<rtc::DataChannel> dc) {
    m_dc = dc;

    m_dc->onOpen([this]() {
        QMetaObject::invokeMethod(this, [this]() {
            qInfo("[voice] [%s] DataChannel open — audio can flow",
                  qPrintable(m_peerId));
        }, Qt::QueuedConnection);
    });

    m_dc->onClosed([this]() {
        QMetaObject::invokeMethod(this, [this]() {
            qInfo("[voice] [%s] DataChannel closed",
                  qPrintable(m_peerId));
        }, Qt::QueuedConnection);
    });

    m_dc->onMessage([this](rtc::message_variant msg) {
        if (std::holds_alternative<rtc::binary>(msg)) {
            auto& data = std::get<rtc::binary>(msg);
            QByteArray frame(reinterpret_cast<const char*>(data.data()),
                           static_cast<int>(data.size()));
            QMetaObject::invokeMethod(this, [this, frame]() {
                m_framesReceived++;
                emit audioFrameReceived(frame);
            }, Qt::QueuedConnection);
        }
    });
}

void PeerConnectionManager::createOffer() {
    qInfo("[voice] [%s] Creating offer (we are offerer)",
          qPrintable(m_peerId));
    // Create unreliable DataChannel for audio
    rtc::DataChannelInit dcInit;
    dcInit.reliability.unordered = true;
    dcInit.reliability.maxRetransmits = 0;

    auto dc = m_pc->createDataChannel("audio", dcInit);
    setupDataChannel(dc);

    m_pc->setLocalDescription(rtc::Description::Type::Offer);
}

void PeerConnectionManager::applyOffer(const std::string& sdp) {
    qInfo("[voice] [%s] Applying remote offer", qPrintable(m_peerId));
    rtc::Description desc(sdp, rtc::Description::Type::Offer);
    m_pc->setRemoteDescription(desc);
    m_remoteDescriptionSet = true;
    flushPendingCandidates();

    m_pc->setLocalDescription(rtc::Description::Type::Answer);
}

void PeerConnectionManager::applyAnswer(const std::string& sdp) {
    qInfo("[voice] [%s] Applying remote answer", qPrintable(m_peerId));
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
    if (!m_pendingCandidates.empty()) {
        qInfo("[voice] [%s] Flushing %d buffered ICE candidates",
              qPrintable(m_peerId), int(m_pendingCandidates.size()));
    }
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
        m_framesSent++;
    }
}
