#include "voice/VoiceEngine.h"
#include "voice/AudioEngine.h"
#include "voice/PeerCaps.h"
#include "voice/PeerConnectionManager.h"
#include "voice/video/VideoDecoder.h"
#include "voice/video/VideoEncoder.h"
#include "voice/video/VideoReceivePipeline.h"
#include "net/MatrixClient.h"

#include <bsfchat/Constants.h>

#include <QDateTime>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QDebug>

VoiceEngine::VoiceEngine(MatrixClient* client, QObject* parent)
    : IVoiceTransport(parent)
    , m_client(client)
{
    m_candidateBatchTimer.setInterval(500);
    m_candidateBatchTimer.setSingleShot(false);
    connect(&m_candidateBatchTimer, &QTimer::timeout, this, &VoiceEngine::flushCandidateBatch);

    m_rrTimer.setInterval(500);
    connect(&m_rrTimer, &QTimer::timeout, this, &VoiceEngine::sendReceiverReports);
}

VoiceEngine::~VoiceEngine() {
    stop();
}

// Voice diagnostics live under a single QLoggingCategory so they
// default off in release builds and can be opted into via
//   QT_LOGGING_RULES="bsfchat.voice.debug=true"
// or Settings → Advanced → "Verbose voice logging". Replaces the
// earlier `qInfo` calls that would spam every user's logcat.
Q_LOGGING_CATEGORY(logVoice, "bsfchat.voice",
                   QtWarningMsg)  // warnings on, info/debug off

bool VoiceEngine::start(const QString& roomId, const QJsonArray& members, const QJsonObject& turnConfig) {
    if (m_running) stop();

    qCInfo(logVoice, "start room=%s members=%lld turn=%s p2p=%s",
          qPrintable(roomId),
          static_cast<long long>(members.size()),
          turnConfig.contains("uris") ? "yes" : "no",
          turnConfig.value("allow_p2p").toBool(false) ? "yes" : "no");

    m_roomId = roomId;
    m_turnConfig = turnConfig;
    m_allowP2P = turnConfig.value("allow_p2p").toBool(false);

    // Relay-only with no relay to use is a guaranteed, silent, total
    // failure: iceTransportPolicy = Relay makes libdatachannel discard
    // host and srflx candidates, so with an empty (or STUN-only) server
    // list it gathers NOTHING, every peer sits in Connecting until the
    // 30 s watchdog reaps it, and the user is told nothing. Refuse the
    // join up front and say why. Checked against the built configuration
    // rather than the raw JSON so it reflects what libdatachannel would
    // actually receive (bad URI schemes are filtered out there).
    if (!m_allowP2P) {
        const auto probe = buildRtcConfig();
        bool hasRelay = false;
        for (const auto& server : probe.iceServers) {
            if (server.type == rtc::IceServer::Type::Turn) { hasRelay = true; break; }
        }
        if (!hasRelay) {
            qCWarning(logVoice, "refusing join: relay-only policy but no TURN "
                     "server in the config (%lld ICE servers)",
                     static_cast<long long>(probe.iceServers.size()));
            emit error(QStringLiteral(
                "Voice is unavailable: this server requires relayed connections "
                "but has no TURN server configured. Ask the server administrator "
                "to configure TURN, or to allow peer-to-peer voice."));
            m_roomId.clear();
            m_turnConfig = QJsonObject();
            return false;
        }
    }

    m_running = true;

    // Start audio engine
    m_audioEngine = new AudioEngine(this);
    connect(m_audioEngine, &AudioEngine::micLevelChanged,
            this, [this](float level) {
        m_micLevel = level;
        emit micLevelChanged(level);
    });
    connect(m_audioEngine, &AudioEngine::peerLevelChanged,
            this, &VoiceEngine::peerLevelChanged);
    if (!m_audioEngine->start()) {
        qCWarning(logVoice) << "AudioEngine::start failed";
        emit error("Failed to initialize audio");
    } else {
        qCInfo(logVoice, "AudioEngine started OK");
    }

    m_candidateBatchTimer.start();
    m_rrTimer.start();

    // Initiate connections to existing members.
    //
    // OFFER DIRECTION — one rule, used here AND by the 5 s mesh
    // reconciler in ServerConnection: we offer to `uid` only when
    // `uid > m_localUserId`; otherwise we wait for THEM to offer to us.
    // User ids are unique and totally ordered, so for any pair exactly
    // one side offers — no glare, ever, in any join ordering:
    //
    //   * joiner N, sitting member E, E > N  → N offers immediately at
    //     start(); E's reconciler never offers to N (N < E).
    //   * joiner N, sitting member E, E < N  → N stays quiet; E's next
    //     member poll sees N, N > E, and E offers (≤ 5 s).
    //   * simultaneous join of A and B → each evaluates the same
    //     comparison, so only the lesser id ends up offering.
    //
    // The previous code offered to EVERY member here while the
    // reconciler used the `uid > m_userId` rule, so half of all pairs
    // manufactured glare on the very first exchange. (It also offered to
    // ourselves whenever the member list included us — the comparison
    // rules that out for free.)
    int offered = 0;
    for (const auto& memberVal : members) {
        auto member = memberVal.toObject();
        QString userId = member.value("user_id").toString();
        if (userId.isEmpty() || userId == m_localUserId) continue;
        if (userId <= m_localUserId) {
            qCInfo(logVoice, "not offering to %s — lower id, they offer to us",
                  qPrintable(userId));
            continue;
        }
        qCInfo(logVoice, "offering to peer %s", qPrintable(userId));
        addPeer(userId, true); // We are the offerer (we just joined)
        ++offered;
    }
    if (members.isEmpty()) {
        qCInfo(logVoice, "no existing members — waiting for invites");
    } else if (offered == 0) {
        qCInfo(logVoice, "all %lld existing members outrank us — awaiting "
              "their invites", static_cast<long long>(members.size()));
    }
    return true;
}

