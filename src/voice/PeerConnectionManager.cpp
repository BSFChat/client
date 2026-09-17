#include "voice/PeerConnectionManager.h"

#include "voice/video/RtpSeqTracker.h"
#include <QDateTime>
#include <mutex>
#include <QDebug>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <chrono>
#include <queue>

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
// mids/PTs agree by construction.
//
// SSRCs ARE PART OF THAT IDENTITY, and they are FIXED rather than
// random for one blunt reason: libdatachannel routes every incoming RTP
// packet by SSRC. impl::PeerConnection::dispatchMedia() has a shortcut
// for a connection with exactly ONE m-line ("there is only one track,
// give it everything"); with two or more it looks the packet's SSRC up
// in a map built solely from the `a=ssrc:` attributes of the negotiated
// descriptions, and drops the packet when the lookup misses. We
// negotiate TWO video m-lines and used to declare no SSRCs at all, so
// that map was empty and every video packet either side ever sent was
// discarded inside the receiver before any track, handler or onFrame
// saw it — a share that sends perfectly and is never seen (rc.15).
//
// Each m-line declares BOTH endpoints' SSRCs: the side that creates the
// m-line sends on `offererSsrc`, the side that adopts it via onTrack
// sends on `answererSsrc`.
//
// Declaring BOTH on the offering side is not belt-and-braces, it is the
// only thing that makes the reverse direction work. The answerer cannot
// contribute an SSRC of its own to the negotiation: its answer is built
// by reciprocating the offer's media, and Description::Media::
// reciprocate() ends with clearSSRCs() — so the answer carries no
// `a=ssrc:` line whatever the answerer does, and our onTrack handler is
// a queued invocation that could not get one in there in time anyway.
// The demux map is fed from the LOCAL description as well as the remote
// one, so one offer carrying both SSRCs is enough for both endpoints:
// the offerer learns `answererSsrc` from its own offer, the answerer
// learns `offererSsrc` from the same offer as its remote description.
//
// Consequence worth knowing: against a peer running a build from before
// this change, only the direction INTO that peer works — its own RTP
// still goes out under a random, undeclared SSRC that we cannot route.
//
// Values are arbitrary but must differ per (m-line, role); SSRCs only
// have to be unique within one peer connection, so the same four
// constants are safe across every call.
struct VideoStreamSpec {
    const char* mid;
    uint8_t payloadType;
    const char* cname;
    uint32_t offererSsrc;    // used by the side that addTrack()s this m-line
    uint32_t answererSsrc;   // used by the side that adopts it via onTrack
};
constexpr VideoStreamSpec kVideoSpecs[kVideoStreamCount] = {
    {"vscreen", 96, "bsf-screen", 0xB5F5C001u, 0xB5F5C002u},
    {"vcamera", 97, "bsf-camera", 0xB5FCA001u, 0xB5FCA002u},
};

// The `a=ssrc:` lines that make the receiver's demultiplexer able to
// find this m-line's track. Must be applied to the media description
// BEFORE it is handed to addTrack(), because that description is what
// goes into the SDP.
void declareVideoSsrcs(rtc::Description::Video& media, const VideoStreamSpec& spec)
{
    media.addSSRC(spec.offererSsrc, spec.cname);
    media.addSSRC(spec.answererSsrc, spec.cname);
}

int streamIndexForMid(const std::string& mid) {
    for (int i = 0; i < kVideoStreamCount; ++i)
        if (mid == kVideoSpecs[i].mid) return i;
    return -1;
}

// Watches incoming RTP sequence numbers and reports gaps. The H264
// depacketizer knowingly assembles incomplete access units when
// packets are missing (per RFC 6184 it only drops the *rest of the
// fragmented NAL*, not the frame) and provides no loss signal — and
// hardware decoders (notably Media Foundation's) error-conceal such
// input and return success, so without this hint the receive pipeline
// would happily display progressively-corrupting frames until the
// next periodic IDR. Sits between the RTCP session and the
// depacketizer in the receive chain (incoming traverses tail→head),
// so it sees clean RTP packets post-RTCP-strip, pre-reassembly.
class RtpGapDetector final : public rtc::MediaHandler {
public:
    explicit RtpGapDetector(std::function<void()> onGap)
        : m_onGap(std::move(onGap)) {}

    void incoming(rtc::message_vector& messages,
                  const rtc::message_callback&) override {
        const int64_t now = QDateTime::currentMSecsSinceEpoch();
        for (const auto& msg : messages) {
            if (!msg || msg->size() < sizeof(rtc::RtpHeader)) continue;
            const auto* h = reinterpret_cast<const rtc::RtpHeader*>(msg->data());
            if (h->version() != 2) continue;
            // RFC 5761 demux: byte 1 of an RTCP packet is its full
            // 8-bit packet type (SR=200 … PSFB=206); in RTP the same
            // byte is marker|PT and our PTs (96/97) land outside
            // [192,223] with or without the marker bit. Don't let a
            // stray RTCP packet masquerade as a sequence jump.
            const uint8_t pt = std::to_integer<uint8_t>(msg->at(1));
            if (pt >= 192 && pt <= 223) continue;
            // S-4: the gap verdict is held for a short reorder window
            // (see RtpSeqTracker) instead of firing on the first
            // out-of-order packet — single-packet reordering used to
            // cost a dropped access unit and an IDR request every time.
            if (m_tracker.observe(h->seqNumber(), now) && m_onGap) m_onGap();
        }
        // A gap at the tail of this batch would otherwise wait for the
        // next one; resolve it on elapsed time here.
        if (m_tracker.poll(now) && m_onGap) m_onGap();
    }

private:
    std::function<void()> m_onGap;
    RtpSeqTracker m_tracker;
};

// Token-bucket RTP pacer with a BOUNDED backlog (S-15).
//
// Replaces rtc::PacingHandler, whose budget is fixed at construction.
// That fixed 20 Mbps sat BELOW what the encoder is allowed to produce
// (the screen-share envelope reaches hundreds of Mbps on a 4K/60
// setting), and its queue has no bound: every byte the encoder emits
// above the budget was buffered rather than dropped, so on a
// misconfigured-high bitrate the send queue — and with it the viewer's
// latency — grew without limit while the picture stayed "smooth".
//
// Two changes fix that:
//   * the ceiling is live-settable and is driven from the encoder's own
//     max bitrate (VoiceEngine::setVideoSendCeiling), so pacing only
//     ever shaves bursts, never throttles the steady state;
//   * the backlog is capped at kMaxBacklogSeconds of budget and
//     overflow drops the OLDEST queued packets. Old RTP is worthless:
//     the receiver has already moved on, and the loss makes it ask for
//     a keyframe (which now actually re-fires, see S-2) instead of
//     replaying stale pictures late.
//
// Leftovers ride out on the next outgoing() call rather than a timer —
// libdatachannel's scheduler lives in a private header. With the
// ceiling tracking the encoder that path is reached only by an IDR
// burst, and the next access unit is one frame interval away.
class PacedRtpSender final : public rtc::MediaHandler {
public:
    explicit PacedRtpSender(int ceilingKbps) { setCeilingKbps(ceilingKbps); }

