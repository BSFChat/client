#pragma once

#include "voice/video/VideoCodec.h"
#include "voice/video/VideoDeliveryReport.h"
#include "voice/video/VideoRatePolicy.h"
#include "voice/video/VideoSendStats.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

// The "always smooth" brain. One per outgoing stream, evaluating every
// 500 ms. Moves QUALITY (bitrate, then fps or resolution per content
// type) instead of ever letting latency build: the pipelines are
// depth-1 latest-wins, so the only way congestion can manifest is as
// measured loss — this controller closes that loop.
//
// ---- Why this was rewritten (S-17) --------------------------------
//
// The previous law graded the path on a DELIVERED/SENT BYTE RATIO and
// backed off below 0.97. That ratio cannot read 1.0 on a real link,
// ever: the two ends sample their byte counters at different instants
// and the encoder's output is bursty (an IDR is 10-20x a P-frame), so
// the numerator is always missing whatever is in flight. Over one
// field session on a lossless LAN it averaged 0.808 — 2798 of 4069
// samples below 0.90, four above 0.97 — so the controller had no
// upward path at all. It reached the 250 kbps floor in seconds and
// stayed there for the whole session while the user, who had
// configured 50 Mbps, watched 720x404 at 28 kbps and reported
// "pixelation is pretty bad even with the highest settings".
//
// So the input changed. The receiver counts PACKETS from RTP sequence
// numbers (RtpSeqTracker) and reports lost/expected; loss is a real
// physical quantity that reads exactly 0 on a clean LAN, so "healthy"
// is now a state the controller can actually observe and probe from.
// Byte goodput survives only as a secondary signal.
//
// ---- Control law ---------------------------------------------------
//
//   damage < 2 %              → probe up (×1.02, ×1.04 while far below
//                               the configured max), never past a
//                               margin over what is actually being sent
//   2 % ≤ damage < 6 %        → hold
//   damage ≥ 6 %, persistent  → back off by 2 × packet loss, clamped to
//     (or kf-storm)             3-15 %, never below 70 % of recent
//                               goodput; 4 s with no ladder upshift
//   no peer reports loss      → hold (old client — must NOT read as
//                               100 % loss, and must not license a
//                               climb on no evidence either)
//
// ("damage" = the fraction of frames that lose a packet; see below.)
//
// Thresholds, floors and the fps/resolution ladder all live in
// VideoRatePolicy.h so they can be reasoned about without a Qt event
// loop. The WORST governing peer sets the rate — a share is only as
// smooth as its worst receiver — and the log line names it.
//
// ---- Frame-rate awareness (2026-09-21, "sharp but very choppy") ----
//
// Three changes, each answering a specific line of the field log that
// accompanied the report (numbers in VideoSendStats.h):
//
//  1. The loss the law grades is FRAME damage, 1-(1-p)^n with n the
//     measured packets per frame, not raw packet loss p — the receiver
//     freezes on any hole, so a 150-packet frame at 0.5 % loss is lost
//     more often than not. Same thresholds, now in frames.
//  2. A KNEE is remembered where loss last began, and the probe creeps
//     near it instead of charging it every second (the sawtooth).
//  3. The SENDER's achieved frame rate is a second input
//     (reportSendWindow). A sender that cannot keep up sheds pixels
//     (encode-bound) or asks for a rate it can sustain (capture-bound),
//     and the bitrate is scaled by sent/asked fps so bytes per frame —
//     and so fragility — stay constant as the frame rate falls.
//
// How the inputs combine: each one only ever LOWERS the outputs, and
// every output is the minimum over what each input allows, all inside
// the user/server envelope:
//
//   targetKbps = network allowance × sender fps scale      (≤ envelope)
//   fps        = min(envelope, network rung fps, sender fps cap)
//   longEdge   = min(envelope, network rung edge, sender edge cap)
//
// The network allowance does NOT probe upward while the sender is the
// limit (scale < 0.95): bits we are not sending prove nothing about the
// path — the "app-limited" rule every modern congestion controller
// has — and when the sender recovers, the allowance it last validated
// is still there to return to immediately.
//
// ---- The spiral (2026-09-22, two viewers, 30 fps / 3840 / Q50) -----
//
// The field log showed the law above failing in a loop:
//  1. Application-limited inflation. A mostly-static desktop sent
//     ~2.2 Mbps while clean reports let the allowance climb ×1.25 a
//     tick to 23.9 Mbps. The pacer ceiling and encoder max follow the
//     allowance (×1.5), so the next scene change's keyframe left at
//     ~35 Mbps into a path that carried a fraction of that.
//  2. That burst lost packets; frame grading turned ~8 % packet loss
//     into ~99 % "frames lost"; the law cut ×0.70 per tick while the
//     lagging reports kept showing it: 23.9 → 16.7 → 11.7 → 8.2 → 5.7
//     … Mbps to the 250 kbps floor at 480 px, actually sending 20-270
//     kbps. The ladder followed every step (1440/1920/480/960 px).
//  3. The knee was forgotten 30 s later and the cycle repeated: 178
//     cuts to 565 raises in one session.
// The fixes, each a targeted change to the law above (constants and
// reasoning in VideoRatePolicy.h, Thresholds):
//  a. Probing is capped at max(1.5 × sent, sent + 1 Mbps) of the
//     sender's measured output (kAppLimited*), so the allowance means
//     "probed capacity", not "nobody objected yet".
//  b. The probe is ~8 %/s far below the max, ~4 %/s near it.
//  c. Back-off size comes from packet loss, clamped to 3-15 % per
//     tick, needs damaging loss on 2 consecutive ticks (3 right after a
//     keyframe: one IDR's loss straddles two report windows), and never
//     goes below 70 % of the recent achieved goodput.
//  d. The resolution ladder steps down only after 3 s below a rung's
//     floor, up only after 8 s of comfort, and never changes twice
//     within 6 s.
//  e. The knee relaxes upward after 15 s without loss instead of being
//     forgotten at 30 s; the creep stops at 97 % of it.
//  f. The worst governing peer still sets the rate (a share is only as
//     smooth as its worst receiver), but through (c): one viewer's
//     transient — a single lossy window — costs nothing, and a
//     persistent one walks the share down ≤ 15 % a tick and stops at
//     70 % of what the path was carrying, instead of repeating ×0.70
//     cuts. Persistence is counted across the worst peer of each tick,
//     so loss that moves between viewers (a shared uplink) is still
//     persistence.
class VideoRateController : public QObject {
    Q_OBJECT
public:
    explicit VideoRateController(VideoStreamId streamId, QObject* parent = nullptr);

