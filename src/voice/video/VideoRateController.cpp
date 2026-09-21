#include "voice/video/VideoRateController.h"

#include <QLoggingCategory>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(logVideoRate, "bsfchat.video.rate", QtWarningMsg)

using videorate::Content;
using T = videorate::Thresholds;
using SP = videorate::SenderPolicy;
using videosend::Bottleneck;

VideoRateController::VideoRateController(VideoStreamId streamId, QObject* parent)
    : QObject(parent)
    , m_streamId(streamId)
{
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &VideoRateController::tick);
}

qint64 VideoRateController::nowMs() const {
    return m_testNowMs >= 0 ? m_testNowMs : QDateTime::currentMSecsSinceEpoch();
}

Content VideoRateController::content() const {
    return m_streamId == VideoStreamId::Camera ? Content::Camera
                                               : Content::Screen;
}

void VideoRateController::setCodec(VideoCodecKind codec) {
    if (codec == m_codec) return;
    m_codec = codec;
    // Deliberately no state reset: the measured path capacity did not
    // change because we changed codec, and throwing away the converged
    // bitrate would make every switch cost a fresh climb. Only the
    // floors this bitrate is judged against move.
}

void VideoRateController::setEnvelope(int minKbps, int maxKbps, int fps,
                                      int maxLongEdge) {
    m_minKbps = qMax(50, minKbps);
    m_maxKbps = qMax(m_minKbps, maxKbps);
    m_fps = qBound(1, fps, 60);
    m_maxLongEdge = qBound(160, maxLongEdge, 3840);
    m_bitrate = qBound(m_minKbps, m_bitrate, m_maxKbps);
}

// Where a fresh share opens. NOT "half the envelope": with a camera
// target of 1500 kbps that produced 750 kbps of 1280x720, which is
// below the rate at which 720p is worth encoding at all, so the very
// first frame was already mush and every subsequent decision was made
// from a hole. The honest answer is resolution-derived — the rate at
// which the size the user actually asked for is comfortable — clamped
// into the configured envelope.
int VideoRateController::startBitrate() const {
    const Content c = content();
    const int edge = videorate::edgeForRung(c, m_maxLongEdge, 0);
    const int fps0 = videorate::fpsForRung(c, m_fps, 0);
    const int comfort = videorate::comfortKbpsFor(c, edge, fps0, m_codec);
    return qBound(m_minKbps, comfort, m_maxKbps);
}

int VideoRateController::blindCeiling() const {
    // Never below where the share started: a path that was proven
    // clean and then lost two reports must not be punished back down
    // past its own opening bitrate.
    return qMax(kBlindCeilingKbps, startBitrate());
}

void VideoRateController::resetState() {
    m_bitrate = startBitrate();
    m_rungIdx = 0;
    m_healthyTicks = m_comfortTicks = m_dwellTicks = m_kfRequests = 0;
    m_everGoverned = false;
    m_peers.clear();
    m_activeSinceMs = nowMs();
    m_wasBlind = false;
    m_lastLossPct = m_lastDamagePct = 0.0;
    m_kneeKbps = 0;
    m_kneeAtTick = 0;
    m_lastKneeStepTick = 0;
    m_cleanKbps = 0;
    m_cleanAtTick = 0;
    // Sender state is per share too: a machine that was encode-bound
    // on the last share (a 4K window, a busy moment) must get a fresh
    // chance at the full envelope on this one.
    m_lastWindow = videosend::Window();
    m_windowFresh = false;
    m_sentFpsEwma = m_captureFpsEwma = m_workMsEwma = 0.0;
    m_pktPerFrameEwma = 0.0;
    m_haveEwma = false;
    m_bitrateScale = 1.0;
    m_senderFpsCap = 0;
    m_senderEdgeStep = 0;
    m_fpsCapReason = Bottleneck::None;
    m_senderBadTicks = m_senderGoodTicks = m_senderDwellTicks = 0;
    m_senderUpWaitTicks = SP::kUpWaitTicks;
    m_ticksSinceSenderUp = m_ticksSinceSenderDown = 1 << 20;
    m_tickCount = 0;
}