    // Thread-safe; called from the Qt main thread while outgoing()
    // runs on libdatachannel's network thread.
    void setCeilingKbps(int kbps) {
        m_bytesPerSecond.store(double(std::max(kbps, kMinCeilingKbps))
                                   * 1000.0 / 8.0,
                               std::memory_order_relaxed);
    }

    quint64 droppedPackets() const {
        return m_dropped.load(std::memory_order_relaxed);
    }

    void outgoing(rtc::message_vector& messages,
                  const rtc::message_callback& send) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        const double rate = m_bytesPerSecond.load(std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        if (m_started) {
            const double elapsed =
                std::chrono::duration<double>(now - m_lastRun).count();
            m_budget = std::min(m_budget + elapsed * rate,
                                rate * kMaxBurstSeconds);
        } else {
            // First burst starts with a full bucket: a share's opening
            // IDR must not be held back behind an empty budget.
            m_budget = rate * kMaxBurstSeconds;
            m_started = true;
        }
        m_lastRun = now;

        for (auto& m : messages) {
            if (!m) continue;
            m_backlogBytes += m->size();
            m_queue.push(std::move(m));
        }
        messages.clear();

        const size_t maxBacklog =
            size_t(std::max(rate * kMaxBacklogSeconds, kMinBacklogBytes));
        quint64 droppedNow = 0;
        while (m_backlogBytes > maxBacklog && !m_queue.empty()) {
            m_backlogBytes -= m_queue.front()->size();
            m_queue.pop();
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            ++droppedNow;
        }
        // Never silent: a pacer that discards media is the difference
        // between "the share is slow" and "the share never appears", and
        // the first version of this class dropped keyframes without a word.
        if (droppedNow > 0) {
            const auto sinceLog = std::chrono::duration<double>(now - m_lastDropLog).count();
            if (!m_everLoggedDrop || sinceLog > 2.0) {
                m_everLoggedDrop = true;
                m_lastDropLog = now;
                qCWarning(logVoicePc, " RTP pacer dropped %llu packet(s) (backlog cap %zu bytes, "
                          "ceiling %.0f kbps, %llu dropped in total)",
                          static_cast<unsigned long long>(droppedNow), maxBacklog,
                          rate * 8.0 / 1000.0,
                          static_cast<unsigned long long>(m_dropped.load(std::memory_order_relaxed)));
            }
        }

        while (!m_queue.empty() && m_budget > 0) {
            auto msg = std::move(m_queue.front());
            m_queue.pop();
            const double size = double(msg->size());
            m_backlogBytes -= msg->size();
            send(std::move(msg));
            m_budget -= size;
        }
    }

private:
    // A pacer that can throttle below this would be a bug generator,
    // not a smoother.
    static constexpr int kMinCeilingKbps = 1000;
    // Bucket depth: how much of a burst may leave back to back.
    static constexpr double kMaxBurstSeconds = 0.05;
    // Hard bound on queued-but-unsent bytes, in seconds of budget.
    // Must comfortably hold a whole keyframe. A 1080p screen IDR is
    // routinely 150-400 KB (field: 133 KB and 293 KB at 7.7 Mbps), and at
    // that bitrate the old 0.25 s cap was ~250 KB — so the leading packets
    // of every large IDR were dropped as "oldest", the viewer could never
    // assemble a complete keyframe, asked for another, and lost that one
    // the same way: a share that never appears. The cap still bounds
    // latency on a misconfigured-high bitrate; it must never bite on a
    // single access unit.
    static constexpr double kMaxBacklogSeconds = 1.0;
    static constexpr double kMinBacklogBytes = 4.0 * 1024 * 1024;

    std::atomic<double> m_bytesPerSecond{0.0};
    std::atomic<quint64> m_dropped{0};
    std::mutex m_mutex;
    std::queue<rtc::message_ptr> m_queue;
    size_t m_backlogBytes = 0;
    double m_budget = 0.0;
    bool m_started = false;
    std::chrono::steady_clock::time_point m_lastRun;
    std::chrono::steady_clock::time_point m_lastDropLog;
    bool m_everLoggedDrop = false;
};
} // namespace

// Surface libdatachannel's own error/warning log through ours. Its
// default sink is invisible in production — which is how "connection
// has no media transport" (every RTP frame silently dropped) went
// undiagnosed while both sides logged healthy-looking track opens.
static void initRtcLogging() {
    static std::once_flag once;
    std::call_once(once, []() {
        rtc::InitLogger(rtc::LogLevel::Warning,
            [](rtc::LogLevel level, rtc::string message) {
                if (level <= rtc::LogLevel::Error)
                    qCWarning(logVoicePc, "[libdatachannel] %s", message.c_str());
                else
                    qCInfo(logVoicePc, "[libdatachannel] %s", message.c_str());
            });
    });
}

PeerConnectionManager::PeerConnectionManager(const QString& peerId, const QString& callId,
                                             const rtc::Configuration& config, QObject* parent)
    : QObject(parent)
    , m_peerId(peerId)
    , m_callId(callId)
{
    qCInfo(logVoicePc, " Creating peer connection → %s (call %s)",
          qPrintable(peerId), qPrintable(callId));
    initRtcLogging();
    m_pc = std::make_shared<rtc::PeerConnection>(config);
    setupCallbacks();
}

void PeerConnectionManager::resetAllCallbacks() noexcept {
    // V-C2/S-6. Every libdatachannel callback this class installs captures
    // raw `this`, and close() is ASYNCHRONOUS while VoiceEngine::stop()
    // does a synchronous qDeleteAll(m_peers) — so between "close() called"
    // and "object freed" libdatachannel could still deliver a frame, a
    // state change or a message into freed memory. Leave / switch / quit /
    // peer drop all hit this window, and it is widest while video is
    // flowing, because onFrame fires per reassembled access unit.
    //
    // resetCallbacks() is the fix: it takes the same lock the dispatcher
    // holds, so it waits for any callback already running and guarantees
    // no new one starts. It must therefore run BEFORE close(), on every
    // rtc object this class owns — a track left with an onFrame handler is
    // the one that actually fires.
    //
    // m_alive is the second line of defence, for the handful of callbacks
    // that were already past their own dispatch when this ran.
    m_alive->store(false);
    forEachRtcHandle([](const auto& handle) {
        try { handle->resetCallbacks(); } catch (...) {}
        if constexpr (std::is_same_v<std::decay_t<decltype(*handle)>, rtc::Track>) {
            // Drop the media-handler chain too, before the track is
            // released. libdatachannel v0.24.5's PacingHandler schedules
            // itself on the global thread pool via weak_bind, which
            // protects the handler but not the `send` callback it holds
            // BY REFERENCE — and that reference belongs to the chain the
            // track owns. Clearing the chain here expires the weak_bind
            // rather than leaving a tick to fire into it.
            try { handle->setMediaHandler(nullptr); } catch (...) {}
        }
    });
}