void VoiceEngine::stop() {
    if (!m_running) return;
    m_running = false;

    m_candidateBatchTimer.stop();

    // Send hangup to all peers
    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
        nlohmann::json content = {
            {"call_id", m_callIds.value(it.key()).toStdString()},
            {"to", it.key().toStdString()},
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
    m_inboundCandidates.clear();
    qDeleteAll(m_disconnectTimers);
    m_disconnectTimers.clear();
    qDeleteAll(m_connectWatchdogs);
    m_connectWatchdogs.clear();
    // Decode pipelines block on their worker threads in their dtors,
    // so tear them down synchronously with the call.
    qDeleteAll(m_recvPipelines);
    m_recvPipelines.clear();
    m_videoSendActive = false;
    m_rrTimer.stop();
    m_rrSnapshots.clear();

    // Stop audio
    if (m_audioEngine) {
        m_audioEngine->stop();
        delete m_audioEngine;
        m_audioEngine = nullptr;
    }

    // Drop the mic indicator so listeners don't see a stale "transmitting"
    // state after the call ends.
    if (m_micLevel != 0.0f) {
        m_micLevel = 0.0f;
        emit micLevelChanged(0.0f);
    }
}

void VoiceEngine::addPeer(const QString& userId, bool isOfferer) {
    if (m_peers.contains(userId)) return;

    QString callId = generateCallId();
    m_callIds[userId] = callId;

    auto config = buildRtcConfig();
    auto* peer = new PeerConnectionManager(userId, callId, config, this);
    m_peers[userId] = peer;
    wirePeer(peer, userId);

    // Anything already parked for this exact call id (a rapid re-invite
    // whose candidates outran the peer object) belongs to this peer.
    replayInboundCandidates(userId, callId);

    if (isOfferer) {
        peer->createOffer();
    }
}

void VoiceEngine::ensurePeer(const QString& userId) {
    if (!m_running || userId.isEmpty()) return;
    if (m_peers.contains(userId)) return;
    qCInfo(logVoice, "reconcile: no peer for %s — offering",
          qPrintable(userId));
    addPeer(userId, true);
}

bool VoiceEngine::hasOpenPeers() const {
    for (auto* peer : m_peers) {
        if (peer && peer->isChannelOpen()) return true;
    }
    return false;
}

void VoiceEngine::wirePeer(PeerConnectionManager* peer, const QString& userId) {
    // Signaling
    connect(peer, &PeerConnectionManager::localDescriptionReady,
            this, [this, userId](const std::string& type, const std::string& sdp) {
                onLocalDescription(userId, type, sdp);
            });

    connect(peer, &PeerConnectionManager::localCandidateReady,
            this, [this, userId](const std::string& candidate, const std::string& mid) {
                onLocalCandidate(userId, candidate, mid);
            });

    // Audio
    if (m_audioEngine) {
        connect(peer, &PeerConnectionManager::audioFrameReceived,
                this, [this, userId](const QByteArray& frame) {
                    if (m_audioEngine) m_audioEngine->receivePeerAudio(userId, frame);
                });

        connect(m_audioEngine, &AudioEngine::audioFrameReady,
                peer, &PeerConnectionManager::sendAudioFrame);
    }
    // Control channel (caps refresh, keyframe requests, receiver
    // reports) — new-client peers only; legacy peers never send 0x04.
    connect(peer, &PeerConnectionManager::controlMessageReceived,
            this, [this, userId](const QByteArray& json) {
                onControlMessage(userId, json);
            });

    // RTP video receive: reassembled access units → per-peer decoder.
    connect(peer, &PeerConnectionManager::videoFrameReceived,
            this, [this, userId](int streamId, const QByteArray& au, bool loss) {
                recvPipeline(userId, streamId, VideoCodecKind::H264)
                    ->submitAccessUnit(au, /*keyframeHint=*/false,
                                       /*lossSuspected=*/loss);
            });
    // Lossless tier: AV1 temporal units off the reliable channel.
    connect(peer, &PeerConnectionManager::losslessFrameReceived,
            this, [this, userId](int streamId, const QByteArray& tu, bool kf) {
                recvPipeline(userId, streamId, VideoCodecKind::Av1Lossless)
                    ->submitAccessUnit(tu, kf);
            });
    // Peer PLI'd our send stream, or its track just opened — either
    // way the next frame we send must be an IDR (a late joiner can't
    // decode anything until one arrives).
    connect(peer, &PeerConnectionManager::keyframeRequestedByPeer,
            this, &VoiceEngine::videoKeyframeRequested);
    connect(peer, &PeerConnectionManager::videoTrackOpen,
            this, &VoiceEngine::videoKeyframeRequested);

    // Route screen-share frames up to the UI.
    connect(peer, &PeerConnectionManager::screenFrameReceived,
            this, [this, userId](const QByteArray& jpeg) {
                emit peerScreenFrameReceived(userId, jpeg);
            });
    connect(peer, &PeerConnectionManager::cameraFrameReceived,
            this, [this, userId](const QByteArray& jpeg) {
                emit peerCameraFrameReceived(userId, jpeg);
            });

    connect(peer, &PeerConnectionManager::connected,
            this, [this, userId]() { emit peerConnected(userId); });

    connect(peer, &PeerConnectionManager::losslessSendStalled,
            this, &VoiceEngine::losslessSendUnavailable);

    connect(peer, &PeerConnectionManager::peerStateChanged,
            this, [this, userId](PeerConnectionManager::PeerState s) {
        static const char* names[] = {"new","connecting","connected","disconnected","failed"};
        emit peerStateChanged(userId, QString::fromLatin1(names[int(s)]));
        // Dead-peer cleanup. Failed covers Closed too (PeerConnectionManager
        // maps both to PeerState::Failed) — tear down immediately so the
        // mesh reconciler can re-offer. Disconnected is often a transient
        // ICE blip, so give it a grace period before giving up.
        if (s == PeerConnectionManager::PeerState::Failed) {
            removePeer(userId);
            emit peerDisconnected(userId);
        } else if (s == PeerConnectionManager::PeerState::Disconnected) {
            startDisconnectGrace(userId);
        } else if (s == PeerConnectionManager::PeerState::Connected) {
            cancelDisconnectGrace(userId);
            cancelConnectWatchdog(userId);
        }
    });

    startConnectWatchdog(userId);
}

void VoiceEngine::startDisconnectGrace(const QString& userId) {
    if (m_disconnectTimers.contains(userId)) return;
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(10000);
    connect(timer, &QTimer::timeout, this, [this, userId]() {
        cancelDisconnectGrace(userId);
        auto* peer = m_peers.value(userId);
        if (!peer) return;
        if (peer->peerState() == PeerConnectionManager::PeerState::Connected) return;
        qCInfo(logVoice, "peer %s did not recover from disconnect — removing",
              qPrintable(userId));
        removePeer(userId);
        emit peerDisconnected(userId);
    });
    m_disconnectTimers[userId] = timer;
    timer->start();
}

void VoiceEngine::cancelDisconnectGrace(const QString& userId) {
    if (auto* timer = m_disconnectTimers.take(userId)) {
        timer->stop();
        timer->deleteLater();
    }
}

void VoiceEngine::startConnectWatchdog(const QString& userId) {
    cancelConnectWatchdog(userId);
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    // Generous: covers TURN allocation plus an answer arriving via
    // long-poll sync. Anything slower is effectively dead, and the
    // mesh reconciler re-offers after teardown if the member is still
    // listed.
    timer->setInterval(30000);
    connect(timer, &QTimer::timeout, this, [this, userId]() {
        cancelConnectWatchdog(userId);
        auto* peer = m_peers.value(userId);
        if (!peer) return;
        if (peer->peerState() == PeerConnectionManager::PeerState::Connected) return;
        qCWarning(logVoice, "peer %s never reached connected — tearing down",
                 qPrintable(userId));
        removePeer(userId);
        emit peerDisconnected(userId);
    });
    m_connectWatchdogs[userId] = timer;
    timer->start();
}

void VoiceEngine::cancelConnectWatchdog(const QString& userId) {
    if (auto* timer = m_connectWatchdogs.take(userId)) {
        timer->stop();
        timer->deleteLater();
    }
}

QVariantMap VoiceEngine::videoReceiveStats(const QString& userId,
                                           int streamId) const {
    auto* pipeline = m_recvPipelines.value({userId, streamId});
    if (!pipeline) return {};
    QVariantMap out;
    out["rxFrames"] = quint64(pipeline->rxFrames());
    out["rxBytes"] = quint64(pipeline->rxBytes());
    out["decodedFrames"] = quint64(pipeline->decodedFrames());
    out["droppedAus"] = quint64(pipeline->droppedAus());
    out["width"] = pipeline->frameWidth();
    out["height"] = pipeline->frameHeight();
    out["codec"] = pipeline->codec() == VideoCodecKind::Av1Lossless
        ? QStringLiteral("av1-lossless") : QStringLiteral("h264");
    return out;
}

QMap<QString, QString> VoiceEngine::peerStates() const {
    QMap<QString, QString> out;
    static const char* names[] = {"new","connecting","connected","disconnected","failed"};
    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
        out[it.key()] = QString::fromLatin1(names[int(it.value()->peerState())]);
    }
    return out;
}