void VideoRateController::setActive(bool active) {
    if (active == m_timer.isActive()) return;
    if (active) {
        // A fresh share NEVER inherits the previous one's end state.
        // In the field a share that had collapsed to 250 kbps / 720 px
        // reopened straight back into 250 kbps / 720 px, because the
        // only thing that reset the state was this branch and the
        // previous share had not passed through it.
        resetState();
        m_timer.start();
    } else {
        m_timer.stop();
    }
}

int VideoRateController::targetKbps() const {
    // The network allowance, scaled to the frame rate actually going
    // out (SenderPolicy). Never below the envelope minimum: a share
    // that is sending at all must be sending something decodable.
    const int scaled = int(double(m_bitrate) * m_bitrateScale);
    return qBound(m_minKbps, scaled, m_maxKbps);
}

int VideoRateController::maxKbps() const {
    // Headroom above target so transient scene complexity doesn't
    // immediately clip, without letting bursts double the budget.
    const int t = targetKbps();
    return qMin(m_maxKbps, t + t / 2);
}

int VideoRateController::longEdge() const {
    const int net = videorate::edgeForRung(content(), m_maxLongEdge, m_rungIdx);
    const int sender = qMax(160, int(double(m_maxLongEdge)
                        * videorate::kSenderEdgeScales[m_senderEdgeStep]) & ~1);
    return qMin(m_maxLongEdge, qMin(net, sender));
}

int VideoRateController::rungFps() const {
    return qMin(m_fps, videorate::fpsForRung(content(), m_fps, m_rungIdx));
}

int VideoRateController::fps() const {
    const int net = rungFps();
    return m_senderFpsCap > 0 ? qMin(net, m_senderFpsCap) : net;
}

void VideoRateController::reportSendWindow(const videosend::Window& w) {
    if (!w.valid) return;
    m_lastWindow = w;
    m_windowFresh = true;
    // Exponentially weighted, time constant ≈ 1.4 s (SenderPolicy).
    // Packets per frame only moves on windows that HAD frames: an
    // empty window says nothing about how big frames are.
    const double a = SP::kEwmaAlpha;
    if (!m_haveEwma) {
        m_sentFpsEwma = w.sentFps;
        m_captureFpsEwma = w.captureFps;
        m_workMsEwma = w.meanWorkMs;
        if (w.sentFps > 0.0) m_pktPerFrameEwma = w.packetsPerFrame;
        m_haveEwma = true;
        return;
    }
    m_sentFpsEwma += a * (w.sentFps - m_sentFpsEwma);
    m_captureFpsEwma += a * (w.captureFps - m_captureFpsEwma);
    if (w.sentFps > 0.0) {
        m_workMsEwma += a * (w.meanWorkMs - m_workMsEwma);
        m_pktPerFrameEwma = m_pktPerFrameEwma > 0.0
            ? m_pktPerFrameEwma + a * (w.packetsPerFrame - m_pktPerFrameEwma)
            : w.packetsPerFrame;
    }
}

void VideoRateController::reportDelivery(const QString& userId,
                                         const VideoDeliveryReport& r) {
    PeerSample& s = m_peers[userId];
    s.atMs = nowMs();
    s.report = r;
    if (r.governs()) {
        m_everGoverned = true;
        s.lostAcc = s.lostAcc * T::kLossEwmaDecay + double(r.lost);
        s.expectedAcc = s.expectedAcc * T::kLossEwmaDecay + double(r.expected);
    }
}

double VideoRateController::PeerSample::gradedLossPct() const {
    const double smoothed = expectedAcc > 0.0
        ? 100.0 * lostAcc / expectedAcc : report.lossPct();
    // A window that lost a real burst is graded on its own, now; one or
    // two stray packets only move the smoothed estimate.
    if (report.lost >= T::kMinLostForRaw)
        return std::max(smoothed, report.lossPct());
    return smoothed;
}

