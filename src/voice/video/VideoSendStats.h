#pragma once

#include <QString>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

// Send-side instrumentation: what the SENDER actually achieved, as
// opposed to what the receiver reports about the path.
//
// ---- Why this exists (the "sharp but choppy" report, 2026-09-21) ----
//
// "Choppy" has three causes that need opposite fixes, and until this
// header nothing on the send side could tell them apart:
//
//   (a) the sender cannot produce frames at the configured rate —
//       capture or encode is the bottleneck. Cutting bitrate does
//       little; fewer pixels or a frame rate the machine can sustain
//       does.
//   (b) the path loses packets. With no NACK/RTX, the receiver drops
//       the whole access unit on ANY hole and freezes until the next
//       IDR (VideoReceivePipeline, "loss-suspect AU dropped"), so what
//       matters is not the packet-loss rate but how many packets each
//       frame is spread over. See videorate::frameDamagePct().
//   (c) the receiver presents badly — not a send-side question.
//
// The field log from the report settled which one dominated. Screen
// share, HEVC, 1920x1080 source, 30 fps configured:
//   * capture delivered 27.7 frames/s, but 105 of 303 sampled push
//     ticks found NO new frame (two free-running 33 ms timers — the
//     capture poller's and the push throttle's — beat against each
//     other, and completions land in clumps), so roughly 19-20 fps of
//     the 30 were actually sent, unevenly. Cause (a), cadence flavour.
//   * over nine minutes the rate controller logged 486 decisions: 276
//     probes (134 of them on ticks that HAD loss, mean 0.42 %, which
//     the law calls healthy), 93 trims and 117 cuts — a sawtooth
//     between ~9 and 75 Mbps. At 30 Mbps / 20 fps a frame is ~150 RTP
//     packets, so 1 % packet loss damages ~78 % of frames. The viewer
//     asked for a keyframe on 62 % of those decisions' ticks — frozen
//     waiting for an IDR much of the time, and the frames it did show
//     were sharp. Cause (b), amplified by frame size.
//
// Nothing here is telemetry: every number stays in this process, is
// logged locally under bsfchat.video.rate (and [screenshare]), and is
// exposed read-only on ScreenShareController::sendStats for a local
// debug surface.

namespace videosend {

// RTP payload bytes per packet. libdatachannel's packetizers fragment
// at RTC_DEFAULT_MAX_FRAGMENT_SIZE = RTC_DEFAULT_MTU (1280, the IPv6
// minimum) - 12 (RTP) - 8 (UDP) - 40 (IPv6) = 1220. We do not pass a
// different size to H264RtpPacketizer / H265RtpPacketizer.
inline constexpr int kRtpPayloadBytes = 1220;

inline quint64 packetsFor(qint64 accessUnitBytes) {
    if (accessUnitBytes <= 0) return 0;
    return quint64((accessUnitBytes + kRtpPayloadBytes - 1) / kRtpPayloadBytes);
}

// Cumulative counters, sampled from their owners at each evaluation.
// Every field only ever increases for the life of its owner, so a
// window is a plain difference; a counter that goes backwards (owner
// rebuilt) re-seeds the Accumulator instead of producing a window.
struct Counters {
    // Capture side (ScreenShareController / CameraController).
    quint64 captured = 0;       // frames the capturer handed us
    quint64 overwritten = 0;    // replaced before any push consumed them
    quint64 pushTicks = 0;      // push opportunities
    quint64 emptyTicks = 0;     // push opportunities with no new frame
    // Encode side (VideoSendPipeline).
    quint64 submitted = 0;      // frames handed to the encode worker
    quint64 superseded = 0;     // replaced in the depth-1 slot, never encoded
    quint64 encoded = 0;        // access units out of the encoder
    quint64 bytes = 0;          // their total size
    quint64 packets = 0;        // RTP packets they fragment into
    quint64 keyframes = 0;
    quint64 workUs = 0;         // convert + encode time, summed
    quint32 maxWorkUs = 0;      // worst single frame since the last sample
};

// Which part of the sender is failing to keep up, if any. Classified
// only from POSITIVE evidence: a static screen legitimately produces
// few frames on capture paths that deliver on change, and a controller
// that read "few frames" as "overloaded" would shed quality from every
// idle desktop share.
enum class Bottleneck {
    None,      // meeting the configured rate
    Idle,      // below rate, but nothing says the sender was the reason
    Capture,   // a polling capturer delivered fewer frames than asked
    Cadence,   // frames arrived but were overwritten before a push
    Encode,    // the encode worker could not keep up
};

inline const char* bottleneckName(Bottleneck b) {
    switch (b) {
    case Bottleneck::None:    return "none";
    case Bottleneck::Idle:    return "idle";
    case Bottleneck::Capture: return "capture";
    case Bottleneck::Cadence: return "cadence";
    case Bottleneck::Encode:  return "encode";
    }
    return "?";
}

// One evaluation window of send-side rates.
struct Window {
    bool valid = false;
    double seconds = 0.0;
    int targetFps = 0;          // the rate the sender was asked for
    // True when the capturer POLLS (macOS: one screenshot per timer
    // tick) — there, a missing frame is positive evidence that capture
    // was late or failed. On capture paths that deliver on change (Qt
    // QScreenCapture / QWindowCapture, cameras) a missing frame may just
    // mean nothing moved, and proves nothing.
    bool capturePolled = false;