void PeerConnectionManager::closeAllHandles() noexcept {
    // A throwing destructor is an immediate std::terminate — close()
    // touches the transport and can raise on a teardown race.
    forEachRtcHandle([this](const auto& handle) {
        try {
            handle->close();
        } catch (const std::exception& e) {
            qCWarning(logVoicePc, " [%s] close failed during teardown: %s",
                     qPrintable(m_peerId), e.what());
        } catch (...) {
            qCWarning(logVoicePc, " [%s] close failed during teardown",
                     qPrintable(m_peerId));
        }
    });
}

PeerConnectionManager::~PeerConnectionManager() {
    qCInfo(logVoicePc, " Destroying peer connection → %s (sent=%d recv=%d)",
          qPrintable(m_peerId), m_framesSent, m_framesReceived);
    // Order is load-bearing: detach every callback FIRST, only then close.
    resetAllCallbacks();
    closeAllHandles();
}

void PeerConnectionManager::failPeer(const char* where,
                                     const std::exception& e) {
    qCWarning(logVoicePc, " [%s] %s failed: %s — marking peer failed",
             qPrintable(m_peerId), where, e.what());
    if (m_peerState == PeerState::Failed) return;
    m_peerState = PeerState::Failed;
    // Synchronous: VoiceEngine tears this peer down (deleteLater) and
    // the mesh reconciler re-offers on its next poll.
    emit peerStateChanged(PeerState::Failed);
}

