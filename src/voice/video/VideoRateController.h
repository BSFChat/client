#pragma once

#include "voice/video/VideoCodec.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QTimer>

// The "always smooth" brain. One per outgoing stream, evaluating every
// 500 ms. Moves QUALITY (bitrate, then resolution) instead of ever
// letting latency build: the pipelines are depth-1 latest-wins, so the
// only way congestion can manifest is as measured delivery loss — this
// controller closes that loop.
//
// Inputs are app-level receiver reports ({"t":"rr"} over the 0x04
// control channel; RTCP RRs are zero-filled in libdatachannel v0.24.5
// and unusable) plus keyframe-request bursts. Per peer we track the
// delivered/sent byte ratio; the WORST recent peer governs (a share is
// only as smooth as its worst receiver).
//
// Control law (AIMD):
//   ratio < 0.90 or kf-storm  → bitrate ×0.60, force IDR   (fast back-off)
//   ratio < 0.97              → bitrate ×0.85              (mild back-off)
//   stable ≥ 4 ticks          → bitrate ×1.08 + 50 kbps    (slow probe)
// Resolution ladder (native, ¾, ½, ⅜, ¼ of the user's max long edge):
//   downshift when bitrate can't sustain a floor bits-per-pixel at the
//   current size; upshift after 10 comfortable ticks. Screen content
//   prefers sharpness, so fps is never touched here (the capture
//   throttle owns it) — resolution gives way first.
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
    // Long-edge cap after the resolution ladder (≤ envelope max).
    int longEdge() const;

    // Inputs.
    void reportDeliveryRatio(const QString& userId, double ratio);
    void reportKeyframeRequest();

signals:
    // Fired when a back-off just happened — the next frame must be an
    // IDR so receivers resync at the new rate immediately.
    void forceKeyframe();

private:
    void tick();
    double worstRecentRatio() const;
    double bppAt(int longEdge, int kbps) const;

    const VideoStreamId m_streamId;
    QTimer m_timer;

    // Envelope.
    int m_minKbps = 250;
    int m_maxKbps = 10000;
    int m_fps = 30;
    int m_maxLongEdge = 1920;

    // State.
    int m_bitrate = 4000;
    int m_scaleIdx = 0;            // index into kScaleLadder
    int m_stableTicks = 0;
    int m_comfortTicks = 0;
    int m_kfRequests = 0;          // since last tick

    struct PeerSample { double ratio = 1.0; qint64 atMs = 0; };
    QHash<QString, PeerSample> m_peerRatios;
    // True while at least one peer has a fresh delivery report.
    bool hasFreshSamples(qint64 nowMs) const;
    qint64 m_activeSinceMs = 0;
    bool m_wasBlind = false;       // edge-detect for the blind-mode log

    static constexpr double kScaleLadder[5] = {1.0, 0.75, 0.5, 0.375, 0.25};
    static constexpr qint64 kPeerSampleTtlMs = 2500;
    // Blind mode: no peer has reported delivery for kPeerSampleTtlMs
    // (old client, control channel down, or reports lost). Without
    // evidence the controller must not climb on faith — a maxed
    // envelope once sent 37 Mbps into a WiFi path with zero feedback
    // and the viewer displayed nothing. Hold at a rate any plausible
    // path carries until reports (re)appear.
    static constexpr int kBlindCeilingKbps = 8000;
    static constexpr qint64 kBlindGraceMs = 3000;
    // Bits-per-pixel-per-frame bands (screen content codes cheaply,
    // so these sit lower than camera-content heuristics would).
    static constexpr double kBppFloor = 0.025;
    static constexpr double kBppComfort = 0.075;
};