void VoiceEngine::removePeer(const QString& userId) {
    cancelDisconnectGrace(userId);
    cancelConnectWatchdog(userId);
    if (auto* peer = m_peers.take(userId)) {
        if (m_audioEngine) m_audioEngine->removePeer(userId);
        peer->deleteLater();
    }
    // Only the buffer for the call we're actually dropping. The replace
    // paths (glare loss / dead peer / new call id) call removePeer and
    // then immediately build a peer for a DIFFERENT call id, whose
    // candidates may already be parked — those must survive.
    const QString goneCallId = m_callIds.value(userId);
    if (!goneCallId.isEmpty()) dropInboundCandidates(userId, goneCallId);
    m_callIds.remove(userId);
    m_pendingCandidates.remove(userId);
    dropRecvPipelines(userId);
    for (int s = 0; s < kVideoStreamCount; ++s)
        m_rrSnapshots.remove({userId, s});
}

nlohmann::json VoiceEngine::localCapsJson() {
    PeerCaps caps;
    caps.videoRtp = true;
    // Probe the codec backends compiled into this build. A platform
    // with no encoder still advertises its decode side so it can
    // receive video it can't send.
    caps.h264ProfilesEncode = VideoEncoder::h264EncodeProfiles();
    caps.h264ProfilesDecode = VideoDecoder::h264DecodeProfiles();
    if (!caps.h264ProfilesEncode.isEmpty() || !caps.h264ProfilesDecode.isEmpty())
        caps.videoCodecs << QStringLiteral("h264");
    if (VideoEncoder::queryCaps(VideoCodecKind::Av1Lossless).losslessSupported)
        caps.lossless << QStringLiteral("av1-dc");
    return caps.toJson();
}

