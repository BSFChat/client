#include "voice/PeerConnectionManager.h"
#include <QDebug>
#include <QLoggingCategory>
#include <QRandomGenerator>

// Mirror VoiceEngine's category. Each TU owns its own QLoggingCategory
// instance — Qt coalesces them by name at runtime, so enabling
// `bsfchat.voice.info=true` picks up both this file and VoiceEngine.
Q_LOGGING_CATEGORY(logVoicePc, "bsfchat.voice", QtWarningMsg)

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

// Fixed per-stream track identity. Both endpoints run this code, so
// mids/PTs agree by construction; SSRCs are randomized per endpoint
// (RTP requires sender-unique SSRCs within a session).
struct VideoStreamSpec { const char* mid; uint8_t payloadType; const char* cname; };
constexpr VideoStreamSpec kVideoSpecs[kVideoStreamCount] = {
    {"vscreen", 96, "bsf-screen"},
    {"vcamera", 97, "bsf-camera"},
};

int streamIndexForMid(const std::string& mid) {
    for (int i = 0; i < kVideoStreamCount; ++i)
        if (mid == kVideoSpecs[i].mid) return i;
    return -1;
}
} // namespace

PeerConnectionManager::PeerConnectionManager(const QString& peerId, const QString& callId,
                                             const rtc::Configuration& config, QObject* parent)
    : QObject(parent)
    , m_peerId(peerId)
    , m_callId(callId)
{
    qCInfo(logVoicePc, " Creating peer connection → %s (call %s)",
          qPrintable(peerId), qPrintable(callId));
    m_pc = std::make_shared<rtc::PeerConnection>(config);
    setupCallbacks();
}

PeerConnectionManager::~PeerConnectionManager() {
    qCInfo(logVoicePc, " Destroying peer connection → %s (sent=%d recv=%d)",
          qPrintable(m_peerId), m_framesSent, m_framesReceived);
    if (m_dc) m_dc->close();
    if (m_pc) m_pc->close();
}

void PeerConnectionManager::setupCallbacks() {
    m_pc->onLocalDescription([this](rtc::Description desc) {
        std::string type = desc.typeString();
        std::string sdp = std::string(desc);
        QMetaObject::invokeMethod(this, [this, type, sdp]() {
            qCInfo(logVoicePc, " [%s] Local SDP %s ready",
                  qPrintable(m_peerId), type.c_str());
            emit localDescriptionReady(type, sdp);
            // The emit above ran VoiceEngine's routing synchronously,
            // so the FIRST answer went out as m.call.answer while the
            // flag was still false; flipping it afterwards makes every
            // subsequent description take the negotiate path. (The
            // offerer side flips in applyAnswer instead.)
            if (type == "answer" && !m_initialNegotiationDone)
                m_initialNegotiationDone = true;
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
            qCInfo(logVoicePc, " [%s] PeerConnection state: %s",
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
            qCInfo(logVoicePc, " [%s] ICE state: %s",
                  qPrintable(m_peerId), iceStr(state));
        }, Qt::QueuedConnection);
    });

    m_pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
        QMetaObject::invokeMethod(this, [this, state]() {
            qCInfo(logVoicePc, " [%s] ICE gathering: %s",
                  qPrintable(m_peerId), gatherStr(state));
        }, Qt::QueuedConnection);
    });

    m_pc->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
        QMetaObject::invokeMethod(this, [this, dc]() {
            qCInfo(logVoicePc, " [%s] Incoming data channel",
                  qPrintable(m_peerId));
            setupDataChannel(dc);
        }, Qt::QueuedConnection);
    });

    // Remote added video m-lines via renegotiation (we're the
    // answerer side of the video upgrade) — adopt the tracks; the
    // m-lines are SendRecv so this same track carries our outgoing
    // video without another renegotiation.
    m_pc->onTrack([this](std::shared_ptr<rtc::Track> track) {
        QMetaObject::invokeMethod(this, [this, track]() {
            const int idx = streamIndexForMid(track->mid());
            qCInfo(logVoicePc, " [%s] Incoming track mid=%s",
                  qPrintable(m_peerId), track->mid().c_str());
            if (idx < 0) return;   // unknown m-line — future stream kind
            if (m_video[idx].track) return;   // already have it
            attachVideoTrack(VideoStreamId(idx), track);
        }, Qt::QueuedConnection);
    });
}