void VideoRateController::reportKeyframeRequest() {
    ++m_kfRequests;
}

VideoRateController::Health
VideoRateController::classify(qint64 now, QString& worstPeer,
                              double& worstLossPct, double& damagePct,
                              int kf) const {
    bool anyFresh = false;
    bool anyGovernor = false;
    bool anyLossNow = false;   // some governor lost packets THIS window
    double worst = -1.0;
    for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it) {
        if (now - it->atMs > kPeerSampleTtlMs) continue;
        anyFresh = true;
        // A peer that reports nothing usable (old client, or a window
        // with no packets in it) is not allowed to drag the rate down.
        if (!it->report.governs()) continue;
        anyGovernor = true;
        if (it->report.lost > 0) anyLossNow = true;
        const double loss = it->gradedLossPct();
        if (loss > worst) {
            worst = loss;
            worstPeer = it.key();
        }
    }

    if (!anyFresh) {
        return (now - m_activeSinceMs > kBlindGraceMs) ? Health::Blind
                                                       : Health::NoGovernor;
    }
    // Frame damage is monotonic in packet loss at a given frame size,
    // so the worst-loss peer is also the worst-damage peer.
    const double n = m_pktPerFrameEwma > 0.0 ? m_pktPerFrameEwma : 1.0;
    if (kf >= T::kKeyframeStorm) {
        worstLossPct = std::max(worst, 0.0);
        damagePct = videorate::frameDamagePct(worstLossPct, n);
        return Health::Cut;
    }
    if (!anyGovernor) return Health::NoGovernor;

    worstLossPct = worst;
    // The thresholds grade the fraction of FRAMES the viewer loses, not
    // packets: see videorate::frameDamagePct for why, and the field
    // numbers that made it matter.
    damagePct = videorate::frameDamagePct(worst, n);
    if (damagePct >= T::kTrimPct)
        // Back off only on loss in THIS window; the smoothed tail of an
        // earlier burst can hold, not cut (Thresholds, loss smoothing).
        return !anyLossNow ? Health::Hold
             : damagePct >= T::kCutPct ? Health::Cut : Health::Trim;
    if (damagePct >= T::kHealthyPct) return Health::Hold;
    return Health::Healthy;
}