void PeerConnectionManager::setupCallbacks() {
    m_pc->onLocalDescription([this, alive = m_alive](rtc::Description desc) {
        if (!alive->load()) return;
        std::string type = desc.typeString();
        std::string sdp = std::string(desc);
        QMetaObject::invokeMethod(this, [this, type, sdp]() {
            qCInfo(logVoicePc, " [%s] Local SDP %s ready",
                  qPrintable(m_peerId), type.c_str());
            // Snapshot BEFORE the emit: VoiceEngine::onLocalDescription
            // routes on exactly this value (invite/answer vs negotiate),
            // and the flag flips below.
            const bool isRenegotiation = m_initialNegotiationDone;
            emit localDescriptionReady(type, sdp);
            // The emit above ran VoiceEngine's routing synchronously,
            // so the FIRST answer went out as m.call.answer while the
            // flag was still false; flipping it afterwards makes every
            // subsequent description take the negotiate path. (The
            // offerer side flips in applyAnswer instead.)
            if (type == "answer" && !m_initialNegotiationDone)
                m_initialNegotiationDone = true;
            // Any post-initial local OFFER is a re-offer, however it was
            // produced. triggerRenegotiation() is NOT the only producer:
            // libdatachannel auto-negotiation re-offers on its own when
            // the PC changes shape (e.g. sendLosslessFrame's lazy
            // createDataChannel("video-lossless")). Tracking only the
            // explicit trigger left m_localReofferPending false for
            // those, applyNegotiateAnswer discarded the peer's answer,
            // and the PC wedged in HaveLocalOffer forever — after which
            // every inbound negotiate offer threw. Latch on the offer we
            // actually emitted instead of on intent.
            if (isRenegotiation && type == "offer")
                m_localReofferPending = true;
        }, Qt::QueuedConnection);
    });

    m_pc->onLocalCandidate([this, alive = m_alive](rtc::Candidate candidate) {
        if (!alive->load()) return;
        std::string cand = std::string(candidate);
        std::string mid = candidate.mid();
        QMetaObject::invokeMethod(this, [this, cand, mid]() {
            emit localCandidateReady(cand, mid);
        }, Qt::QueuedConnection);
    });

    m_pc->onStateChange([this, alive = m_alive](rtc::PeerConnection::State state) {
        if (!alive->load()) return;
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

    m_pc->onIceStateChange([this, alive = m_alive](rtc::PeerConnection::IceState state) {
        if (!alive->load()) return;
        QMetaObject::invokeMethod(this, [this, state]() {
            qCInfo(logVoicePc, " [%s] ICE state: %s",
                  qPrintable(m_peerId), iceStr(state));
        }, Qt::QueuedConnection);
    });

    m_pc->onGatheringStateChange([this, alive = m_alive](rtc::PeerConnection::GatheringState state) {
        if (!alive->load()) return;
        QMetaObject::invokeMethod(this, [this, state]() {
            qCInfo(logVoicePc, " [%s] ICE gathering: %s",
                  qPrintable(m_peerId), gatherStr(state));
        }, Qt::QueuedConnection);
    });

    m_pc->onDataChannel([this, alive = m_alive](std::shared_ptr<rtc::DataChannel> dc) {
        if (!alive->load()) return;
        QMetaObject::invokeMethod(this, [this, dc]() {
            qCInfo(logVoicePc, " [%s] Incoming data channel \"%s\"",
                  qPrintable(m_peerId), dc->label().c_str());
            // Dispatch on label — the lossless-video channel must not
            // clobber the audio/control channel.
            if (dc->label() == "video-lossless")
                setupLosslessChannel(dc);
            else if (dc->label() == "control")
                setupControlChannel(dc);
            else
                setupDataChannel(dc);
        }, Qt::QueuedConnection);
    });

    // Remote added video m-lines via renegotiation (we're the
    // answerer side of the video upgrade) — adopt the tracks; the
    // m-lines are SendRecv so this same track carries our outgoing
    // video without another renegotiation.
    m_pc->onTrack([this, alive = m_alive](std::shared_ptr<rtc::Track> track) {
        if (!alive->load()) return;
        QMetaObject::invokeMethod(this, [this, track]() {
            const int idx = streamIndexForMid(track->mid());
            qCInfo(logVoicePc, " [%s] Incoming track mid=%s",
                  qPrintable(m_peerId), track->mid().c_str());
            if (idx < 0) return;   // unknown m-line — future stream kind
            if (m_video[idx].track) return;   // already have it
            attachVideoTrack(VideoStreamId(idx), track, /*adopted=*/true);
        }, Qt::QueuedConnection);
    });
}

void PeerConnectionManager::setupDataChannel(std::shared_ptr<rtc::DataChannel> dc) {
    m_dc = dc;

    m_dc->onOpen([this, alive = m_alive]() {
        if (!alive->load()) return;
        QMetaObject::invokeMethod(this, [this]() {
            qCInfo(logVoicePc, " [%s] DataChannel open — audio can flow",
                  qPrintable(m_peerId));
            // Fallback path for control: whatever was queued before any
            // channel existed goes out now, in order.
            flushPendingControl();
        }, Qt::QueuedConnection);
    });

    m_dc->onClosed([this, alive = m_alive]() {
        if (!alive->load()) return;
        QMetaObject::invokeMethod(this, [this]() {
            qCInfo(logVoicePc, " [%s] DataChannel closed",
                  qPrintable(m_peerId));
        }, Qt::QueuedConnection);
    });

    m_dc->onMessage([this, alive = m_alive](rtc::message_variant msg) {
        if (!alive->load()) return;
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
            // Reserved. Lossless video moved to its own dedicated
            // "video-lossless" channel; 0x05 stays claimed here so it
            // can never fall through to the legacy-audio branch.
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

void PeerConnectionManager::ensureControlChannel() {
    if (m_controlDc || !m_pc) return;
    // HARD compatibility gate. A build that predates this channel
    // dispatches every incoming data channel except "video-lossless"
    // into setupDataChannel(), which would REBIND ITS AUDIO CHANNEL to
    // whatever we just opened — its Opus would then ride a reliable,
    // ordered channel and stall behind retransmissions. Fixing video
    // must not damage audio for anyone who has not updated, so the
    // channel is opened only toward a peer that says it knows the
    // label. Everyone else keeps getting control on the audio channel,
    // exactly as before.
    if (!peerUsesControlChannel(m_remoteCaps, m_remoteCapsKnown)) return;
    // Only the offerer opens channels; the answerer adopts by label via
    // onDataChannel. Opening from both sides would produce two.
    if (!m_isOfferer) return;
    try {
        // Reliable and ordered are the DataChannelInit defaults —
        // exactly what a keyframe request needs. Opens in-band; the
        // renegotiation libdatachannel raises for it is handled by the
        // auto-negotiation path in setupCallbacks (same as the lossless
        // channel).
        auto dc = m_pc->createDataChannel("control");
        setupControlChannel(dc);
        qCInfo(logVoicePc, " [%s] opening reliable control channel",
              qPrintable(m_peerId));
    } catch (const std::exception& e) {
        qCWarning(logVoicePc, " [%s] control channel create failed: %s "
                 "— control stays on the audio channel",
                 qPrintable(m_peerId), e.what());
    }
}

void PeerConnectionManager::setupControlChannel(std::shared_ptr<rtc::DataChannel> dc) {
    m_controlDc = dc;
    dc->onOpen([this, alive = m_alive]() {
        if (!alive->load()) return;
        QMetaObject::invokeMethod(this, [this]() {
            qCInfo(logVoicePc, " [%s] control channel open (reliable)",
                  qPrintable(m_peerId));
            flushPendingControl();
        }, Qt::QueuedConnection);
    });
    dc->onMessage([this, alive = m_alive](rtc::message_variant msg) {
        if (!alive->load()) return;
        if (!std::holds_alternative<rtc::binary>(msg)) return;
        auto& data = std::get<rtc::binary>(msg);
        if (data.empty()) return;
        // Same 0x04 framing as the audio channel so both paths hand the
        // engine an identical payload. Untagged JSON is accepted too
        // (a '{' is 0x7B, never 0x04) — cheap tolerance for anything
        // that ever writes this channel without the tag.
        const bool tagged = std::to_integer<uint8_t>(data[0]) == 0x04;
        const size_t off = tagged ? 1u : 0u;
        QByteArray payload(reinterpret_cast<const char*>(data.data() + off),
                           int(data.size() - off));
        QMetaObject::invokeMethod(this, [this, payload]() {
            emit controlMessageReceived(payload);
        }, Qt::QueuedConnection);
    });
}

void PeerConnectionManager::createOffer() {
    qCInfo(logVoicePc, " [%s] Creating offer (we are offerer)",
          qPrintable(m_peerId));
    m_isOfferer = true;
    // Video m-lines MUST be in the initial offer. libdatachannel
    // instantiates the transport from the first negotiation: a data-
    // channel-only offer gets plain DTLS with NO SRTP, and m-lines
    // added by later renegotiation "open" at the SDP level while
    // openTracks() fails with "connection has no media transport" —
    // every RTP packet silently dies. (Reproduced in
    // test_media_loopback: initial-offer case passes, renegotiated
    // case fails.) Announcing the m-lines costs nothing when unused —
    // whether video actually flows is gated on peer caps at send time.
    //
    // Tracks are added BEFORE the data channel: createDataChannel can
    // auto-trigger negotiation immediately, and if that offer races
    // ahead of addTrack it carries no media section — same trap via a
    // different door (caught by the loopback test flaking on CI).
    try {
        for (int i = 0; i < kVideoStreamCount; ++i) {
            rtc::Description::Video media(kVideoSpecs[i].mid,
                                          rtc::Description::Direction::SendRecv);
            media.addH264Codec(kVideoSpecs[i].payloadType);
            declareVideoSsrcs(media, kVideoSpecs[i]);
            auto track = m_pc->addTrack(std::move(media));
            attachVideoTrack(VideoStreamId(i), track, /*adopted=*/false);
        }

        // Create unreliable DataChannel for audio
        rtc::DataChannelInit dcInit;
        dcInit.reliability.unordered = true;
        dcInit.reliability.maxRetransmits = 0;

        auto dc = m_pc->createDataChannel("audio", dcInit);
        setupDataChannel(dc);

        // The reliable "control" channel is NOT opened here: at this
        // point the peer's caps are unknown, and it may only be opened
        // toward a peer that advertises control_dc. See
        // ensureControlChannel().

        // createDataChannel() above triggers libdatachannel's own
        // negotiation, which sets the local description itself. Asking
        // again once it has is not just redundant — it logs
        // "Unexpected local description in signaling state
        // have-local-offer, ignoring" on EVERY offer we make, which
        // reads like the offer was dropped when in fact the auto-
        // negotiated one went out. Only set it when we are still in
        // Stable, i.e. when nothing has negotiated for us.
        if (m_pc->signalingState() == rtc::PeerConnection::SignalingState::Stable)
            m_pc->setLocalDescription(rtc::Description::Type::Offer);
    } catch (const std::exception& e) {
        failPeer("createOffer", e);
    }
}

void PeerConnectionManager::applyOffer(const std::string& sdp) {
    if (!m_pc) return;
    qCInfo(logVoicePc, " [%s] Applying remote offer", qPrintable(m_peerId));
    try {
        rtc::Description desc(sdp, rtc::Description::Type::Offer);
        m_pc->setRemoteDescription(desc);
        m_remoteDescriptionSet = true;
        flushPendingCandidates();

        // libdatachannel auto-negotiates: setRemoteDescription(offer)
        // already produced the answer (it arrives via onLocalDescription)
        // and returned the signaling state to Stable. Setting the answer a
        // second time from Stable throws "Unexpected local desciption type
        // answer in signaling state stable". That throw used to be
        // survivable — the queued answer still went out — until a failed
        // peer started being removed at once (rc.10+), which dropped the
        // answer with it: the answerer never replied, every call timed out
        // after 30 s, and nobody could hear or see anybody (rc.13 field
        // report). Only answer explicitly if auto-negotiation did not.
        if (m_pc->signalingState() == rtc::PeerConnection::SignalingState::HaveRemoteOffer)
            m_pc->setLocalDescription(rtc::Description::Type::Answer);
    } catch (const std::exception& e) {
        failPeer("applyOffer", e);
    }
}

void PeerConnectionManager::applyAnswer(const std::string& sdp) {
    if (!m_pc) return;
    qCInfo(logVoicePc, " [%s] Applying remote answer", qPrintable(m_peerId));
    // A duplicated m.call.answer (the timeline is persisted and replays
    // on re-sync) arrives in Stable and throws std::logic_error out of
    // setRemoteDescription. Cheap pre-check keeps the common case out of
    // the catch block; the catch still covers malformed SDP.
    if (m_initialNegotiationDone) {
        qCInfo(logVoicePc, " [%s] Ignoring duplicate answer — initial "
              "negotiation already complete", qPrintable(m_peerId));
        return;
    }
    try {
        rtc::Description desc(sdp, rtc::Description::Type::Answer);
        m_pc->setRemoteDescription(desc);
        m_remoteDescriptionSet = true;
        m_initialNegotiationDone = true;
        flushPendingCandidates();
    } catch (const std::exception& e) {
        failPeer("applyAnswer", e);
    }
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
    try {
        m_pc->setLocalDescription();
    } catch (const std::exception& e) {
        m_localReofferPending = false;
        failPeer("triggerRenegotiation", e);
    }
}

void PeerConnectionManager::applyNegotiateOffer(const std::string& sdp) {
    if (!m_pc) return;
    qCInfo(logVoicePc, " [%s] Applying renegotiation offer", qPrintable(m_peerId));
    try {
        rtc::Description desc(sdp, rtc::Description::Type::Offer);
        m_pc->setRemoteDescription(desc);
        m_pc->setLocalDescription(rtc::Description::Type::Answer);
    } catch (const std::exception& e) {
        failPeer("applyNegotiateOffer", e);
        return;
    }
    maybeRenegotiateAgain();
}

void PeerConnectionManager::applyNegotiateAnswer(const std::string& sdp) {
    if (!m_pc) return;
    // m_localReofferPending is now latched on every local offer we
    // actually emit (see setupCallbacks), so it covers auto-negotiated
    // re-offers too. Keep signalingState as a belt-and-braces second
    // opinion: if the PC really is sitting in HaveLocalOffer, this
    // answer is the only thing that can unwedge it.
    const bool haveLocalOffer =
        m_pc->signalingState()
        == rtc::PeerConnection::SignalingState::HaveLocalOffer;
    if (!m_localReofferPending && !haveLocalOffer) {
        qCInfo(logVoicePc, " [%s] Ignoring unexpected negotiate answer",
              qPrintable(m_peerId));
        return;
    }
    qCInfo(logVoicePc, " [%s] Applying renegotiation answer", qPrintable(m_peerId));
    try {
        rtc::Description desc(sdp, rtc::Description::Type::Answer);
        m_pc->setRemoteDescription(desc);
    } catch (const std::exception& e) {
        m_localReofferPending = false;
        failPeer("applyNegotiateAnswer", e);
        return;
    }
    m_localReofferPending = false;
    maybeRenegotiateAgain();
}

void PeerConnectionManager::rollbackLocalReoffer() {
    if (!m_pc || !m_localReofferPending) return;
    qCInfo(logVoicePc, " [%s] Rolling back local re-offer (glare, polite side)",
          qPrintable(m_peerId));
    m_localReofferPending = false;
    // Whatever we wanted to negotiate (added tracks) is still attached
    // to the PC — re-offer once the winning exchange completes.
    m_renegotiateAgain = true;
    try {
        m_pc->setLocalDescription(rtc::Description::Type::Rollback);
    } catch (const std::exception& e) {
        failPeer("rollbackLocalReoffer", e);
    }
}

void PeerConnectionManager::maybeRenegotiateAgain() {
    if (!m_renegotiateAgain) return;
    if (!m_pc) return;
    if (m_pc->signalingState() != rtc::PeerConnection::SignalingState::Stable) return;
    m_renegotiateAgain = false;
    triggerRenegotiation();
}

void PeerConnectionManager::sendControl(const QByteArray& json) {
    // HARD compatibility gate: legacy clients misparse unknown tags as
    // audio frames (see onMessage's fallback), so control traffic may
    // only flow once the caps handshake proved the peer understands it.
    if (!remoteSupportsVideoRtp()) return;
    // Brings the reliable channel up the first time control traffic
    // flows toward a peer that advertises it; no-op otherwise.
    ensureControlChannel();
    if (deliverControl(json)) return;
    // Nothing is open YET. That is the normal state at the only moment
    // the engine has to tell a joining peer about a stream that is
    // already running: caps arrive with the SDP answer, a full round
    // trip before any data channel opens. Dropping the message here is
    // how a viewer who joined during a share was never told the share
    // existed (rc.15) — it saw the camera, started after it connected,
    // and nothing else. Hold it until a channel opens.
    if (m_pendingControl.size() >= kMaxPendingControl) {
        // Bounded: control is small and idempotent-ish, and a peer that
        // never opens a channel is a peer that is going away.
        m_pendingControl.removeFirst();
        qCDebug(logVoicePc, " [%s] pre-open control backlog full — "
               "dropped the oldest message", qPrintable(m_peerId));
    }
    m_pendingControl.append(json);
}

// Returns false when there is no open channel to put this on; the
// caller decides whether to queue it or let it go.
bool PeerConnectionManager::deliverControl(const QByteArray& json) {
    rtc::binary data;
    data.reserve(json.size() + 1);
    data.push_back(std::byte{0x04});
    auto* raw = reinterpret_cast<const std::byte*>(json.constData());
    data.insert(data.end(), raw, raw + json.size());

    // Reliable path first. Control messages are tiny (tens of bytes), so
    // the max-message-size guard sendOnDataChannel() applies to media is
    // not needed here; a throw is still possible on a teardown race.
    if (m_controlDc && m_controlDc->isOpen()) {
        try {
            m_controlDc->send(data);
            return true;
        } catch (const std::exception& e) {
            qCDebug(logVoicePc, " [%s] control send failed on reliable "
                   "channel (%s) — falling back to the audio channel",
                   qPrintable(m_peerId), e.what());
        }
    }
    if (!m_dc || !m_dc->isOpen()) return false;
    return sendOnDataChannel(std::move(data), "control");
}

// Called from every channel's onOpen. Order is preserved, and anything
// that still cannot go out (the audio channel opened but the message
// was rejected) goes back on the queue for the next opening.
void PeerConnectionManager::flushPendingControl() {
    if (m_pendingControl.isEmpty()) return;
    const QList<QByteArray> queued = std::move(m_pendingControl);
    m_pendingControl.clear();
    qCInfo(logVoicePc, " [%s] replaying %d control message(s) held until a "
          "channel opened", qPrintable(m_peerId), int(queued.size()));
    for (const QByteArray& json : queued) {
        if (!deliverControl(json))
            m_pendingControl.append(json);
    }
}

bool PeerConnectionManager::videoMidsAlreadyDeclared() const {
    if (!m_pc) return true;   // nothing we could safely add anyway
    const auto declares = [](const auto& desc) {
        if (!desc) return false;
        for (const auto& spec : kVideoSpecs)
            if (desc->hasMid(spec.mid)) return true;
        return false;
    };
    return declares(m_pc->localDescription())
        || declares(m_pc->remoteDescription());
}

void PeerConnectionManager::ensureVideoTracks() {
    if (!m_pc || m_video[0].track) return;
    if (!remoteSupportsVideoRtp()) return;
    // INVARIANT: between two BSFChat clients the video m-lines are
    // always present from the INITIAL exchange — the offerer puts them
    // in createOffer() (they MUST be there or libdatachannel builds a
    // transport with no SRTP), and the answerer adopts them via
    // onTrack(). So the body below is normally unreachable and this
    // whole path is a fallback for a peer that negotiated without them.
    //
    // It is NOT dead, though, and the guard above is not sufficient:
    // onTrack is delivered through a QUEUED invocation, so on the
    // answerer side m_video[0].track is still null for one event-loop
    // turn after applyOffer() — and VoiceEngine::handleCallInvite calls
    // maybeSetupVideoFor() synchronously inside exactly that window.
    // Adding tracks there would emit a second m-line for a mid the SDP
    // already carries. Consult the negotiated descriptions rather than
    // our own (lagging) bookkeeping.
    if (videoMidsAlreadyDeclared()) {
        qCDebug(logVoicePc, " [%s] video m-lines already negotiated — "
               "tracks arrive via onTrack, not adding", qPrintable(m_peerId));
        return;
    }
    qCInfo(logVoicePc, " [%s] Adding video tracks + renegotiating",
          qPrintable(m_peerId));
    try {
        for (int i = 0; i < kVideoStreamCount; ++i) {
            rtc::Description::Video media(kVideoSpecs[i].mid,
                                          rtc::Description::Direction::SendRecv);
            media.addH264Codec(kVideoSpecs[i].payloadType);
            declareVideoSsrcs(media, kVideoSpecs[i]);
            auto track = m_pc->addTrack(std::move(media));
            attachVideoTrack(VideoStreamId(i), track, /*adopted=*/false);
        }
    } catch (const std::exception& e) {
        failPeer("ensureVideoTracks", e);
        return;
    }
    triggerRenegotiation();
}

void PeerConnectionManager::attachVideoTrack(VideoStreamId stream,
                                             std::shared_ptr<rtc::Track> track,
                                             bool adopted) {
    const int idx = int(stream);
    const auto& spec = kVideoSpecs[idx];
    auto& ctx = m_video[idx];

    ctx.track = track;
    ctx.startTimeUs = -1;
    // Which half of the m-line's declared SSRC pair is ours depends on
    // which side put the m-line there, NOT on m_isOfferer: the fallback
    // renegotiation path (ensureVideoTracks) can add tracks from either
    // role. `adopted` is exactly that question. Getting this wrong
    // collides the two endpoints' SSRCs and the depacketizer interleaves
    // two senders' packets into one broken access unit — see
    // kVideoSpecs for why the SSRC is fixed rather than random.
    ctx.rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
        adopted ? spec.answererSsrc : spec.offererSsrc, spec.cname,
        spec.payloadType, rtc::H264RtpPacketizer::ClockRate);

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
    // one UDP salvo (bursts are what routers drop first). The ceiling
    // tracks the encoder's own max bitrate (setVideoPacingCeilingKbps)
    // so pacing only ever shaves peaks, and the backlog is bounded —
    // see PacedRtpSender for why the stock handler could not do either.
    ctx.pacer = std::make_shared<PacedRtpSender>(
        m_pacerCeilingKbps[idx] > 0 ? m_pacerCeilingKbps[idx]
                                    : kDefaultPacerCeilingKbps);
    packetizer->addToChain(ctx.pacer);
    packetizer->addToChain(std::make_shared<rtc::H264RtpDepacketizer>(
        rtc::NalUnit::Separator::LongStartSequence));
    // Gap detector AFTER the depacketizer in build order = BEFORE it
    // on the incoming (tail→head) traversal, so lossPending is set
    // before the depacketizer assembles — and onFrame delivers — the
    // access unit the gap corrupted.
    packetizer->addToChain(std::make_shared<RtpGapDetector>([this, idx]() {
        m_video[idx].lossPending.store(true, std::memory_order_relaxed);
    }));
    packetizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
    track->setMediaHandler(packetizer);

    track->onFrame([this, idx, alive = m_alive](rtc::binary data, rtc::FrameInfo) {
        if (!alive->load()) return;
        QByteArray au(reinterpret_cast<const char*>(data.data()),
                      int(data.size()));
        if (!m_video[idx].rxLogged.exchange(true)) {
            qCInfo(logVoicePc, " [%s] first video AU received on %s (%d bytes)",
                  qPrintable(m_peerId), kVideoSpecs[idx].mid, int(au.size()));
        }
        // Same-thread as the gap detector (libdatachannel delivers
        // frames during the incoming chain traversal), so this
        // read-and-clear pairs exactly with the AU it corrupted.
        const bool loss = m_video[idx].lossPending.exchange(
            false, std::memory_order_relaxed);
        QMetaObject::invokeMethod(this, [this, idx, au, loss]() {
            emit videoFrameReceived(idx, au, loss);
        }, Qt::QueuedConnection);
    });
    track->onOpen([this, idx, alive = m_alive]() {
        if (!alive->load()) return;
        QMetaObject::invokeMethod(this, [this, idx]() {
            qCInfo(logVoicePc, " [%s] Video track %s open",
                  qPrintable(m_peerId), kVideoSpecs[idx].mid);
            m_video[idx].open = true;
            emit videoTrackOpen(idx);
        }, Qt::QueuedConnection);
    });
    track->onClosed([this, idx, alive = m_alive]() {
        if (!alive->load()) return;
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
    // S-1: never push H.264 at a peer that cannot decode it. The track
    // being open proves only that the m-lines were negotiated — they
    // are in every initial offer — not that anything on the far side
    // can turn the bytes back into pictures. VoiceEngine gates on the
    // same predicate; this is the backstop that makes it impossible to
    // reach the wire by another route.
    if (!remoteCanReceiveRtpVideo()) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - ctx.txSkipLogMs > 5000) {
            ctx.txSkipLogMs = now;
            qCInfo(logVoicePc, " [%s] video tx skipped for %s: peer "
                  "advertises no h264 decode (legacy JPEG path instead)",
                  qPrintable(m_peerId), kVideoSpecs[int(stream)].mid);
        }
        return;
    }
    if (!ctx.open || !ctx.track || !ctx.track->isOpen()) {
        // Encoder is producing but this track can't carry it — say so
        // (throttled), a silent return here once hid a dead share.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - ctx.txSkipLogMs > 5000) {
            ctx.txSkipLogMs = now;
            qCInfo(logVoicePc, " [%s] video tx skipped for %s: track %s",
                  qPrintable(m_peerId), kVideoSpecs[int(stream)].mid,
                  !ctx.track ? "missing" : (ctx.open ? "closed at rtc layer"
                                                     : "not open yet"));
        }
        return;
    }
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
        if (!ctx.txLogged) {
            ctx.txLogged = true;
            qCInfo(logVoicePc, " [%s] first video AU sent on %s (%d bytes%s)",
                  qPrintable(m_peerId), kVideoSpecs[int(stream)].mid,
                  int(frame.data.size()), frame.keyframe ? ", IDR" : "");
        }
    } catch (const std::exception& e) {
        // Transient (track closing mid-send) — the open flag will
        // catch up via onClosed; don't spam.
        qCDebug(logVoicePc, " [%s] video send failed: %s",
               qPrintable(m_peerId), e.what());
    }
}

void PeerConnectionManager::setVideoPacingCeilingKbps(VideoStreamId stream,
                                                      int maxKbps) {
    const int idx = int(stream);
    m_pacerCeilingKbps[idx] = maxKbps;
    if (auto pacer = std::static_pointer_cast<PacedRtpSender>(m_video[idx].pacer))
        pacer->setCeilingKbps(maxKbps);
}

void PeerConnectionManager::requestPeerKeyframe(VideoStreamId stream) {
    sendControl(QByteArrayLiteral("{\"t\":\"kf\",\"stream\":")
                + QByteArray::number(int(stream)) + "}");
}

void PeerConnectionManager::setupLosslessChannel(std::shared_ptr<rtc::DataChannel> dc) {
    m_losslessDc = dc;
    dc->onMessage([this, alive = m_alive](rtc::message_variant msg) {
        if (!alive->load()) return;
        if (!std::holds_alternative<rtc::binary>(msg)) return;
        auto& data = std::get<rtc::binary>(msg);
        if (data.size() < 10) return;  // [stream][flags][seq][tsMs]
        const int stream = int(std::to_integer<uint8_t>(data[0]));
        if (stream < 0 || stream >= kVideoStreamCount) return;
        const uint8_t flags = std::to_integer<uint8_t>(data[1]);
        const bool keyframe = (flags & 0x1) != 0;

        // Chunked frame (flags 0x2): header grows by
        // [chunkIdx:u16][chunkTotal:u16]; payload chunks of one frame
        // arrive contiguously on this reliable+ordered channel and are
        // reassembled here before the normal emit.
        if (flags & 0x2) {
            if (data.size() < 14) return;
            quint32 seq = 0;
            for (int i = 2; i < 6; ++i)
                seq = (seq << 8) | std::to_integer<uint8_t>(data[i]);
            const quint16 idx = quint16((std::to_integer<uint8_t>(data[10]) << 8)
                                        | std::to_integer<uint8_t>(data[11]));
            const quint16 total = quint16((std::to_integer<uint8_t>(data[12]) << 8)
                                          | std::to_integer<uint8_t>(data[13]));
            if (total == 0 || idx >= total) return;
            if (idx == 0) {
                m_losslessAsm[stream].clear();
                m_losslessAsmSeq[stream] = seq;
                m_losslessAsmNext[stream] = 0;
            } else if (seq != m_losslessAsmSeq[stream]
                       || idx != m_losslessAsmNext[stream]) {
                // Gap or interleaving that shouldn't happen — drop the
                // partial frame; the next chunk 0 restarts cleanly.
                m_losslessAsm[stream].clear();
                m_losslessAsmNext[stream] = 0;
                return;
            }
            m_losslessAsm[stream].append(
                reinterpret_cast<const char*>(data.data() + 14),
                int(data.size() - 14));
            m_losslessAsmNext[stream] = quint16(idx + 1);
            if (idx + 1 < total) return;   // more chunks coming

            QByteArray tu = std::move(m_losslessAsm[stream]);
            m_losslessAsm[stream] = QByteArray();
            QMetaObject::invokeMethod(this, [this, stream, tu, keyframe]() {
                emit losslessFrameReceived(stream, tu, keyframe);
            }, Qt::QueuedConnection);
            return;
        }

        QByteArray tu(reinterpret_cast<const char*>(data.data() + 10),
                      int(data.size() - 10));
        QMetaObject::invokeMethod(this, [this, stream, tu, keyframe]() {
            emit losslessFrameReceived(stream, tu, keyframe);
        }, Qt::QueuedConnection);
    });
}

void PeerConnectionManager::sendLosslessFrame(VideoStreamId stream,
                                              const EncodedFrame& frame) {
    if (!m_pc) return;
    if (!remoteSupportsVideoRtp()) return;   // caps gate, like sendControl
    if (!m_losslessDc) {
        // Reliable + ordered are data-channel defaults — exactly what
        // lossless wants. Opens in-band, no renegotiation.
        try {
            auto dc = m_pc->createDataChannel("video-lossless");
            setupLosslessChannel(dc);
        } catch (const std::exception& e) {
            qCWarning(logVoicePc, " [%s] lossless channel create failed: %s",
                     qPrintable(m_peerId), e.what());
            return;
        }
    }
    if (!m_losslessDc->isOpen()) return;

    quint32& seq = m_losslessSeq[int(stream)];
    const quint32 tsMs = quint32(frame.captureTimeUs / 1000);

    // Header writer shared by the single-message and chunked paths.
    // flags: 0x1 keyframe, 0x2 chunked ([idx:u16][total:u16] appended).
    auto putHeader = [&](rtc::binary& out, quint8 flags) {
        out.push_back(std::byte(quint8(stream)));
        out.push_back(std::byte(flags));
        for (int shift = 24; shift >= 0; shift -= 8)
            out.push_back(std::byte(quint8(seq >> shift)));
        for (int shift = 24; shift >= 0; shift -= 8)
            out.push_back(std::byte(quint8(tsMs >> shift)));
    };
    const quint8 kfFlag = frame.keyframe ? 0x1 : 0x0;
    auto* raw = reinterpret_cast<const std::byte*>(frame.data.constData());
    const size_t totalBytes = size_t(frame.data.size());
    // Stay comfortably under the negotiated cap — SCTP refuses (throws
    // on) anything larger, which once silently blackholed every frame
    // of a share (identity-I444 1080p far exceeds the default 256 KB).
    const size_t maxMsg = size_t(m_losslessDc->maxMessageSize());
    const size_t chunkBudget = maxMsg > 4096 ? maxMsg - 1024 : 65536;

    try {
        if (totalBytes + 10 <= maxMsg) {
            rtc::binary data;
            data.reserve(totalBytes + 10);
            putHeader(data, kfFlag);
            data.insert(data.end(), raw, raw + totalBytes);
            m_losslessDc->send(data);
        } else {
            const size_t nChunks = (totalBytes + chunkBudget - 1) / chunkBudget;
            if (nChunks > 0xFFFF) return;   // absurd; drop
            for (size_t i = 0; i < nChunks; ++i) {
                const size_t off = i * chunkBudget;
                const size_t len = std::min(chunkBudget, totalBytes - off);
                rtc::binary data;
                data.reserve(len + 14);
                putHeader(data, quint8(kfFlag | 0x2));
                data.push_back(std::byte(quint8(i >> 8)));
                data.push_back(std::byte(quint8(i)));
                data.push_back(std::byte(quint8(nChunks >> 8)));
                data.push_back(std::byte(quint8(nChunks)));
                data.insert(data.end(), raw + off, raw + off + len);
                m_losslessDc->send(data);
            }
        }
        ++seq;
        m_txFrames[int(stream)] += 1;
        m_txBytes[int(stream)] += quint64(frame.data.size());
    } catch (const std::exception& e) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_losslessStallEmitMs > 5000) {
            m_losslessStallEmitMs = now;
            qCWarning(logVoicePc, " [%s] lossless send failed: %s "
                     "(frame %d bytes, channel cap %d) — signalling fallback",
                     qPrintable(m_peerId), e.what(), int(totalBytes),
                     int(maxMsg));
            QMetaObject::invokeMethod(this, [this]() {
                emit losslessSendStalled();
            }, Qt::QueuedConnection);
        }
    }
}

