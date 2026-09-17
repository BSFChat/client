#pragma once

#include "voice/video/VideoCodec.h"
#include "voice/video/VideoDeliveryReport.h"
#include "voice/video/VideoRatePolicy.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

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
//   loss < 2 %                → probe up (×1.10, ×1.25 while far below
//                               the configured max)
//   2 % ≤ loss < 6 %          → hold
//   6 % ≤ loss < 10 %         → trim ×0.95, 2 s dwell
//   loss ≥ 10 % or kf-storm   → cut ×0.70, then a 4 s dwell
//   no peer reports loss      → hold (old client — must NOT read as
//                               100 % loss, and must not license a
//                               climb on no evidence either)
//
// Thresholds, floors and the fps/resolution ladder all live in
// VideoRatePolicy.h so they can be reasoned about without a Qt event
// loop. The WORST governing peer sets the rate — a share is only as
// smooth as its worst receiver — and the log line names it.
class VideoRateController : public QObject {
    Q_OBJECT
public:
    explicit VideoRateController(VideoStreamId streamId, QObject* parent = nullptr);

    // User/server-resolved envelope; re-apply whenever settings change.
    void setEnvelope(int minKbps, int maxKbps, int fps, int maxLongEdge);
    void setActive(bool active);

    // Current outputs, read by the sender each capture tick.
    int targetKbps() const { return m_bitrate; }
    int maxKbps() const;
    // Long edge and frame rate after the ladder (≤ the envelope).
    int longEdge() const;
    int fps() const;

    // Inputs.
    void reportDelivery(const QString& userId, const VideoDeliveryReport& r);
    void reportKeyframeRequest();

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

    qint64 nowMs() const;
    videorate::Content content() const;
    int startBitrate() const;
    int blindCeiling() const;
    void resetState();
    // Worst governing peer this window; writes its id and loss.
    Health classify(qint64 now, QString& worstPeer, double& worstLossPct,
                    int kf) const;
    void applyLadder();

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
    int m_dwellTicks = 0;          // minimum wait after any downshift
    int m_kfRequests = 0;          // since last tick
    bool m_everGoverned = false;   // a peer has reported loss at least once
    qint64 m_testNowMs = -1;

    struct PeerSample { VideoDeliveryReport report; qint64 atMs = 0; };
    QHash<QString, PeerSample> m_peers;

    qint64 m_activeSinceMs = 0;
    bool m_wasBlind = false;       // edge-detect for the blind-mode log

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