void VoiceEngine::onControlMessage(const QString& userId, const QByteArray& json) {
    const auto doc = nlohmann::json::parse(json.constData(),
                                           json.constData() + json.size(),
                                           nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) {
        qCWarning(logVoice, "malformed control message from %s (%d bytes)",
                 qPrintable(userId), int(json.size()));
        return;
    }
    const std::string t = doc.value("t", "");
    if (t == "caps") {
        // Mid-call capability refresh.
        if (auto* peer = m_peers.value(userId)) {
            peer->setRemoteCaps(PeerCaps::fromJson(doc.value("caps", nlohmann::json::object())));
            maybeSetupVideoFor(userId);
        }
    } else if (t == "kf") {
        // App-level keyframe request — the guaranteed cross-version
        // recovery path (RTCP PLI also feeds videoKeyframeRequested).
        const int stream = doc.value("stream", 0);
        if (stream >= 0 && stream < kVideoStreamCount)
            emit videoKeyframeRequested(stream);
    } else if (t == "rr") {
        // Receiver report for OUR outgoing stream toward `userId`:
        // cumulative received bytes. Windowed against our cumulative
        // sent bytes since the previous report → delivery ratio.
        const int stream = doc.value("stream", 0);
        if (stream < 0 || stream >= kVideoStreamCount) return;
        auto* peer = m_peers.value(userId);
        if (!peer) return;
        const quint64 rxBytes = doc.value("b", quint64(0));
        const quint64 txBytes = peer->videoTxBytes(VideoStreamId(stream));
        auto& snap = m_rrSnapshots[{userId, stream}];
        const quint64 dRx = rxBytes - qMin(rxBytes, snap.rxBytes);
        const quint64 dTx = txBytes - qMin(txBytes, snap.txBytes);
        snap.rxBytes = rxBytes;
        snap.txBytes = txBytes;
        if (dTx == 0) return;   // idle window — nothing to grade
        const double ratio = qMin(1.0, double(dRx) / double(dTx));
        emit videoDeliveryRatio(userId, stream, ratio);
    } else {
        qCDebug(logVoice, "unhandled control '%s' from %s",
               t.c_str(), qPrintable(userId));
    }
}