// The frame-rate-aware half of the controller. Runs every tick, before
// the network law, so the probe gate below sees this tick's scale.
void VideoRateController::evaluateSender() {
    ++m_ticksSinceSenderUp;
    ++m_ticksSinceSenderDown;
    if (m_senderDwellTicks > 0) --m_senderDwellTicks;

    const bool fresh = m_windowFresh;
    m_windowFresh = false;
    if (!fresh || !m_lastWindow.valid) {
        // No window this tick (no source, or the share is not producing
        // frames at all): keep the previous decision rather than
        // snapping the scale back to 1.0 and the bitrate with it.
        return;
    }
    const videosend::Window& w = m_lastWindow;
    const Bottleneck b = w.bottleneck();
    const double asked = double(qMax(1, w.targetFps));

    // ---- Bitrate follows the frame rate actually sent --------------
    //
    // Two factors. The cap factor: m_bitrate is an allowance for the
    // network rung's frame rate, and a sender cap below it sends fewer
    // frames. The measured factor: only on POSITIVE evidence the sender
    // is short (not Idle — a still desktop sends few frames, and
    // starving its next burst of motion would be wrong).
    double scale = double(fps()) / double(qMax(1, rungFps()));
    const bool senderShort = b == Bottleneck::Encode
        || b == Bottleneck::Capture || b == Bottleneck::Cadence;
    if (senderShort) {
        const double measured = std::clamp(m_sentFpsEwma / asked,
                                           SP::kMinBitrateScale, 1.0);
        scale *= measured;
    }
    scale = std::floor(scale / SP::kBitrateScaleStep + 1e-9)
          * SP::kBitrateScaleStep;
    scale = std::clamp(scale, SP::kMinBitrateScale, 1.0);
    if (std::abs(scale - m_bitrateScale) > 1e-9) {
        qCInfo(logVideoRate, "[%d] sender %s: sent %.1f of %d fps — bitrate "
               "scale %.2f→%.2f (%d kbps of %d allowed)",
               int(m_streamId), videosend::bottleneckName(b), m_sentFpsEwma,
               w.targetFps, m_bitrateScale, scale,
               qBound(m_minKbps, int(double(m_bitrate) * scale), m_maxKbps),
               m_bitrate);
        m_bitrateScale = scale;
    }

    // ---- Caps: shed load when the sender demonstrably cannot keep up
    const bool deficit = (b == Bottleneck::Encode || b == Bottleneck::Capture)
        && m_sentFpsEwma < SP::kDeficitRatio * asked;
    if (deficit) {
        m_senderGoodTicks = 0;
        if (++m_senderBadTicks < SP::kDownTicks || m_senderDwellTicks > 0)
            return;
        const int beforeEdge = longEdge();
        const int beforeFps = fps();
        if (b == Bottleneck::Encode
            && m_senderEdgeStep < videorate::kSenderEdgeSteps - 1) {
            // Encode cost tracks pixels: shed resolution, keep motion.
            ++m_senderEdgeStep;
        } else {
            // Capture-bound (pixels cannot help: the capturer grabs at
            // source size), or encode-bound with no resolution left.
            // Ask for what was actually achieved, snapped to a short
            // list so a wobbling measurement cannot rebuild the encoder
            // session every time it moves.
            const double achieved = b == Bottleneck::Capture
                ? std::min(m_captureFpsEwma, m_sentFpsEwma * 1.1)
                : m_sentFpsEwma;
            const int cap = qMin(videorate::snapFpsDown(achieved),
                                 videorate::snapFpsDown(double(fps()) - 1.0));
            m_senderFpsCap = qMax(5, cap);
            m_fpsCapReason = b;
        }
        m_senderBadTicks = 0;
        m_senderDwellTicks = SP::kDwellTicks;
        // A downshift soon after an upshift means the upshift did not
        // fit: wait twice as long before trying again.
        if (m_ticksSinceSenderUp <= SP::kFailedUpWindowTicks)
            m_senderUpWaitTicks = qMin(SP::kUpWaitMaxTicks,
                                       m_senderUpWaitTicks * 2);
        m_ticksSinceSenderDown = 0;
        // Warning, not info: this is the line a person reading a log
        // after "it was choppy" needs to find, and it fires at most once
        // per dwell (10 s).
        qCWarning(logVideoRate,
                 "[%d] sender cannot keep up (%s-bound: %s) — %d px @ %d fps "
                 "→ %d px @ %d fps",
                 int(m_streamId), videosend::bottleneckName(b),
                 qPrintable(w.describe()), beforeEdge, beforeFps,
                 longEdge(), fps());
        return;
    }
    m_senderBadTicks = 0;

    if (m_senderFpsCap == 0 && m_senderEdgeStep == 0) {
        // Fully recovered; after a minute at the top, forget any backoff
        // a failed upshift earned.
        if (m_ticksSinceSenderDown > SP::kUpWaitMaxTicks)
            m_senderUpWaitTicks = SP::kUpWaitTicks;
        m_senderGoodTicks = 0;
        return;
    }

    // ---- Upshift: only on predicted headroom, sustained ------------
    //
    // Restore frame rate before resolution: smooth over sharp.
    bool headroom = b == Bottleneck::None;
    int nextFps = 0;
    if (headroom && m_senderFpsCap > 0) {
        nextFps = videorate::nextFpsUp(m_senderFpsCap);
        if (m_fpsCapReason == Bottleneck::Encode) {
            // Per-frame work is unchanged by the rate; it has to fit the
            // shorter interval.
            headroom = m_workMsEwma < SP::kUpHeadroom * 1000.0 / double(nextFps);
        } else {
            // A poller that is keeping up with the cap tells us nothing
            // about the rate above it; the wait IS the probe budget, and
            // the backoff bounds what a failed probe costs.
            headroom = m_captureFpsEwma >= videosend::Window::kMeetingRatio
                                            * double(m_senderFpsCap);
        }
    } else if (headroom) {
        // Edge step only: predict the next size's work from this one's,
        // scaled by pixel count.
        const double r = videorate::kSenderEdgeScales[m_senderEdgeStep - 1]
                       / videorate::kSenderEdgeScales[m_senderEdgeStep];
        headroom = m_workMsEwma * r * r
                   < SP::kUpHeadroom * 1000.0 / double(qMax(1, fps()));
    }
    if (!headroom) { m_senderGoodTicks = 0; return; }
    if (++m_senderGoodTicks < m_senderUpWaitTicks || m_senderDwellTicks > 0)
        return;

    const int beforeEdge = longEdge();
    const int beforeFps = fps();
    if (m_senderFpsCap > 0) {
        m_senderFpsCap = nextFps >= rungFps() ? 0 : nextFps;
        if (m_senderFpsCap == 0) m_fpsCapReason = Bottleneck::None;
    } else {
        --m_senderEdgeStep;
    }
    m_senderGoodTicks = 0;
    m_senderDwellTicks = SP::kDwellTicks;
    m_ticksSinceSenderUp = 0;
    qCInfo(logVideoRate, "[%d] sender headroom — %d px @ %d fps → %d px @ %d fps",
           int(m_streamId), beforeEdge, beforeFps, longEdge(), fps());
}