    double captureFps = 0.0;
    double sentFps = 0.0;
    double overwrittenPerSec = 0.0;
    double emptyTickFraction = 0.0;
    double supersededPerSec = 0.0;
    double meanWorkMs = 0.0;
    double maxWorkMs = 0.0;
    double sentKbps = 0.0;
    double packetsPerFrame = 0.0;
    int keyframes = 0;

    // Thresholds (fractions of the target rate). Kept next to the
    // classification so the numbers and the judgement cannot drift.
    //  - kMeetingRatio: at or above this the sender is keeping up.
    //    0.9, not 1.0: timer slop (QTimer's coarse default is ±5 %)
    //    and window edges alone cost a few percent.
    //  - kEvidenceRatio: how much of the frame rate a symptom must
    //    account for before it is blamed. 10 % of 30 fps is 3 frames
    //    a second — well above one-off noise, well below "obvious".
    //  - kWorkBudget: mean convert+encode time at or above this share
    //    of the frame interval means the worker is saturated even if
    //    the depth-1 slot has not overflowed yet.
    static constexpr double kMeetingRatio = 0.90;
    static constexpr double kEvidenceRatio = 0.10;
    static constexpr double kWorkBudget = 0.90;

    // What this deliberately does NOT see: the RTP pacer. `sentFps`
    // counts access units leaving the ENCODER; the pacer sits after it
    // and never blocks it (a full pacer queue drops its own oldest
    // packets instead), so a starved pacer cannot masquerade as an
    // encode-bound sender and cost resolution for nothing. Its effect
    // reaches the controller the way network loss does — as holes in
    // the receiver reports — and the pacer logs its own drops and
    // queueing (PacedRtpSender, bsfchat.voice).
    Bottleneck bottleneck() const {
        if (!valid || targetFps <= 0) return Bottleneck::None;
        const double target = double(targetFps);
        if (sentFps >= kMeetingRatio * target) return Bottleneck::None;
        const double evidence = kEvidenceRatio * target;
        const double intervalMs = 1000.0 / target;
        // Order matters: encode pressure also starves the push path,
        // so it is checked first and wins.
        if (supersededPerSec >= evidence
            || (sentFps > 0.0 && meanWorkMs >= kWorkBudget * intervalMs))
            return Bottleneck::Encode;
        if (overwrittenPerSec >= evidence) return Bottleneck::Cadence;
        if (capturePolled && captureFps < kMeetingRatio * target)
            return Bottleneck::Capture;
        return Bottleneck::Idle;
    }