    // User/server-resolved envelope; re-apply whenever settings change.
    void setEnvelope(int minKbps, int maxKbps, int fps, int maxLongEdge);
    void setActive(bool active);
    // The codec the stream is actually being encoded in. It changes
    // the QUALITY FLOORS, not the control law: HEVC carries the same
    // picture in ~0.6x the bits, so the bitrate below which a given
    // size stops being worth sending is 0.6x too. Set it before the
    // encoder session is rebuilt, or the ladder spends the first few
    // ticks after a switch judging H.265 by H.264's floors and steps
    // resolution down for no reason.
    void setCodec(VideoCodecKind codec);
    VideoCodecKind codec() const { return m_codec; }

    // Current outputs, read by the sender each capture tick.
    int targetKbps() const;
    int maxKbps() const;
    // Long edge and frame rate after the ladder (≤ the envelope).
    int longEdge() const;
    int fps() const;

    // Inputs.
    void reportDelivery(const QString& userId, const VideoDeliveryReport& r);
    void reportKeyframeRequest();
    // One window of what the SENDER achieved (capture/encode rates,
    // packets per frame). Production feeds it through the source below
    // at the top of every tick; tests call it directly.
    void reportSendWindow(const videosend::Window& w);
    // Called at the start of each tick with the fps the sender is
    // currently being asked for; returns the window since the last
    // call (invalid = nothing to report). Owners wire their counters
    // here so the window and the decision that uses it are always the
    // same age.
    using SendWindowSource = std::function<videosend::Window(int askedFps)>;
    void setSendWindowSource(SendWindowSource source) {
        m_sendSource = std::move(source);
    }

    // Diagnostics — for logs, tests and a local stats surface only.
    int networkKbps() const { return m_bitrate; }
    double bitrateScale() const { return m_bitrateScale; }
    int senderFpsCap() const { return m_senderFpsCap; }      // 0 = none
    int senderEdgeStep() const { return m_senderEdgeStep; }  // 0 = full
    double sentFpsEstimate() const { return m_sentFpsEwma; }
    double packetsPerFrameEstimate() const { return m_pktPerFrameEwma; }
    double lastWorstLossPct() const { return m_lastLossPct; }
    double lastFrameDamagePct() const { return m_lastDamagePct; }
    int kneeKbps() const { return m_kneeKbps; }
    // Smoothed measured sender output (kbps); < 0 = no measurement yet.
    double sentKbpsEstimate() const { return m_haveSentRate ? m_sentKbpsEwma : -1.0; }
    // The most probing may lift the allowance to right now (app-limited
    // cap, Thresholds::kAppLimited*); the envelope max without a
    // measurement.
    int probeCapKbps() const;
    const videosend::Window& lastSendWindow() const { return m_lastWindow; }

    // One evaluation of the control law. Production drives this from
    // the internal 500 ms timer and nothing else calls it; it is public
    // so the unit tests can step the loop deterministically instead of
    // sleeping through timer ticks.
    void tick();

    // Test seam: the ladder's dwell/upshift hysteresis is measured in
    // ticks, but blind mode is measured in wall-clock ms. Tests that
    // want to reach blind mode without sleeping move the clock instead.
    void setNowForTest(qint64 nowMs) { m_testNowMs = nowMs; }

signals:
    // Fired when a back-off just happened — the next frame must be an
    // IDR so receivers resync at the new rate immediately.
    void forceKeyframe();

private:
    enum class Health { Blind, NoGovernor, Healthy, Hold, Trim, Cut };

    VideoCodecKind m_codec = VideoCodecKind::H264;

