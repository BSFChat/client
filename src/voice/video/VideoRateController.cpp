#include "voice/video/VideoRateController.h"

#include <QLoggingCategory>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(logVideoRate, "bsfchat.video.rate", QtWarningMsg)

constexpr double VideoRateController::kScaleLadder[];

VideoRateController::VideoRateController(VideoStreamId streamId, QObject* parent)
    : QObject(parent)
    , m_streamId(streamId)
{
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &VideoRateController::tick);
}

void VideoRateController::setEnvelope(int minKbps, int maxKbps, int fps,
                                      int maxLongEdge) {
    m_minKbps = qMax(50, minKbps);
    m_maxKbps = qMax(m_minKbps, maxKbps);
    m_fps = qBound(1, fps, 60);
    m_maxLongEdge = qBound(160, maxLongEdge, 3840);
    m_bitrate = qBound(m_minKbps, m_bitrate, m_maxKbps);
}

void VideoRateController::setActive(bool active) {
    if (active == m_timer.isActive()) return;
    if (active) {
        // Fresh share: start mid-envelope rather than inheriting the
        // previous session's end state — but never above the blind
        // ceiling, since no delivery reports exist yet by definition.
        m_bitrate = qBound(m_minKbps,
                           qMin(m_maxKbps / 2, kBlindCeilingKbps), m_maxKbps);
        m_scaleIdx = 0;
        m_stableTicks = m_comfortTicks = m_kfRequests = 0;
        m_peerRatios.clear();
        m_activeSinceMs = QDateTime::currentMSecsSinceEpoch();
        m_wasBlind = false;
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
    const int edge = int(m_maxLongEdge * kScaleLadder[m_scaleIdx]);
    return qMax(160, edge & ~1);
}

void VideoRateController::reportDeliveryRatio(const QString& userId, double ratio) {
    PeerSample s;
    s.atMs = QDateTime::currentMSecsSinceEpoch();
    // Light EWMA per peer so one lucky window doesn't mask loss.
    const auto prev = m_peerRatios.constFind(userId);
    s.ratio = prev != m_peerRatios.constEnd()
        ? 0.5 * prev->ratio + 0.5 * ratio
        : ratio;
    m_peerRatios[userId] = s;
}

void VideoRateController::reportKeyframeRequest() {
    ++m_kfRequests;
}

double VideoRateController::worstRecentRatio() const {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    double worst = 1.0;
    for (const auto& s : m_peerRatios) {
        if (now - s.atMs > kPeerSampleTtlMs) continue;
        worst = std::min(worst, s.ratio);
    }
    return worst;
}

bool VideoRateController::hasFreshSamples(qint64 nowMs) const {
    for (const auto& s : m_peerRatios) {
        if (nowMs - s.atMs <= kPeerSampleTtlMs) return true;
    }
    return false;
}

double VideoRateController::bppAt(int longEdge, int kbps) const {
    // Assume 16:9-ish area for the pixel estimate; exactness doesn't
    // matter, the bands are heuristic.
    const double pixels = double(longEdge) * (double(longEdge) * 9.0 / 16.0);
    return (double(kbps) * 1000.0) / (pixels * double(m_fps));
}

void VideoRateController::tick() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Blind mode: nobody is telling us whether frames arrive (worst-
    // ratio would read a meaningless 1.0). Hold — never probe upward
    // on faith — and cap at a rate any plausible path carries. The
    // startup grace keeps the first reports from racing the clamp.
    if (!hasFreshSamples(now) && now - m_activeSinceMs > kBlindGraceMs) {
        if (!m_wasBlind) {
            m_wasBlind = true;
            qCWarning(logVideoRate,
                     "[%d] no delivery reports from any peer — holding ≤%d kbps",
                     int(m_streamId), kBlindCeilingKbps);
        }
        m_kfRequests = 0;
        m_stableTicks = 0;
        if (m_bitrate > kBlindCeilingKbps) {
            m_bitrate = qMax(m_minKbps, kBlindCeilingKbps);
            emit forceKeyframe();   // resync receivers at the new rate
        }
        return;
    }
    if (m_wasBlind) {
        m_wasBlind = false;
        qCInfo(logVideoRate, "[%d] delivery reports resumed", int(m_streamId));
    }

    const double ratio = worstRecentRatio();
    const int kf = m_kfRequests;
    m_kfRequests = 0;

    const int before = m_bitrate;
    bool backedOff = false;

    // Cut gently, recover briskly. The original −40 %/−15 % steps with
    // a +8 %-per-2 s climb made the bitrate saw-tooth hard enough that
    // the quality visibly pulsed on every WiFi loss blip; softer cuts
    // hold quality steadier and the receiver-side loss handling (drop
    // corrupt AUs, resync on IDR) now covers the actual artefacts.
    if (ratio < 0.90 || kf >= 3) {
        m_bitrate = int(m_bitrate * 0.75);
        backedOff = true;
        m_stableTicks = 0;
        m_comfortTicks = 0;
    } else if (ratio < 0.97) {
        m_bitrate = int(m_bitrate * 0.90);
        backedOff = true;
        m_stableTicks = 0;
        m_comfortTicks = 0;
    } else if (++m_stableTicks >= 2) {
        m_bitrate = int(m_bitrate * 1.08) + 50;
        m_stableTicks = 0;
    }
    m_bitrate = qBound(m_minKbps, m_bitrate, m_maxKbps);

    // Resolution ladder. Downshift when the floor bpp is unreachable
    // at this size; upshift after sustained comfort at the next size.
    if (m_scaleIdx < 4 && bppAt(longEdge(), m_bitrate) < kBppFloor) {
        ++m_scaleIdx;
        backedOff = true;
        m_comfortTicks = 0;
        qCInfo(logVideoRate, "[%d] resolution down → %d px long edge",
              int(m_streamId), longEdge());
    } else if (m_scaleIdx > 0) {
        const int upEdge = qMax(160, int(m_maxLongEdge
                                * kScaleLadder[m_scaleIdx - 1]) & ~1);
        if (bppAt(upEdge, m_bitrate) > kBppComfort) {
            if (++m_comfortTicks >= 10) {
                --m_scaleIdx;
                m_comfortTicks = 0;
                qCInfo(logVideoRate, "[%d] resolution up → %d px long edge",
                      int(m_streamId), longEdge());
            }
        } else {
            m_comfortTicks = 0;
        }
    }

    if (backedOff) {
        qCInfo(logVideoRate,
              "[%d] back-off: ratio=%.3f kf=%d bitrate %d→%d kbps edge=%d",
              int(m_streamId), ratio, kf, before, m_bitrate, longEdge());
        // Deliberately NO forceKeyframe here. An IDR is the largest
        // frame the encoder can emit — blasting one at the exact
        // moment the path is congested is how keyframe storms start
        // (IDR burst → AP queue drops part of it → receiver requests
        // another IDR → repeat). Receivers that actually lost data
        // request their own keyframe via keyframeNeeded/PLI, and a
        // resolution-ladder change rebuilds the encode session, which
        // opens on an IDR anyway.
    }
}