    // One line a person can read in a log without a decoder ring.
    QString describe() const {
        if (!valid) return QStringLiteral("(no send window yet)");
        return QStringLiteral(
                   "target %1 fps | capture %2 fps | sent %3 fps | "
                   "overwritten %4/s | empty ticks %5%% | encoder slot "
                   "drops %6/s | work %7 ms (max %8) | %9 kbps | "
                   "%10 pkt/frame | kf %11 | bottleneck=%12")
            .arg(targetFps)
            .arg(captureFps, 0, 'f', 1)
            .arg(sentFps, 0, 'f', 1)
            .arg(overwrittenPerSec, 0, 'f', 1)
            .arg(int(std::lround(emptyTickFraction * 100.0)))
            .arg(supersededPerSec, 0, 'f', 1)
            .arg(meanWorkMs, 0, 'f', 1)
            .arg(maxWorkMs, 0, 'f', 1)
            .arg(int(std::lround(sentKbps)))
            .arg(packetsPerFrame, 0, 'f', 1)
            .arg(keyframes)
            .arg(QLatin1String(bottleneckName(bottleneck())));
    }
};

// Differences successive counter samples into windows. Pure: the
// caller supplies the clock, so tests drive it deterministically.
class Accumulator {
public:
    // Returns an invalid window for the first sample (it only seeds)
    // and for a sample too close to the previous one to mean anything.
    Window sample(const Counters& c, qint64 nowMs, int targetFps,
                  bool capturePolled) {
        Window w;
        w.targetFps = targetFps;
        w.capturePolled = capturePolled;
        if (!m_seeded || nowMs < m_atMs || backwards(c)
            || nowMs - m_atMs > kMaxWindowMs) {
            m_prev = c;
            m_atMs = nowMs;
            m_seeded = true;
            return w;
        }
        const qint64 dtMs = nowMs - m_atMs;
        if (dtMs < kMinWindowMs) return w;   // keep accumulating

        const double s = double(dtMs) / 1000.0;
        const quint64 enc = c.encoded - m_prev.encoded;
        const quint64 ticks = c.pushTicks - m_prev.pushTicks;
        w.valid = true;
        w.seconds = s;
        w.captureFps = double(c.captured - m_prev.captured) / s;
        w.sentFps = double(enc) / s;
        w.overwrittenPerSec = double(c.overwritten - m_prev.overwritten) / s;
        w.emptyTickFraction = ticks > 0
            ? double(c.emptyTicks - m_prev.emptyTicks) / double(ticks) : 0.0;
        w.supersededPerSec = double(c.superseded - m_prev.superseded) / s;
        w.meanWorkMs = enc > 0
            ? double(c.workUs - m_prev.workUs) / double(enc) / 1000.0 : 0.0;
        w.maxWorkMs = double(c.maxWorkUs) / 1000.0;
        w.sentKbps = double(c.bytes - m_prev.bytes) * 8.0 / double(dtMs);
        w.packetsPerFrame = enc > 0
            ? double(c.packets - m_prev.packets) / double(enc) : 0.0;
        w.keyframes = int(c.keyframes - m_prev.keyframes);

        m_prev = c;
        m_atMs = nowMs;
        return w;
    }

    void reset() { *this = Accumulator(); }

    // Shorter than this and a 30 fps window holds a handful of frames:
    // one late frame reads as a 20 % shortfall.
    static constexpr qint64 kMinWindowMs = 250;
    // Longer than this and the window spans a gap in sampling (the
    // controller was inactive — lossless tier, share restarting), not a
    // stretch of sending; its rates would describe the gap. Re-seed.
    static constexpr qint64 kMaxWindowMs = 2000;

private:
    bool backwards(const Counters& c) const {
        return c.captured < m_prev.captured || c.encoded < m_prev.encoded
            || c.submitted < m_prev.submitted || c.bytes < m_prev.bytes;
    }

    Counters m_prev;
    qint64 m_atMs = 0;
    bool m_seeded = false;
};

} // namespace videosend