void VoiceEngine::sendReceiverReports() {
    for (auto it = m_recvPipelines.constBegin();
         it != m_recvPipelines.constEnd(); ++it) {
        auto* peer = m_peers.value(it.key().first);
        if (!peer) continue;
        nlohmann::json rr = {
            {"t", "rr"},
            {"stream", it.key().second},
            {"f", it.value()->rxFrames()},
            {"b", it.value()->rxBytes()},
        };
        peer->sendControl(QByteArray::fromStdString(rr.dump()));
    }
}

void VoiceEngine::maybeSetupVideoFor(const QString& userId) {
    if (!m_videoSendActive) return;
    auto* peer = m_peers.value(userId);
    if (peer && peer->remoteSupportsVideoRtp()) peer->ensureVideoTracks();
}

void VoiceEngine::prepareVideoSend() {
    if (!m_running) return;
    m_videoSendActive = true;
    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
        if (it.value()->remoteSupportsVideoRtp())
            it.value()->ensureVideoTracks();
    }
}

void VoiceEngine::broadcastEncodedVideo(VideoStreamId stream, const EncodedFrame& frame) {
    if (!m_running) return;
    for (auto* peer : m_peers) {
        if (peer) peer->sendVideoFrame(stream, frame);
    }
}

void VoiceEngine::broadcastLosslessVideo(VideoStreamId stream, const EncodedFrame& frame) {
    if (!m_running) return;
    static const QString kAv1Dc = QStringLiteral("av1-dc");
    for (auto* peer : m_peers) {
        if (peer && peer->remoteCaps().lossless.contains(kAv1Dc))
            peer->sendLosslessFrame(stream, frame);
    }
}

bool VoiceEngine::allPeersSupportLossless() const {
    static const QString kAv1Dc = QStringLiteral("av1-dc");
    bool any = false;
    for (auto* peer : m_peers) {
        if (!peer || !peer->remoteSupportsVideoRtp()) continue;
        if (peer->remoteCaps().videoCodecs.isEmpty()) continue;
        if (!peer->remoteCaps().lossless.contains(kAv1Dc)) return false;
        any = true;
    }
    return any;
}

bool VoiceEngine::losslessBackpressure(qint64 budgetBytes) const {
    for (auto* peer : m_peers) {
        if (peer && peer->losslessBufferedAmount() > budgetBytes) return true;
    }
    return false;
}

bool VoiceEngine::hasLegacyOpenPeers(VideoStreamId stream) const {
    // "Needs the JPEG path": open data channel but no open video
    // track for this stream — covers legacy clients AND capable peers
    // still mid-renegotiation (the broadcast transition-gap rule).
    for (auto* peer : m_peers) {
        if (peer && peer->isChannelOpen()
            && !peer->hasVideoTrackOpen(stream))
            return true;
    }
    return false;
}

bool VoiceEngine::hasVideoCapablePeers() const {
    for (auto* peer : m_peers) {
        if (peer && peer->remoteSupportsVideoRtp()
            && !peer->remoteCaps().videoCodecs.isEmpty())
            return true;
    }
    return false;
}

H264Profile VoiceEngine::negotiatedH264Profile() const {
    static const QString kHigh = QStringLiteral("high");
    if (!VideoEncoder::h264EncodeProfiles().contains(kHigh))
        return H264Profile::ConstrainedBaseline;
    for (auto* peer : m_peers) {
        if (!peer || !peer->remoteSupportsVideoRtp()) continue;
        if (peer->remoteCaps().videoCodecs.isEmpty()) continue;
        if (!peer->remoteCaps().h264ProfilesDecode.contains(kHigh))
            return H264Profile::ConstrainedBaseline;
    }
    return H264Profile::High;
}

VideoReceivePipeline* VoiceEngine::recvPipeline(const QString& userId, int streamId,
                                                VideoCodecKind codec) {
    const QPair<QString, int> key{userId, streamId};
    if (auto* existing = m_recvPipelines.value(key)) {
        if (existing->codec() == codec) return existing;
        // Sender switched codec mid-call (lossless toggle) — rebuild.
        m_recvPipelines.remove(key);
        existing->deleteLater();
    }
    auto* pipeline = new VideoReceivePipeline(userId, VideoStreamId(streamId),
                                              codec, this);
    m_recvPipelines[key] = pipeline;
    connect(pipeline, &VideoReceivePipeline::frameDecoded,
            this, &VoiceEngine::peerVideoFrameDecoded);
    // Decode hiccup (loss/backlog) — ask THAT sender for an IDR.
    connect(pipeline, &VideoReceivePipeline::keyframeNeeded,
            this, [this](const QString& uid, int stream) {
        if (auto* peer = m_peers.value(uid))
            peer->requestPeerKeyframe(VideoStreamId(stream));
    });
    return pipeline;
}

void VoiceEngine::dropRecvPipelines(const QString& userId) {
    for (int s = 0; s < kVideoStreamCount; ++s) {
        if (auto* p = m_recvPipelines.take({userId, s})) p->deleteLater();
    }
}

