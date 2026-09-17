#include "voice/video/VideoRateController.h"

#include <QLoggingCategory>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(logVideoRate, "bsfchat.video.rate", QtWarningMsg)

using videorate::Content;
using T = videorate::Thresholds;

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

int VideoRateController::maxKbps() const {
    // Headroom above target so transient scene complexity doesn't
    // immediately clip, without letting bursts double the budget.
    return qMin(m_maxKbps, m_bitrate + m_bitrate / 2);
}

int VideoRateController::longEdge() const {
    return qMin(m_maxLongEdge,
                videorate::edgeForRung(content(), m_maxLongEdge, m_rungIdx));
}

int VideoRateController::fps() const {
    return qMin(m_fps, videorate::fpsForRung(content(), m_fps, m_rungIdx));
}

void VideoRateController::reportDelivery(const QString& userId,
                                         const VideoDeliveryReport& r) {
    PeerSample s;
    s.atMs = nowMs();
    s.report = r;
    m_peers[userId] = s;
    if (r.governs()) m_everGoverned = true;
}

void VideoRateController::reportKeyframeRequest() {
    ++m_kfRequests;
}

VideoRateController::Health
VideoRateController::classify(qint64 now, QString& worstPeer,
                              double& worstLossPct, int kf) const {
    bool anyFresh = false;
    bool anyGovernor = false;
    double worst = -1.0;
    for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it) {
        if (now - it->atMs > kPeerSampleTtlMs) continue;
        anyFresh = true;
        // A peer that reports nothing usable (old client, or a window
        // with no packets in it) is not allowed to drag the rate down.
        if (!it->report.governs()) continue;
        anyGovernor = true;
        const double loss = it->report.lossPct();
        if (loss > worst) {
            worst = loss;
            worstPeer = it.key();
        }
    }

    if (!anyFresh) {
        return (now - m_activeSinceMs > kBlindGraceMs) ? Health::Blind
                                                       : Health::NoGovernor;
    }
    if (kf >= T::kKeyframeStorm) {
        worstLossPct = std::max(worst, 0.0);
        return Health::Cut;
    }
    if (!anyGovernor) return Health::NoGovernor;

    worstLossPct = worst;
    if (worst >= T::kCutPct) return Health::Cut;
    if (worst >= T::kTrimPct) return Health::Trim;
    if (worst >= T::kHealthyPct) return Health::Hold;
    return Health::Healthy;
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

    QString worstPeer;
    double worstLoss = 0.0;
    const Health h = classify(now, worstPeer, worstLoss, kf);

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
    const int beforeEdge = longEdge();
    const int beforeFps = fps();
    if (m_dwellTicks > 0) --m_dwellTicks;

    switch (h) {
    case Health::Healthy:
        if (++m_healthyTicks >= T::kHealthyTicksBeforeProbe) {
            const double factor =
                double(m_bitrate) < T::kFastProbeBelow * double(m_maxKbps)
                    ? T::kFastProbeFactor : T::kProbeFactor;
            m_bitrate = int(double(m_bitrate) * factor) + T::kProbeFloorKbps;
        }
        break;
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
        m_bitrate = int(double(m_bitrate) * T::kTrimFactor);
        m_healthyTicks = 0;
        m_comfortTicks = 0;
        m_dwellTicks = qMax(m_dwellTicks, T::kDwellTicks / 2);
        break;
    case Health::Cut:
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

    if (m_bitrate != before || longEdge() != beforeEdge || fps() != beforeFps) {
        static const char* kNames[] = {"blind", "no-governor", "healthy",
                                       "hold", "trim", "cut"};
        qCInfo(logVideoRate,
              "[%d] %s: loss=%.2f%% kf=%d peers=%d governor=%s "
              "bitrate %d→%d kbps %d px @ %d fps",
              int(m_streamId), kNames[int(h)], worstLoss, kf,
              int(m_peers.size()),
              worstPeer.isEmpty() ? "(none)" : qPrintable(worstPeer),
              before, m_bitrate, longEdge(), fps());
        // Deliberately NO forceKeyframe on a back-off. An IDR is the
        // largest frame the encoder can emit — blasting one at the
        // exact moment the path is congested is how keyframe storms
        // start. Receivers that actually lost data request their own,
        // and a ladder change rebuilds the encode session, which opens
        // on an IDR anyway.
    }
}