void PeerConnectionManager::setupDataChannel(std::shared_ptr<rtc::DataChannel> dc) {
    m_dc = dc;

    m_dc->onOpen([this]() {
        QMetaObject::invokeMethod(this, [this]() {
            qCInfo(logVoicePc, " [%s] DataChannel open — audio can flow",
                  qPrintable(m_peerId));
        }, Qt::QueuedConnection);
    });

    m_dc->onClosed([this]() {
        QMetaObject::invokeMethod(this, [this]() {
            qCInfo(logVoicePc, " [%s] DataChannel closed",
                  qPrintable(m_peerId));
        }, Qt::QueuedConnection);
    });

    m_dc->onMessage([this](rtc::message_variant msg) {
        if (!std::holds_alternative<rtc::binary>(msg)) return;
        auto& data = std::get<rtc::binary>(msg);
        if (data.empty()) return;
        // Type-tag framing. First byte = frame kind. Older audio-only
        // peers (pre-screen-share) sent untagged Opus; those look
        // indistinguishable from tag=OPUS_FIRST_BYTE here, so we
        // treat an unknown tag as an audio frame with no tag strip
        // — preserves backwards compatibility with legacy peers.
        uint8_t tag = static_cast<uint8_t>(data[0]);
        QByteArray payload;
        if (tag == 0x01) {
            payload = QByteArray(reinterpret_cast<const char*>(data.data() + 1),
                                 static_cast<int>(data.size() - 1));
            QMetaObject::invokeMethod(this, [this, payload]() {
                m_framesReceived++;
                emit audioFrameReceived(payload);
            }, Qt::QueuedConnection);
        } else if (tag == 0x02) {
            payload = QByteArray(reinterpret_cast<const char*>(data.data() + 1),
                                 static_cast<int>(data.size() - 1));
            QMetaObject::invokeMethod(this, [this, payload]() {
                emit screenFrameReceived(payload);
            }, Qt::QueuedConnection);
        } else if (tag == 0x03) {
            payload = QByteArray(reinterpret_cast<const char*>(data.data() + 1),
                                 static_cast<int>(data.size() - 1));
            QMetaObject::invokeMethod(this, [this, payload]() {
                emit cameraFrameReceived(payload);
            }, Qt::QueuedConnection);
        } else if (tag == 0x04) {
            // JSON control message (caps refresh / keyframe request /
            // receiver report). Only new clients ever send these —
            // gated by the caps handshake on the send side.
            payload = QByteArray(reinterpret_cast<const char*>(data.data() + 1),
                                 static_cast<int>(data.size() - 1));
            QMetaObject::invokeMethod(this, [this, payload]() {
                emit controlMessageReceived(payload);
            }, Qt::QueuedConnection);
        } else if (tag == 0x05) {
            // Lossless-video frame (AV1 over the reliable channel).
            payload = QByteArray(reinterpret_cast<const char*>(data.data() + 1),
                                 static_cast<int>(data.size() - 1));
            QMetaObject::invokeMethod(this, [this, payload]() {
                emit losslessFrameReceived(payload);
            }, Qt::QueuedConnection);
        } else {
            // Legacy untagged audio — no tag strip.
            payload = QByteArray(reinterpret_cast<const char*>(data.data()),
                                 static_cast<int>(data.size()));
            QMetaObject::invokeMethod(this, [this, payload]() {
                m_framesReceived++;
                emit audioFrameReceived(payload);
            }, Qt::QueuedConnection);
        }
    });
}

void PeerConnectionManager::createOffer() {
    qCInfo(logVoicePc, " [%s] Creating offer (we are offerer)",
          qPrintable(m_peerId));
    m_isOfferer = true;
    // Create unreliable DataChannel for audio
    rtc::DataChannelInit dcInit;
    dcInit.reliability.unordered = true;
    dcInit.reliability.maxRetransmits = 0;

    auto dc = m_pc->createDataChannel("audio", dcInit);
    setupDataChannel(dc);

    m_pc->setLocalDescription(rtc::Description::Type::Offer);
}