void VideoRateController::logSendWindow() const {
    qCInfo(logVideoRate,
           "[%d] send: %s | net: loss %.2f%% → %.1f%% of frames (%.0f pkt/frame) "
           "| allowance %d kbps × %.2f = %d kbps | knee %d | caps: %s fps, "
           "edge step %d | out %d px @ %d fps",
           int(m_streamId), qPrintable(m_lastWindow.describe()),
           m_lastLossPct, m_lastDamagePct,
           m_pktPerFrameEwma > 0.0 ? m_pktPerFrameEwma : 1.0,
           m_bitrate, m_bitrateScale, targetKbps(), m_kneeKbps,
           m_senderFpsCap > 0 ? qPrintable(QString::number(m_senderFpsCap))
                              : "no",
           m_senderEdgeStep, longEdge(), fps());
}

void VideoRateController::applyLadder() {
    const Content c = content();
    const int floorHere =
        videorate::rungMinKbps(c, m_maxLongEdge, m_fps, m_rungIdx, m_codec);

    if (m_bitrate < floorHere) {
        if (m_rungIdx >= videorate::kLadderRungs - 1) return;  // bottom
        ++m_rungIdx;
        m_comfortTicks = 0;
        m_dwellTicks = T::kDwellTicks;
        qCInfo(logVideoRate, "[%d] quality down → %d px @ %d fps",
              int(m_streamId), longEdge(), fps());
        return;
    }
    if (m_rungIdx == 0) { m_comfortTicks = 0; return; }
    // Minimum dwell after any downshift: without it a bitrate sitting
    // on a rung boundary walks the ladder up and down every second.
    if (m_dwellTicks > 0) { m_comfortTicks = 0; return; }

    const int upIdx = m_rungIdx - 1;
    const int comfortThere = videorate::comfortKbpsFor(
        c, videorate::edgeForRung(c, m_maxLongEdge, upIdx),
        videorate::fpsForRung(c, m_fps, upIdx), m_codec);
    if (m_bitrate < comfortThere) { m_comfortTicks = 0; return; }
    if (++m_comfortTicks < T::kUpshiftTicks) return;

    m_rungIdx = upIdx;
    m_comfortTicks = 0;
    qCInfo(logVideoRate, "[%d] quality up → %d px @ %d fps",
          int(m_streamId), longEdge(), fps());
}