void VoiceEngine::handleCallInvite(const QString& sender, const QString& callId,
                                   const std::string& sdp, const nlohmann::json& caps) {
    if (!m_running) {
        qCInfo(logVoice, "ignore invite from %s — engine not running",
              qPrintable(sender));
        return;
    }
    qCInfo(logVoice, "recv invite from %s callId=%s sdp_bytes=%zu",
          qPrintable(sender), qPrintable(callId), sdp.size());

    if (auto* existing = m_peers.value(sender)) {
        const auto state = existing->peerState();
        if (state == PeerConnectionManager::PeerState::Failed
            || state == PeerConnectionManager::PeerState::Disconnected) {
            // Dead connection — the remote is retrying. Replace it.
            qCInfo(logVoice, "invite from %s replaces dead peer",
                  qPrintable(sender));
            removePeer(sender);
        } else if (existing->isOfferer()
                   && state != PeerConnectionManager::PeerState::Connected) {
            // Glare: both sides offered simultaneously. Deterministic
            // tie-break — the lexicographically LESSER user id's offer
            // wins; both sides apply the same rule so exactly one
            // connection survives.
            if (sender < m_localUserId) {
                qCInfo(logVoice, "glare with %s — remote wins, answering",
                      qPrintable(sender));
                removePeer(sender);
            } else {
                qCInfo(logVoice, "glare with %s — we win, ignoring invite",
                      qPrintable(sender));
                return;
            }
        } else if (callId != m_callIds.value(sender)) {
            // New call_id from a peer we already hold — they restarted
            // their session; our existing connection is stale.
            qCInfo(logVoice, "invite from %s carries new callId — replacing peer",
                  qPrintable(sender));
            removePeer(sender);
        } else {
            qCInfo(logVoice, "ignore duplicate invite from %s",
                  qPrintable(sender));
            return;
        }
    }

    m_callIds[sender] = callId;

    auto config = buildRtcConfig();
    auto* peer = new PeerConnectionManager(sender, callId, config, this);
    m_peers[sender] = peer;
    wirePeer(peer, sender);
    peer->setRemoteCaps(PeerCaps::fromJson(caps));

    peer->applyOffer(sdp);
    // Their candidates are sent ONCE and commonly beat (or race) this
    // invite through the timeline — replay whatever we parked for this
    // call id now that a peer exists to take them.
    replayInboundCandidates(sender, callId);
    // If we're mid-share, upgrade this newcomer to video right after
    // the initial exchange (ensureVideoTracks queues the renegotiation
    // until signaling is stable again).
    maybeSetupVideoFor(sender);
}

void VoiceEngine::handleCallAnswer(const QString& sender, const QString& callId,
                                   const std::string& sdp, const nlohmann::json& caps) {
    qCInfo(logVoice, "recv answer from %s callId=%s sdp_bytes=%zu",
          qPrintable(sender), qPrintable(callId), sdp.size());
    if (callId != m_callIds.value(sender)) {
        qCInfo(logVoice, "ignore answer from %s — callId mismatch",
              qPrintable(sender));
        return;
    }
    if (auto* peer = m_peers.value(sender)) {
        peer->setRemoteCaps(PeerCaps::fromJson(caps));
        peer->applyAnswer(sdp);
        // The remote description is set now, so anything parked for
        // this call can finally be handed to libdatachannel.
        replayInboundCandidates(sender, callId);
        maybeSetupVideoFor(sender);
    } else {
        qCWarning(logVoice, "answer from unknown peer %s",
                 qPrintable(sender));
    }
}

void VoiceEngine::handleCallNegotiate(const QString& sender, const QString& callId,
                                      const std::string& type, const std::string& sdp) {
    if (callId != m_callIds.value(sender)) {
        qCInfo(logVoice, "ignore negotiate from %s — callId mismatch",
              qPrintable(sender));
        return;
    }
    auto* peer = m_peers.value(sender);
    if (!peer) {
        qCWarning(logVoice, "negotiate from unknown peer %s", qPrintable(sender));
        return;
    }
    if (!peer->initialNegotiationDone()) {
        // A negotiate event can't legitimately precede the initial
        // offer/answer round-trip — likely a stale timeline replay.
        qCInfo(logVoice, "ignore premature negotiate from %s", qPrintable(sender));
        return;
    }

    if (type == "offer") {
        if (peer->hasPendingLocalReoffer()) {
            // Renegotiation glare: both sides re-offered at once. Same
            // deterministic tie-break as the invite path — the
            // lexicographically LESSER id is impolite and its offer
            // wins; the polite side rolls back and answers.
            if (sender < m_localUserId) {
                qCInfo(logVoice, "negotiate glare with %s — remote wins, rolling back",
                      qPrintable(sender));
                peer->rollbackLocalReoffer();
            } else {
                qCInfo(logVoice, "negotiate glare with %s — we win, ignoring offer",
                      qPrintable(sender));
                return;
            }
        }
        peer->applyNegotiateOffer(sdp);
    } else if (type == "answer") {
        peer->applyNegotiateAnswer(sdp);
    } else {
        qCWarning(logVoice, "negotiate from %s with unknown type '%s'",
                 qPrintable(sender), type.c_str());
    }
}