void PeerConnectionManager::applyOffer(const std::string& sdp) {
    qCInfo(logVoicePc, " [%s] Applying remote offer", qPrintable(m_peerId));
    rtc::Description desc(sdp, rtc::Description::Type::Offer);
    m_pc->setRemoteDescription(desc);
    m_remoteDescriptionSet = true;
    flushPendingCandidates();

    m_pc->setLocalDescription(rtc::Description::Type::Answer);
}

void PeerConnectionManager::applyAnswer(const std::string& sdp) {
    qCInfo(logVoicePc, " [%s] Applying remote answer", qPrintable(m_peerId));
    rtc::Description desc(sdp, rtc::Description::Type::Answer);
    m_pc->setRemoteDescription(desc);
    m_remoteDescriptionSet = true;
    m_initialNegotiationDone = true;
    flushPendingCandidates();
}

void PeerConnectionManager::triggerRenegotiation() {
    if (!m_pc) return;
    if (m_localReofferPending
        || m_pc->signalingState() != rtc::PeerConnection::SignalingState::Stable) {
        // Mid-exchange — queue and re-fire from maybeRenegotiateAgain()
        // once the connection settles.
        m_renegotiateAgain = true;
        return;
    }
    qCInfo(logVoicePc, " [%s] Triggering renegotiation", qPrintable(m_peerId));
    m_localReofferPending = true;
    // Unspec in stable state ⇒ a fresh offer reflecting current
    // tracks/channels, delivered through onLocalDescription and routed
    // to bsfchat.call.negotiate by VoiceEngine.
    m_pc->setLocalDescription();
}

void PeerConnectionManager::applyNegotiateOffer(const std::string& sdp) {
    qCInfo(logVoicePc, " [%s] Applying renegotiation offer", qPrintable(m_peerId));
    rtc::Description desc(sdp, rtc::Description::Type::Offer);
    m_pc->setRemoteDescription(desc);
    m_pc->setLocalDescription(rtc::Description::Type::Answer);
    maybeRenegotiateAgain();
}

void PeerConnectionManager::applyNegotiateAnswer(const std::string& sdp) {
    if (!m_localReofferPending) {
        qCInfo(logVoicePc, " [%s] Ignoring unexpected negotiate answer",
              qPrintable(m_peerId));
        return;
    }
    qCInfo(logVoicePc, " [%s] Applying renegotiation answer", qPrintable(m_peerId));
    rtc::Description desc(sdp, rtc::Description::Type::Answer);
    m_pc->setRemoteDescription(desc);
    m_localReofferPending = false;
    maybeRenegotiateAgain();
}

void PeerConnectionManager::rollbackLocalReoffer() {
    if (!m_localReofferPending) return;
    qCInfo(logVoicePc, " [%s] Rolling back local re-offer (glare, polite side)",
          qPrintable(m_peerId));
    m_pc->setLocalDescription(rtc::Description::Type::Rollback);
    m_localReofferPending = false;
    // Whatever we wanted to negotiate (added tracks) is still attached
    // to the PC — re-offer once the winning exchange completes.
    m_renegotiateAgain = true;
}

void PeerConnectionManager::maybeRenegotiateAgain() {
    if (!m_renegotiateAgain) return;
    if (m_pc->signalingState() != rtc::PeerConnection::SignalingState::Stable) return;
    m_renegotiateAgain = false;
    triggerRenegotiation();
}

void PeerConnectionManager::sendControl(const QByteArray& json) {
    if (!m_dc || !m_dc->isOpen()) return;
    // HARD compatibility gate: legacy clients misparse unknown tags as
    // audio frames (see onMessage's fallback), so control traffic may
    // only flow once the caps handshake proved the peer understands it.
    if (!remoteSupportsVideoRtp()) return;
    rtc::binary data;
    data.reserve(json.size() + 1);
    data.push_back(std::byte{0x04});
    auto* raw = reinterpret_cast<const std::byte*>(json.constData());
    data.insert(data.end(), raw, raw + json.size());
    m_dc->send(data);
}