qint64 PeerConnectionManager::losslessBufferedAmount() const {
    return m_losslessDc ? qint64(m_losslessDc->bufferedAmount()) : 0;
}

void PeerConnectionManager::addRemoteCandidate(const std::string& candidate, const std::string& mid) {
    if (!m_pc) return;
    if (!m_remoteDescriptionSet) {
        m_pendingCandidates.emplace_back(candidate, mid);
        return;
    }
    // A single malformed/unparseable candidate is not worth killing an
    // otherwise healthy connection over — ICE simply proceeds without
    // it. Log and carry on rather than calling failPeer().
    try {
        m_pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
    } catch (const std::exception& e) {
        qCWarning(logVoicePc, " [%s] rejected remote candidate (mid=%s): %s",
                 qPrintable(m_peerId), mid.c_str(), e.what());
    }
}

void PeerConnectionManager::flushPendingCandidates() {
    if (m_pendingCandidates.empty()) return;
    qCInfo(logVoicePc, " [%s] Flushing %d buffered ICE candidates",
          qPrintable(m_peerId), int(m_pendingCandidates.size()));
    // Take a copy first: addRemoteCandidate() below is the guarded form
    // and re-entering this function must not iterate a mutating vector.
    const auto pending = std::move(m_pendingCandidates);
    m_pendingCandidates.clear();
    for (const auto& [cand, mid] : pending) {
        try {
            m_pc->addRemoteCandidate(rtc::Candidate(cand, mid));
        } catch (const std::exception& e) {
            qCWarning(logVoicePc, " [%s] rejected buffered candidate: %s",
                     qPrintable(m_peerId), e.what());
        }
    }
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
        if (sendOnDataChannel(std::move(data), "audio"))
            m_framesSent++;
    }
}