    qint64 nowMs() const;
    videorate::Content content() const;
    int startBitrate() const;
    int blindCeiling() const;
    void resetState();
    // Worst governing peer this window; writes its id, its packet loss
    // and the frame damage that loss implies at the current frame size.
    // Cut here means "damaging loss this window, or a keyframe storm" —
    // a back-off CANDIDATE; tick() applies persistence and sizes it
    // from `smoothedLossPct` (the worst peer's smoothed packet loss).
    Health classify(qint64 now, QString& worstPeer, double& worstLossPct,
                    double& smoothedLossPct, double& damagePct, int kf) const;
    void applyLadder();
    void evaluateSender();
    int rungFps() const;
    void logSendWindow() const;

    const VideoStreamId m_streamId;
    QTimer m_timer;

    // Envelope.
    int m_minKbps = 250;
    int m_maxKbps = 10000;
    int m_fps = 30;
    int m_maxLongEdge = 1920;

    // State.
    int m_bitrate = 4000;
    int m_rungIdx = 0;             // index into the content's ladder
    int m_healthyTicks = 0;        // consecutive healthy evaluations
    int m_comfortTicks = 0;        // consecutive upshift-worthy ones
    int m_dwellTicks = 0;          // no ladder upshift for this long after a cut
    int m_ladderDwellTicks = 0;    // no ladder change of either kind
    int m_belowFloorTicks = 0;     // consecutive ticks below the rung floor
    int m_lossStreak = 0;          // consecutive ticks with damaging loss
    quint32 m_lossHistory = 0;     // one bit per tick: damaging loss?
    int m_kfRequests = 0;          // since last tick
    bool m_everGoverned = false;   // a peer has reported loss at least once
    qint64 m_testNowMs = -1;

    struct PeerSample {
        VideoDeliveryReport report;
        qint64 atMs = 0;
        // Smoothed packet counts (Thresholds::kLossEwmaDecay).
        double lostAcc = 0.0;
        double expectedAcc = 0.0;
        double gradedLossPct() const;
        double smoothedLossPct() const;
    };
    QHash<QString, PeerSample> m_peers;

    qint64 m_activeSinceMs = 0;
    bool m_wasBlind = false;       // edge-detect for the blind-mode log
    double m_lastLossPct = 0.0;
    double m_lastDamagePct = 0.0;

    // Knee memory (Thresholds::kKnee*): the rate at which loss last
    // began, and when.
    int m_kneeKbps = 0;
    int m_kneeAtTick = 0;
    // Last tick the probe moved inside the knee band — or a loss event
    // happened. Its own clock, so a trim does not restart the creep's
    // 4 s interval from zero and let it fire two ticks later.
    int m_lastKneeStepTick = 0;
    // Highest rate a healthy tick was seen at, and when
    // (Thresholds::kKneeNeedsCleanRatio).
    int m_cleanKbps = 0;
    int m_cleanAtTick = 0;

    // Measured sender output (Thresholds::kSentRateAlpha) — the basis of
    // the app-limited probe cap and the goodput floor.
    double m_sentKbpsEwma = 0.0;
    bool m_haveSentRate = false;
    int m_ticksSinceKeyframe = 1 << 20;  // ticks since a send window held an IDR
    bool m_windowHadKeyframe = false;    // reported since the last tick
    int m_lastLossTick = 0;              // last back-off (knee hold clock)

    // Sender-capacity state (SenderPolicy).
    SendWindowSource m_sendSource;
    videosend::Window m_lastWindow;
    bool m_windowFresh = false;    // m_lastWindow not yet evaluated
    double m_sentFpsEwma = 0.0;
    double m_captureFpsEwma = 0.0;
    double m_workMsEwma = 0.0;
    double m_pktPerFrameEwma = 0.0; // 0 = unknown → damage == loss
    bool m_haveEwma = false;
    double m_bitrateScale = 1.0;
    int m_senderFpsCap = 0;
    int m_senderEdgeStep = 0;
    videosend::Bottleneck m_fpsCapReason = videosend::Bottleneck::None;
    int m_senderBadTicks = 0;
    int m_senderGoodTicks = 0;
    int m_senderDwellTicks = 0;
    int m_senderUpWaitTicks = videorate::SenderPolicy::kUpWaitTicks;
    int m_ticksSinceSenderUp = 1 << 20;
    int m_ticksSinceSenderDown = 1 << 20;
    int m_tickCount = 0;

    static constexpr qint64 kPeerSampleTtlMs = 2500;
    // Blind mode: no peer has reported anything for kPeerSampleTtlMs
    // (old client, control channel down, or reports lost). Without
    // evidence the controller must not climb on faith — a maxed
    // envelope once sent 37 Mbps into a WiFi path with zero feedback
    // and the viewer displayed nothing. The clamp applies only while
    // NO report has EVER arrived, and never below the share's own
    // start bitrate: clamping a 50 Mbps LAN share that has already
    // been proven clean, because two reports went missing, is how the
    // ceiling turned into a second death spiral.
    static constexpr int kBlindCeilingKbps = 8000;
    static constexpr qint64 kBlindGraceMs = 3000;
};