void VideoRateController::tick() {
    const qint64 now = nowMs();
    const int kf = m_kfRequests;
    m_kfRequests = 0;
    ++m_tickCount;

    const int beforeTarget = targetKbps();
    const int beforeEdge = longEdge();
    const int beforeFps = fps();

    // Sender first: its window is pulled at the same instant the
    // decision is made, and its scale gates the probe below.
    if (m_sendSource) reportSendWindow(m_sendSource(fps()));
    evaluateSender();

    QString worstPeer;
    double worstLoss = 0.0;
    double damage = 0.0;
    const Health h = classify(now, worstPeer, worstLoss, damage, kf);
    m_lastLossPct = worstLoss;
    m_lastDamagePct = damage;
    // A person-readable picture every 5 s (10 ticks) while verbose
    // logging is on: the one line that says which of capture, encode or
    // network is costing frames.
    if (m_lastWindow.valid && m_tickCount % 10 == 0) logSendWindow();

    if (h == Health::Blind) {
        if (!m_wasBlind) {
            m_wasBlind = true;
            qCWarning(logVideoRate,
                     "[%d] no delivery reports from any peer — holding at "
                     "%d kbps%s",
                     int(m_streamId), m_bitrate,
                     m_everGoverned ? "" : " (blind ceiling applies)");
        }
        m_healthyTicks = 0;
        m_comfortTicks = 0;
        // The ceiling is for a path that has NEVER answered. Once a
        // peer has reported real numbers we know what the path does,
        // so a gap in reporting means "hold", not "descend".
        if (!m_everGoverned) {
            const int ceiling = blindCeiling();
            if (m_bitrate > ceiling) {
                m_bitrate = qBound(m_minKbps, ceiling, m_maxKbps);
                emit forceKeyframe();   // resync receivers at the new rate
            }
        }
        return;
    }
    if (m_wasBlind) {
        m_wasBlind = false;
        qCInfo(logVideoRate, "[%d] delivery reports resumed", int(m_streamId));
    }

    const int before = m_bitrate;
    if (m_dwellTicks > 0) --m_dwellTicks;

    // Knee expiry: exceeded cleanly, or simply old.
    if (m_kneeKbps > 0
        && (m_tickCount - m_kneeAtTick > T::kKneeMemoryTicks
            || double(m_bitrate) > T::kKneeForgetAbove * double(m_kneeKbps))) {
        m_kneeKbps = 0;
    }

    if (m_cleanKbps > 0 && m_tickCount - m_cleanAtTick > T::kKneeMemoryTicks)
        m_cleanKbps = 0;

    // Record the knee at a trim or cut. Back-to-back loss events keep
    // the FIRST rate that failed: the second one's starting point is
    // already a consequence of the first reduction, not a new
    // measurement of the path. ("Back-to-back" = within two ticks.)
    auto noteKnee = [&]() {
        m_lastKneeStepTick = m_tickCount;
        // Loss below a rate that was clean moments ago is a burst, not
        // the wall: cut for it, learn nothing from it.
        if (m_cleanKbps > 0
            && double(m_bitrate) < T::kKneeNeedsCleanRatio * double(m_cleanKbps))
            return;
        if (m_kneeKbps == 0 || m_tickCount - m_kneeAtTick > 2
            || m_bitrate > m_kneeKbps)
            m_kneeKbps = m_bitrate;
        m_kneeAtTick = m_tickCount;
    };

    switch (h) {
    case Health::Healthy: {
        ++m_healthyTicks;
        // App-limited: while the sender is the bottleneck we are not
        // sending m_bitrate, so a clean report says nothing about it.
        if (m_bitrateScale < 0.95) break;
        if (m_bitrate >= m_cleanKbps) {
            m_cleanKbps = m_bitrate;
            m_cleanAtTick = m_tickCount;
        }
        if (m_healthyTicks < T::kHealthyTicksBeforeProbe) break;
        const double kneeBand = double(m_kneeKbps) * T::kKneeBand;
        if (m_kneeKbps > 0 && double(m_bitrate) >= kneeBand) {
            // Near where loss last began: creep, one step per 4 s.
            if (m_tickCount - m_lastKneeStepTick >= T::kKneeProbeEveryTicks) {
                m_bitrate = int(double(m_bitrate) * T::kKneeProbeFactor) + 1;
                m_lastKneeStepTick = m_tickCount;
            }
            break;
        }
        const double factor =
            double(m_bitrate) < T::kFastProbeBelow * double(m_maxKbps)
                ? T::kFastProbeFactor : T::kProbeFactor;
        int next = int(double(m_bitrate) * factor) + T::kProbeFloorKbps;
        // A sprint lands at the edge of the knee band, never past it.
        if (m_kneeKbps > 0 && double(next) > kneeBand)
            next = qMax(m_bitrate + 1, int(kneeBand));
        m_bitrate = next;
        break;
    }
    case Health::Hold:
    case Health::NoGovernor:
        // Evidence of mild loss, or no evidence at all: stop climbing,
        // change nothing else. An old peer that sends reports without
        // the packet counters lands here — reading its silence as
        // 100 % loss would collapse the share for every other viewer.
        m_healthyTicks = 0;
        m_comfortTicks = 0;
        break;
    case Health::Trim:
        noteKnee();
        m_bitrate = int(double(m_bitrate) * T::kTrimFactor);
        m_healthyTicks = 0;
        m_comfortTicks = 0;
        m_dwellTicks = qMax(m_dwellTicks, T::kDwellTicks / 2);
        break;
    case Health::Cut:
        noteKnee();
        m_bitrate = int(double(m_bitrate) * T::kCutFactor);
        m_healthyTicks = 0;
        m_comfortTicks = 0;
        m_dwellTicks = T::kDwellTicks;
        break;
    case Health::Blind:
        break;                      // handled above
    }

    // The hard floor is the bottom rung's floor: below it there is no
    // size left to trade, and sending fewer bits than that produces a
    // picture nobody can use.
    const int hardFloor = qBound(
        m_minKbps,
        videorate::rungMinKbps(content(), m_maxLongEdge, m_fps,
                               videorate::kLadderRungs - 1, m_codec),
        m_maxKbps);
    m_bitrate = qBound(hardFloor, m_bitrate, m_maxKbps);
    applyLadder();
    m_bitrate = qBound(hardFloor, m_bitrate, m_maxKbps);

    if (m_bitrate != before || targetKbps() != beforeTarget
        || longEdge() != beforeEdge || fps() != beforeFps) {
        static const char* kNames[] = {"blind", "no-governor", "healthy",
                                       "hold", "trim", "cut"};
        qCInfo(logVideoRate,
              "[%d] %s: loss=%.2f%% frames=%.1f%% kf=%d peers=%d governor=%s "
              "bitrate %d→%d kbps (out %d, knee %d) %d px @ %d fps",
              int(m_streamId), kNames[int(h)], worstLoss, damage, kf,
              int(m_peers.size()),
              worstPeer.isEmpty() ? "(none)" : qPrintable(worstPeer),
              before, m_bitrate, targetKbps(), m_kneeKbps, longEdge(), fps());
        // Deliberately NO forceKeyframe on a back-off. An IDR is the
        // largest frame the encoder can emit — blasting one at the
        // exact moment the path is congested is how keyframe storms
        // start. Receivers that actually lost data request their own,
        // and a ladder change rebuilds the encode session, which opens
        // on an IDR anyway.
    }
}