bool PeerConnectionManager::sendOnDataChannel(rtc::binary&& data,
                                              const char* what) {
    // Every DataChannel::send() can throw: std::invalid_argument when
    // the message exceeds the negotiated SCTP max message size
    // (DEFAULT 256 KB — a 3840px Q100 JPEG is ~1.5 MB), and runtime
    // errors on transport teardown races. These sends run inside Qt
    // timer/slot handlers, so an escaped exception unwinds the event
    // loop and aborts the whole app (0xc0000409 fail-fast) — which is
    // precisely how "screen share at max quality instantly crashes
    // with a legacy peer" manifested. Drop the frame instead; the
    // stream self-heals on the next one.
    if (data.size() > m_dc->maxMessageSize()) {
        // Rate-limit: complain once per ~5 s per peer, not per frame.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastOversizeWarnMs > 5000) {
            m_lastOversizeWarnMs = now;
            qCWarning(logVoicePc,
                     " [%s] %s frame dropped: %zu bytes exceeds channel "
                     "max %zu — lower screen-share resolution/quality "
                     "for legacy peers",
                     qPrintable(m_peerId), what, data.size(),
                     m_dc->maxMessageSize());
        }
        return false;
    }
    try {
        m_dc->send(std::move(data));
        return true;
    } catch (const std::exception& e) {
        qCWarning(logVoicePc, " [%s] %s send failed: %s",
                 qPrintable(m_peerId), what, e.what());
        return false;
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
    if (!sendOnDataChannel(std::move(data), "screen"))
        ++m_screenFramesDropped;
}

void PeerConnectionManager::sendCameraFrame(const QByteArray& jpegData) {
    if (!m_dc || !m_dc->isOpen()) return;
    rtc::binary data;
    data.reserve(jpegData.size() + 1);
    data.push_back(std::byte{0x03});
    auto* raw = reinterpret_cast<const std::byte*>(jpegData.constData());
    data.insert(data.end(), raw, raw + jpegData.size());
    sendOnDataChannel(std::move(data), "camera");
}