void PeerConnectionManager::ensureVideoTracks() {
    if (!m_pc || m_video[0].track) return;
    if (!remoteSupportsVideoRtp()) return;
    qCInfo(logVoicePc, " [%s] Adding video tracks + renegotiating",
          qPrintable(m_peerId));
    for (int i = 0; i < kVideoStreamCount; ++i) {
        rtc::Description::Video media(kVideoSpecs[i].mid,
                                      rtc::Description::Direction::SendRecv);
        media.addH264Codec(kVideoSpecs[i].payloadType);
        auto track = m_pc->addTrack(std::move(media));
        attachVideoTrack(VideoStreamId(i), track);
    }
    triggerRenegotiation();
}

void PeerConnectionManager::attachVideoTrack(VideoStreamId stream,
                                             std::shared_ptr<rtc::Track> track) {
    const int idx = int(stream);
    const auto& spec = kVideoSpecs[idx];
    auto& ctx = m_video[idx];

    ctx.track = track;
    ctx.startTimeUs = -1;
    ctx.rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
        QRandomGenerator::global()->generate(), spec.cname,
        spec.payloadType, rtc::H264RtpPacketizer::defaultClockRate);

    // Chain: outgoing traverses head→tail (packetize, then SR/NACK
    // bookkeeping); incoming traverses tail→head (RTCP session strips
    // control packets, depacketizer reassembles AUs for onFrame; the
    // send-side handlers pass incoming data through untouched).
    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::LongStartSequence, ctx.rtpConfig);
    ctx.srReporter = std::make_shared<rtc::RtcpSrReporter>(ctx.rtpConfig);
    packetizer->addToChain(ctx.srReporter);
    packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
    packetizer->addToChain(std::make_shared<rtc::PliHandler>([this, idx]() {
        QMetaObject::invokeMethod(this, [this, idx]() {
            emit keyframeRequestedByPeer(idx);
        }, Qt::QueuedConnection);
    }));
    // Pace outgoing RTP so a large IDR doesn't burst-blast the path in
    // one UDP salvo (bursts are what routers drop first). Budget sits
    // far above any configured bitrate — it only shaves peaks.
    packetizer->addToChain(std::make_shared<rtc::PacingHandler>(
        20'000'000.0, std::chrono::milliseconds(5)));
    packetizer->addToChain(std::make_shared<rtc::H264RtpDepacketizer>(
        rtc::NalUnit::Separator::LongStartSequence));
    packetizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
    track->setMediaHandler(packetizer);

    track->onFrame([this, idx](rtc::binary data, rtc::FrameInfo) {
        QByteArray au(reinterpret_cast<const char*>(data.data()),
                      int(data.size()));
        QMetaObject::invokeMethod(this, [this, idx, au]() {
            emit videoFrameReceived(idx, au);
        }, Qt::QueuedConnection);
    });
    track->onOpen([this, idx]() {
        QMetaObject::invokeMethod(this, [this, idx]() {
            qCInfo(logVoicePc, " [%s] Video track %s open",
                  qPrintable(m_peerId), kVideoSpecs[idx].mid);
            m_video[idx].open = true;
            emit videoTrackOpen(idx);
        }, Qt::QueuedConnection);
    });
    track->onClosed([this, idx]() {
        QMetaObject::invokeMethod(this, [this, idx]() {
            m_video[idx].open = false;
        }, Qt::QueuedConnection);
    });

    // Sender reports let receivers map RTP timestamps to wall clock.
    if (!m_srTimer) {
        m_srTimer = new QTimer(this);
        m_srTimer->setInterval(1000);
        connect(m_srTimer, &QTimer::timeout, this, [this]() {
            for (auto& v : m_video)
                if (v.srReporter && v.track && v.open)
                    v.srReporter->setNeedsToReport();
        });
        m_srTimer->start();
    }
}

bool PeerConnectionManager::hasVideoTrackOpen(VideoStreamId stream) const {
    const auto& ctx = m_video[int(stream)];
    return ctx.open && ctx.track && ctx.track->isOpen();
}