void VoiceEngine::handleCallCandidates(const QString& sender, const QString& callId,
                                        const std::vector<std::pair<std::string, std::string>>& candidates) {
    if (candidates.empty()) return;
    auto* peer = m_peers.value(sender);
    if (peer && callId == m_callIds.value(sender)) {
        for (const auto& [cand, mid] : candidates) {
            peer->addRemoteCandidate(cand, mid);
        }
        return;
    }
    // No peer yet, or the peer we hold belongs to an older call id and
    // the matching invite is still in flight. Candidates are sent
    // exactly once, so dropping them here (the old behaviour) meant
    // permanently losing the remote's transport addresses — ICE then
    // never completed and the connect watchdog reaped the peer 30 s
    // later. Park them instead.
    bufferInboundCandidates(sender, callId, candidates);
}

void VoiceEngine::bufferInboundCandidates(
    const QString& sender, const QString& callId,
    const std::vector<std::pair<std::string, std::string>>& candidates) {
    if (sender.isEmpty() || callId.isEmpty()) return;
    pruneInboundCandidates();
    const QPair<QString, QString> key{sender, callId};
    if (!m_inboundCandidates.contains(key)
        && m_inboundCandidates.size() >= kMaxInboundCandidateCalls) {
        qCWarning(logVoice, "inbound candidate buffer full (%d calls) — "
                 "dropping candidates for %s/%s",
                 int(m_inboundCandidates.size()), qPrintable(sender),
                 qPrintable(callId));
        return;
    }
    auto& bucket = m_inboundCandidates[key];
    if (bucket.firstSeenMs == 0)
        bucket.firstSeenMs = QDateTime::currentMSecsSinceEpoch();
    for (const auto& c : candidates) {
        if (int(bucket.items.size()) >= kMaxInboundCandidatesPerCall) break;
        bucket.items.push_back(c);
    }
    qCInfo(logVoice, "parked %d ICE candidate(s) from %s for call %s "
          "(%d held)", int(candidates.size()), qPrintable(sender),
          qPrintable(callId), int(bucket.items.size()));
}

void VoiceEngine::replayInboundCandidates(const QString& sender,
                                          const QString& callId) {
    // Check the peer BEFORE taking: a mismatched call id must leave the
    // bucket parked for whichever peer eventually claims it.
    auto* peer = m_peers.value(sender);
    if (!peer || peer->callId() != callId) return;
    const auto bucket = m_inboundCandidates.take({sender, callId});
    if (bucket.items.empty()) return;
    qCInfo(logVoice, "replaying %d parked ICE candidate(s) for %s/%s",
          int(bucket.items.size()), qPrintable(sender), qPrintable(callId));
    for (const auto& [cand, mid] : bucket.items) {
        peer->addRemoteCandidate(cand, mid);
    }
}

void VoiceEngine::dropInboundCandidates(const QString& sender,
                                        const QString& callId) {
    m_inboundCandidates.remove({sender, callId});
}

void VoiceEngine::pruneInboundCandidates() {
    if (m_inboundCandidates.isEmpty()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_inboundCandidates.begin(); it != m_inboundCandidates.end(); ) {
        if (now - it.value().firstSeenMs > kInboundCandidateTtlMs) {
            qCInfo(logVoice, "expiring parked ICE candidates for %s/%s",
                  qPrintable(it.key().first), qPrintable(it.key().second));
            it = m_inboundCandidates.erase(it);
        } else {
            ++it;
        }
    }
}

void VoiceEngine::handleCallHangup(const QString& sender, const QString& callId) {
    // Only honour a hangup for the call we actually hold — a late
    // hangup from a session the peer already replaced must not kill
    // the live connection. Empty stored id = no peer = no-op.
    const QString stored = m_callIds.value(sender);
    if (stored.isEmpty() || callId != stored) {
        // Still drop anything parked for the call being hung up: that
        // peer is never going to materialise.
        dropInboundCandidates(sender, callId);
        return;
    }
    removePeer(sender);   // drops this call's parked candidates too
    emit peerDisconnected(sender);
}

void VoiceEngine::setMuted(bool muted) {
    if (m_audioEngine) m_audioEngine->setMuted(muted);
}

void VoiceEngine::setDeafened(bool deafened) {
    if (m_audioEngine) m_audioEngine->setDeafened(deafened);
}