void PeerConnectionManager::sendVideoFrame(VideoStreamId stream,
                                           const EncodedFrame& frame) {
    auto& ctx = m_video[int(stream)];
    if (!ctx.open || !ctx.track || !ctx.track->isOpen()) return;
    if (ctx.startTimeUs < 0) ctx.startTimeUs = frame.captureTimeUs;
    const double elapsed = double(frame.captureTimeUs - ctx.startTimeUs) / 1e6;
    ctx.rtpConfig->timestamp = ctx.rtpConfig->startTimestamp
        + ctx.rtpConfig->secondsToTimestamp(elapsed);
    try {
        ctx.track->send(
            reinterpret_cast<const std::byte*>(frame.data.constData()),
            size_t(frame.data.size()));
        m_txFrames[int(stream)] += 1;
        m_txBytes[int(stream)] += quint64(frame.data.size());
    } catch (const std::exception& e) {
        // Transient (track closing mid-send) — the open flag will
        // catch up via onClosed; don't spam.
        qCDebug(logVoicePc, " [%s] video send failed: %s",
               qPrintable(m_peerId), e.what());
    }
}

void PeerConnectionManager::requestPeerKeyframe(VideoStreamId stream) {
    sendControl(QByteArrayLiteral("{\"t\":\"kf\",\"stream\":")
                + QByteArray::number(int(stream)) + "}");
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
        qCInfo(logVoicePc, " [%s] Flushing %d buffered ICE candidates",
              qPrintable(m_peerId), int(m_pendingCandidates.size()));
    }
    for (const auto& [cand, mid] : m_pendingCandidates) {
        m_pc->addRemoteCandidate(rtc::Candidate(cand, mid));
    }
    m_pendingCandidates.clear();
}

void PeerConnectionManager::sendAudioFrame(const QByteArray& frame) {
    if (m_dc && m_dc->isOpen()) {
        // Prepend the 0x01 audio tag so peers can distinguish from
        // screen-share frames on the shared data channel.
        rtc::binary data;
        data.reserve(frame.size() + 1);
        data.push_back(std::byte{0x01});
        auto* raw = reinterpret_cast<const std::byte*>(frame.constData());
        data.insert(data.end(), raw, raw + frame.size());
        m_dc->send(data);
        m_framesSent++;
    }
}

void PeerConnectionManager::sendScreenFrame(const QByteArray& jpegData) {
    if (!m_dc || !m_dc->isOpen()) return;
    // Backpressure: drop the frame if the data channel has
    // backed up beyond a sensible budget. Without this, raising
    // the user's fps/quality past what their uplink can sustain
    // makes libdatachannel's internal queue grow until SCTP
    // panics or memory blows out — symptoms users would read as
    // "the app is broken at high quality".
    //
    // Threshold is 4 MB (≈ 8 frames @ 500 KB ea, ≈ 0.5s of
    // backlog at 15 fps). Tuned against the worst case of a
    // 3840-px Q100 frame (~1.5 MB) so we drop after ~3 such
    // frames pile up, well before SCTP starts to misbehave.
    constexpr size_t kBufferedHighWatermark = 4 * 1024 * 1024;
    if (m_dc->bufferedAmount() > kBufferedHighWatermark) {
        ++m_screenFramesDropped;
        return;
    }
    rtc::binary data;
    data.reserve(jpegData.size() + 1);
    data.push_back(std::byte{0x02});
    auto* raw = reinterpret_cast<const std::byte*>(jpegData.constData());
    data.insert(data.end(), raw, raw + jpegData.size());
    m_dc->send(data);
}

void PeerConnectionManager::sendCameraFrame(const QByteArray& jpegData) {
    if (!m_dc || !m_dc->isOpen()) return;
    rtc::binary data;
    data.reserve(jpegData.size() + 1);
    data.push_back(std::byte{0x03});
    auto* raw = reinterpret_cast<const std::byte*>(jpegData.constData());
    data.insert(data.end(), raw, raw + jpegData.size());
    m_dc->send(data);
}