void VoiceEngine::onLocalDescription(const QString& peerId, const std::string& type, const std::string& sdp) {
    auto callId = m_callIds.value(peerId);

    // After the initial offer/answer round-trip, descriptions are
    // renegotiations (adding video m-lines etc.) and take the
    // bsfchat.call.negotiate path — legacy clients never receive them
    // because renegotiation is only ever triggered toward peers whose
    // caps advertise video_rtp.
    // "to" is the RECIPIENT of this event. Call signaling rides the
    // shared room timeline, so in a mesh of 3+ everyone sees everyone
    // else's invites/answers/candidates. Without an addressee the
    // receiver could only dispatch on `sender`, so C would apply an
    // offer A meant for B — and because addPeer() mints a distinct call
    // id per peer, the call-id mismatch branch in handleCallInvite read
    // that as "peer restarted" and tore down a working connection. That
    // is why voice worked 1:1 and collapsed at 3+.
    //
    // Receivers that don't understand "to" (older clients) still fall
    // back to sender-only dispatch, so this is additive on the wire.
    const std::string to = peerId.toStdString();

    auto* peer = m_peers.value(peerId);
    if (peer && peer->initialNegotiationDone()) {
        nlohmann::json content = {
            {"call_id", callId.toStdString()},
            {"to", to},
            {"description", {{"type", type}, {"sdp", sdp}}},
            {"version", 1}
        };
        sendCallEvent(QString::fromUtf8(bsfchat::event_type::kCallNegotiate), content);
        return;
    }

    if (type == "offer") {
        nlohmann::json content = {
            {"call_id", callId.toStdString()},
            {"to", to},
            {"lifetime", 60000},
            {"offer", {{"type", "offer"}, {"sdp", sdp}}},
            {"bsfchat_caps", localCapsJson()},
            {"version", 1}
        };
        sendCallEvent(QString::fromUtf8(bsfchat::event_type::kCallInvite), content);
    } else if (type == "answer") {
        nlohmann::json content = {
            {"call_id", callId.toStdString()},
            {"to", to},
            {"answer", {{"type", "answer"}, {"sdp", sdp}}},
            {"bsfchat_caps", localCapsJson()},
            {"version", 1}
        };
        sendCallEvent(QString::fromUtf8(bsfchat::event_type::kCallAnswer), content);
    }
}

void VoiceEngine::onLocalCandidate(const QString& peerId, const std::string& candidate, const std::string& mid) {
    m_pendingCandidates[peerId].emplace_back(candidate, mid);
}

void VoiceEngine::flushCandidateBatch() {
    // Cheap piggy-back: this ticks every 500 ms for the whole call, so
    // it is the natural place to age out parked inbound candidates.
    pruneInboundCandidates();

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
            {"to", it.key().toStdString()},   // see onLocalDescription
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

    const auto user = m_turnConfig.value("username").toString();
    const auto pass = m_turnConfig.value("password").toString();

    // The server may return several turn: URIs (udp + tcp transport
    // variants) plus stun: URIs — feed them all to libdatachannel.
    // Credentials are assigned via the IceServer struct fields, NOT
    // embedded in the URL: the ephemeral TURN passwords are base64
    // HMAC values that can contain '+'/'/' and would corrupt a
    // turn:user:pass@host string.
    for (const auto& uriVal : m_turnConfig.value("uris").toArray()) {
        QString u = uriVal.toString();
        if (u.startsWith("turn:") || u.startsWith("turns:")) {
            rtc::IceServer turn(u.toStdString());
            turn.username = user.toStdString();
            turn.password = pass.toStdString();
            config.iceServers.push_back(std::move(turn));
        } else if (u.startsWith("stun:")) {
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

void VoiceEngine::broadcastScreenFrame(const QByteArray& jpegData) {
    if (!m_running) return;
    static int s_bc = 0;
    if (++s_bc % 25 == 1) {
        qCDebug(logVoice, "broadcastScreenFrame: %d peers, %d bytes",
              int(m_peers.size()), int(jpegData.size()));
    }
    for (auto* peer : m_peers) {
        if (!peer) continue;
        // Peers with an open video track get real video instead; the
        // JPEG keeps flowing to legacy peers AND capable peers whose
        // track is still renegotiating (long-poll signaling can take
        // seconds), so nobody stares at a placeholder in between.
        if (peer->hasVideoTrackOpen(VideoStreamId::Screen)) continue;
        peer->sendScreenFrame(jpegData);
    }
}

void VoiceEngine::broadcastCameraFrame(const QByteArray& jpegData) {
    if (!m_running) return;
    static int s_cc = 0;
    if (++s_cc % 25 == 1) {
        qCDebug(logVoice, "broadcastCameraFrame: %d peers, %d bytes",
              int(m_peers.size()), int(jpegData.size()));
    }
    for (auto* peer : m_peers) {
        if (!peer) continue;
        // Same transition-gap rule as the screen stream: JPEG flows
        // until the peer's camera track opens, then RTP takes over.
        if (peer->hasVideoTrackOpen(VideoStreamId::Camera)) continue;
        peer->sendCameraFrame(jpegData);
    }
}
