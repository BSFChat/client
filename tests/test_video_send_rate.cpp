// The send side's frame-rate awareness (2026-09-21, "streams are really
// good quality, but very choppy"): the instrumentation that tells a
// capture-, encode- or network-bound sender apart, and the rate
// controller's response to each.
//
// Every controller case here is deterministic. There is no wall clock
// and no timer: the controller is stepped with tick(), its send-side
// input is a synthetic videosend::Window, and its receiver input is a
// synthetic report — or, for the oscillation cases, a small path model
// with a hard capacity that turns the controller's own output back into
// loss and frame sizes, closing the loop the way the field does.
//
// Deliberately a separate target from test_video_pipeline: that one
// also carries the receive pipeline, which is being reworked in
// parallel, and nothing here needs it.

#include <QtTest/QtTest>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QSignalSpy>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include <algorithm>
#include <cmath>
#include <vector>

#include "voice/video/FrameConverter.h"
#include "voice/video/RtpPacerCore.h"
#include "voice/video/VideoDeliveryReport.h"
#include "voice/video/VideoEncoder.h"
#include "voice/video/VideoRateController.h"
#include "voice/video/VideoRatePolicy.h"
#include "voice/video/VideoSendPipeline.h"
#include "voice/video/VideoSendStats.h"

using videosend::Bottleneck;
using videosend::Window;

namespace {

const QString kPeer = QStringLiteral("@viewer:test");
constexpr int kEncodeMs = 30000;   // liveness only; see test_video_pipeline

VideoDeliveryReport report(quint64 expected, double lossPct) {
    VideoDeliveryReport r;
    r.hasLoss = true;
    r.expected = std::max<quint64>(expected, 1);
    r.lost = quint64(std::llround(double(r.expected) * lossPct / 100.0));
    return r;
}

// A window as the ScreenShareController would produce it.
// `sentKbps` 0 means "this window carries no rate measurement" — the
// controller then has no app-limited probe cap and no goodput floor,
// which is what the pure frame-rate cases below want.
Window window(int targetFps, double sentFps, double captureFps,
              double packetsPerFrame, bool polled = true,
              double supersededPerSec = 0.0, double workMs = 4.0,
              double overwrittenPerSec = 0.0, double sentKbps = 0.0,
              int keyframes = 0) {
    Window w;
    w.valid = true;
    w.seconds = 0.5;
    w.targetFps = targetFps;
    w.capturePolled = polled;
    w.sentFps = sentFps;
    w.captureFps = captureFps;
    w.packetsPerFrame = packetsPerFrame;
    w.supersededPerSec = supersededPerSec;
    w.meanWorkMs = workMs;
    w.overwrittenPerSec = overwrittenPerSec;
    w.sentKbps = sentKbps;
    w.keyframes = keyframes;
    return w;
}

// A window from a sender that is filling its allowance: what it sends
// IS the controller's current output. The closed-loop cases below use
// it so the app-limited cap and the goodput floor see real numbers.
Window fullWindow(const VideoRateController& rc, double packetsPerFrame,
                  int keyframes = 0) {
    return window(rc.fps(), rc.fps(), rc.fps(), packetsPerFrame, true, 0.0,
                  4.0, 0.0, double(rc.targetKbps()), keyframes);
}

// Packets per frame the controller's CURRENT output implies.
double packetsAt(const VideoRateController& rc) {
    const double bytesPerFrame =
        double(rc.targetKbps()) * 1000.0 / 8.0 / double(std::max(rc.fps(), 1));
    return std::max(1.0, bytesPerFrame / double(videosend::kRtpPayloadBytes));
}

// Packets the current output sends in one 500 ms report window.
quint64 packetsPerTick(const VideoRateController& rc) {
    return quint64(std::max(1.0, double(rc.targetKbps()) * 1000.0 / 8.0
                                 / double(videosend::kRtpPayloadBytes) / 2.0));
}

// A path with a hard capacity: everything above it is dropped, plus a
// fixed loss floor. Feeds the controller its own consequences.
struct PathModel {
    double capacityKbps = 25000.0;
    double baseLossPct = 0.0;
    // Lost packets are whole numbers; the fractional remainder carries
    // to the next window, so the long-run rate is exactly the model's
    // and small rates arrive the way they do on a real path — mostly 0,
    // sometimes 1. (Rounding each window independently would turn
    // 0.4 % into a step function of the bitrate: 0 lost below ~125
    // packets a window, 1 above.)
    double carry = 0.0;

    double lossFor(int kbps) const {
        const double over = kbps > capacityKbps
            ? (double(kbps) - capacityKbps) / double(kbps) * 100.0 : 0.0;
        return std::min(100.0, baseLossPct + over);
    }

    VideoDeliveryReport reportFor(int kbps, quint64 expected) {
        const double want = double(expected) * lossFor(kbps) / 100.0 + carry;
        const quint64 lost = quint64(std::floor(want));
        carry = want - double(lost);
        VideoDeliveryReport r;
        r.hasLoss = true;
        r.expected = std::max<quint64>(expected, 1);
        r.lost = std::min(lost, r.expected);
        return r;
    }
};

struct RunStats {
    int lossEvents = 0;       // ticks that trimmed or cut
    int configChanges = 0;    // (edge, fps) changes = encoder rebuilds = IDRs
    double meanKbps = 0.0;
    double meanDamagePct = 0.0;
    int minKbps = 1 << 30;
    int maxKbps = 0;
};

// Steps `ticks` evaluations against the path; statistics are gathered
// over the last `measure` ticks only, so start-up does not count.
RunStats runPath(VideoRateController& rc, PathModel& path, int ticks,
                 int measure, int fps = 30) {
    RunStats st;
    int lastEdge = rc.longEdge(), lastFps = rc.fps();
    double sumKbps = 0.0, sumDamage = 0.0;
    for (int i = 0; i < ticks; ++i) {
        const int before = rc.networkKbps();
        const int out = rc.targetKbps();
        const double loss = path.lossFor(out);
        rc.reportSendWindow(fullWindow(rc, packetsAt(rc)));
        rc.reportDelivery(kPeer, path.reportFor(out, packetsPerTick(rc)));
        rc.tick();
        if (i < ticks - measure) {
            lastEdge = rc.longEdge();
            lastFps = rc.fps();
            continue;
        }
        if (rc.networkKbps() < before) ++st.lossEvents;
        if (rc.longEdge() != lastEdge || rc.fps() != lastFps) {
            ++st.configChanges;
            lastEdge = rc.longEdge();
            lastFps = rc.fps();
        }
        sumKbps += out;
        sumDamage += videorate::frameDamagePct(loss, double(out) * 1000.0 / 8.0
                         / double(fps) / double(videosend::kRtpPayloadBytes));
        st.minKbps = std::min(st.minKbps, out);
        st.maxKbps = std::max(st.maxKbps, out);
    }
    st.meanKbps = sumKbps / double(measure);
    st.meanDamagePct = sumDamage / double(measure);
    return st;
}

// Screen-like content with real motion: text-ish stripes plus a moving
// block and a scrolling band, so the encoder has inter-frame work to do.
QVideoFrame makeScreenFrame(int w, int h, int index) {
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(QColor(24, 26, 30));
    QPainter p(&img);
    for (int y = (index * 6) % 18; y < h; y += 18)
        p.fillRect(40, y, w - 80, 3, QColor(200, 200, 205));
    p.fillRect((index * 23) % (w - 300), h / 3, 300, 220, QColor(210, 70, 60));
    p.fillRect(0, (index * 11) % h, w, 40, QColor(60, 140, 220));
    p.end();
    QVideoFrameFormat fmt(img.size(),
        QVideoFrameFormat::pixelFormatFromImageFormat(img.format()));
    QVideoFrame frame(fmt);
    if (!frame.map(QVideoFrame::WriteOnly)) return {};
    for (int row = 0; row < img.height(); ++row)
        memcpy(frame.bits(0) + row * frame.bytesPerLine(0),
               img.constScanLine(row), size_t(img.bytesPerLine()));
    frame.unmap();
    return frame;
}

// ---- Pacer simulation (RtpPacerCore, fake clock) ----------------------
//
// A stream of access units at `fps`, P-frames sized for `targetKbps`, an
// IDR of `idrBytes` every `idrEvery` frames, packetised at the real RTP
// payload size, through a pacer whose ceiling is 1.5 × target — exactly
// what ScreenShareController sets (maxKbps = target + target/2). Driven
// either the old way (drain only when a frame is pushed) or the new way
// (also every 5 ms while anything is queued).
struct PacerRun {
    int frames = 0;
    int complete = 0;            // every packet of the frame was sent
    quint64 droppedPackets = 0;
    double idrLatencyMs = 0.0;   // mean capture → last packet sent, IDRs
    double pLatencyMs = 0.0;     // same, P-frames
    double worstLatencyMs = 0.0;
    double sentKbps = 0.0;
};

PacerRun simulatePacer(int fps, int targetKbps, int idrBytes, int idrEvery,
                       double seconds, bool ticked) {
    RtpPacerCore<int> pacer;   // item = frame index
    pacer.setCeilingKbps(targetKbps + targetKbps / 2);
    const int nFrames = int(seconds * fps);
    const qint64 intervalUs = 1000000 / fps;
    const int pBytes = std::max(1200, int(double(targetKbps) * 1000.0 / 8.0
                                          / double(fps)));
    std::vector<int> packetsOf(nFrames, 0), sentOf(nFrames, 0);
    std::vector<qint64> lastSentUs(nFrames, -1);
    quint64 sentBytes = 0;
    qint64 now = 0;
    auto send = [&](int&& frame) {
        ++sentOf[frame];
        lastSentUs[frame] = now;
        sentBytes += videosend::kRtpPayloadBytes;
        return true;
    };
    const qint64 endUs = qint64(seconds * 1e6) + 2000000;   // 2 s to drain
    int next = 0;
    for (now = 0; now <= endUs; now += 1000) {                // 1 ms steps
        bool pushed = false;
        if (next < nFrames && now >= next * intervalUs) {
            const int bytes = (next % idrEvery == 0) ? idrBytes : pBytes;
            const int packets = int(videosend::packetsFor(bytes));
            packetsOf[next] = packets;
            for (int k = 0; k < packets; ++k)
                pacer.push(now, next, size_t(videosend::kRtpPayloadBytes));
            ++next;
            pushed = true;
        }
        if (pushed || (ticked && now % 5000 == 0 && !pacer.empty()))
            pacer.drain(now, send);
    }
    PacerRun r;
    r.frames = nFrames;
    r.droppedPackets = pacer.dropped();
    double idrSum = 0, pSum = 0;
    int idrN = 0, pN = 0;
    for (int f = 0; f < nFrames; ++f) {
        if (sentOf[f] != packetsOf[f]) continue;
        ++r.complete;
        const double ms = double(lastSentUs[f] - f * intervalUs) / 1000.0;
        r.worstLatencyMs = std::max(r.worstLatencyMs, ms);
        if (f % idrEvery == 0) { idrSum += ms; ++idrN; } else { pSum += ms; ++pN; }
    }
    r.idrLatencyMs = idrN ? idrSum / idrN : -1.0;
    r.pLatencyMs = pN ? pSum / pN : -1.0;
    r.sentKbps = double(sentBytes) * 8.0 / 1000.0 / seconds;
    return r;
}

QVideoFrame makeTestFrame(int w, int h, int index) {
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(QColor(30, 32, 38));
    QPainter p(&img);
    p.fillRect(20 + index * 7, 40, 120, 80, QColor(200, 60, 60));
    for (int y = 0; y < h; y += 14)
        p.fillRect(0, y, w, 2, QColor(220, 220, 225));
    p.end();
    QVideoFrameFormat fmt(img.size(),
        QVideoFrameFormat::pixelFormatFromImageFormat(img.format()));
    QVideoFrame frame(fmt);
    if (!frame.map(QVideoFrame::WriteOnly)) return {};
    for (int row = 0; row < img.height(); ++row)
        memcpy(frame.bits(0) + row * frame.bytesPerLine(0),
               img.constScanLine(row), size_t(img.bytesPerLine()));
    frame.unmap();
    return frame;
}

} // namespace

class TestVideoSendRate : public QObject {
    Q_OBJECT

private slots:

    // ---- The arithmetic the policy rests on ------------------------

    void frameDamageIsTheFieldArithmetic() {
        using videorate::frameDamagePct;
        // A one-packet frame is lost exactly as often as a packet: the
        // old law's view, and still the answer when n is unknown.
        QCOMPARE(frameDamagePct(1.0, 1.0), 1.0);
        QCOMPARE(frameDamagePct(1.0, 0.0), 1.0);
        // The field case: 1 % loss on ~150-packet frames loses ~78 %
        // of frames. A 3-packet P-frame loses ~3 %.
        QVERIFY(std::abs(frameDamagePct(1.0, 150.0) - 77.9) < 0.1);
        QVERIFY(std::abs(frameDamagePct(1.0, 3.0) - 2.97) < 0.01);
        QCOMPARE(frameDamagePct(0.0, 500.0), 0.0);
        QCOMPARE(frameDamagePct(100.0, 2.0), 100.0);
    }

    void packetCountMatchesTheFragmentSize() {
        QCOMPARE(videosend::packetsFor(0), quint64(0));
        QCOMPARE(videosend::packetsFor(1), quint64(1));
        QCOMPARE(videosend::packetsFor(1220), quint64(1));
        QCOMPARE(videosend::packetsFor(1221), quint64(2));
        // The field's opening IDR: 234080 bytes.
        QCOMPARE(videosend::packetsFor(234080), quint64(192));
    }

    void fpsSnapsToAShortList() {
        using videorate::snapFpsDown;
        using videorate::nextFpsUp;
        QCOMPARE(snapFpsDown(27.7), 25);
        QCOMPARE(snapFpsDown(23.9), 20);
        QCOMPARE(snapFpsDown(30.0), 30);
        QCOMPARE(snapFpsDown(3.0), 5);
        QCOMPARE(nextFpsUp(20), 25);
        QCOMPARE(nextFpsUp(25), 30);
        QCOMPARE(nextFpsUp(60), 60);
    }

    // ---- Instrumentation: telling the causes apart -----------------

    void theAccumulatorSeedsThenDifferences() {
        videosend::Accumulator acc;
        videosend::Counters c;
        QVERIFY(!acc.sample(c, 1000, 30, true).valid);   // seed only

        c.captured = 15; c.pushTicks = 15; c.submitted = 15;
        c.encoded = 14; c.bytes = 14 * 12200; c.packets = 14 * 10;
        c.keyframes = 1; c.workUs = 14 * 6000; c.maxWorkUs = 11000;
        c.superseded = 1; c.overwritten = 0; c.emptyTicks = 0;
        const Window w = acc.sample(c, 1500, 30, true);
        QVERIFY(w.valid);
        QCOMPARE(w.captureFps, 30.0);
        QCOMPARE(w.sentFps, 28.0);
        QCOMPARE(w.packetsPerFrame, 10.0);
        QCOMPARE(w.meanWorkMs, 6.0);
        QCOMPARE(w.maxWorkMs, 11.0);
        QCOMPARE(w.keyframes, 1);
        QVERIFY(std::abs(w.sentKbps - 14.0 * 12200.0 * 8.0 / 500.0) < 1e-6);

        // Too short a window keeps accumulating instead of reporting.
        QVERIFY(!acc.sample(c, 1600, 30, true).valid);
        // A gap (controller inactive) re-seeds rather than reporting the
        // gap as a capturer that delivered nothing.
        QVERIFY(!acc.sample(c, 9000, 30, true).valid);
        c.encoded += 15; c.captured += 15;
        QVERIFY(acc.sample(c, 9500, 30, true).valid);
        // A counter going backwards (owner rebuilt) re-seeds too.
        videosend::Counters fresh;
        QVERIFY(!acc.sample(fresh, 10000, 30, true).valid);
    }

    void theBottleneckIsNamedOnlyFromPositiveEvidence() {
        // Meeting the rate: nothing to blame.
        QCOMPARE(window(30, 29, 30, 5).bottleneck(), Bottleneck::None);
        // The encoder's depth-1 slot overflowing is encode, whatever
        // capture did.
        QCOMPARE(window(30, 20, 30, 5, true, 8.0).bottleneck(),
                 Bottleneck::Encode);
        // So is work eating the whole frame interval (33 ms at 30 fps).
        QCOMPARE(window(30, 22, 30, 5, true, 0.0, 31.0).bottleneck(),
                 Bottleneck::Encode);
        // Frames arrived and were overwritten before a push: cadence.
        // (The field's 27.7 captured / ~20 sent.)
        QCOMPARE(window(30, 20, 27.7, 5, true, 0.0, 4.0, 7.7).bottleneck(),
                 Bottleneck::Cadence);
        // A polling capturer that came up short is positive evidence.
        QCOMPARE(window(30, 18, 18, 5, true).bottleneck(), Bottleneck::Capture);
        // The same numbers from an on-change capturer are just a still
        // screen: proves nothing.
        QCOMPARE(window(30, 3, 3, 1, false).bottleneck(), Bottleneck::Idle);
        // The log line names the verdict.
        QVERIFY(window(30, 18, 18, 5, true).describe()
                    .contains(QLatin1String("bottleneck=capture")));
    }

    // ---- Network input: frames, not packets ------------------------

    // Without send windows the controller sees n = 1 and the law is
    // exactly what it was: sub-2 % loss probes. This is what the field
    // controller did with 0.4 % loss on 150-packet frames.
    void withoutSendWindowsSmallLossStillProbes() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 31000, 30, 1920);
        rc.setActive(true);
        const int start = rc.targetKbps();
        for (int i = 0; i < 6; ++i) {
            rc.reportDelivery(kPeer, report(2000, 0.4));
            rc.tick();
        }
        QVERIFY(rc.targetKbps() > start);
    }

    // The same 0.4 % loss with the frame sizes it actually meets: big
    // frames are mostly lost, so the controller must come DOWN until
    // frames are small enough to survive — and then stop.
    void smallLossOnBigFramesBacksOffUntilFramesSurvive() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 31000, 30, 1920);
        rc.setActive(true);
        // Get it up high on a clean path first, as in the field. The
        // probe is ~8 %/s now (it used to be ×1.25 a TICK, which is what
        // inflated the allowance in the field log), so this is ~27 s.
        for (int i = 0; i < 60; ++i) {
            rc.reportSendWindow(fullWindow(rc, packetsAt(rc)));
            rc.reportDelivery(kPeer, report(packetsPerTick(rc), 0.0));
            rc.tick();
        }
        QCOMPARE(rc.targetKbps(), 31000);

        PathModel path;
        path.capacityKbps = 1e9;       // no capacity limit…
        path.baseLossPct = 0.4;        // …just the field's residual loss
        const RunStats st = runPath(rc, path, 160, 80);
        const double damage = videorate::frameDamagePct(0.4, packetsAt(rc));
        qInfo("0.4%% loss: settled %d kbps (range %d-%d), %.1f pkt/frame, "
              "%.1f%% frames lost, %d back-offs / %d rebuilds in 40 s",
              rc.targetKbps(), st.minKbps, st.maxKbps, packetsAt(rc), damage,
              st.lossEvents, st.configChanges);
        QVERIFY2(damage < videorate::Thresholds::kTrimPct, qPrintable(
            QStringLiteral("settled at %1 kbps, %2 pkt/frame, %3%% of frames "
                           "still lost").arg(rc.targetKbps())
                .arg(packetsAt(rc)).arg(damage)));
        QVERIFY2(rc.targetKbps() < 31000 / 2, "must actually come down");
        // …and settles instead of hunting: over the last 40 s no more
        // than a couple of trims, and no encoder rebuild churn.
        QVERIFY2(st.lossEvents <= 3, qPrintable(QStringLiteral(
            "%1 back-offs in 40 s of steady conditions").arg(st.lossEvents)));
        QVERIFY2(st.configChanges <= 1, qPrintable(QStringLiteral(
            "%1 ladder changes in 40 s").arg(st.configChanges)));
    }

    // ---- Oscillation: the sawtooth ---------------------------------

    // A hard 25 Mbps capacity and nothing else. The field controller
    // hit a wall like this every ~3 s (117 cuts in nine minutes). With
    // knee memory it must re-touch the wall rarely, sit close to it in
    // between, and keep the frames it sends intact.
    void aHardCapacityIsTouchedRarelyNotEveryFewSeconds() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 75000, 30, 1920);
        rc.setActive(true);
        PathModel path;
        path.capacityKbps = 25000;
        // 4 minutes; measure the last 3.
        const RunStats st = runPath(rc, path, 480, 360);
        qInfo("25 Mbps wall: %d back-offs in 180 s, mean %.0f kbps "
              "(range %d-%d), %.2f%% frames lost, %d rebuilds",
              st.lossEvents, st.meanKbps, st.minKbps, st.maxKbps,
              st.meanDamagePct, st.configChanges);
        QVERIFY2(st.lossEvents <= 12, qPrintable(QStringLiteral(
            "%1 back-offs in 3 minutes against a fixed ceiling (one every "
            "%2 s)").arg(st.lossEvents)
                .arg(180.0 / std::max(st.lossEvents, 1), 0, 'f', 1)));
        QVERIFY2(st.meanKbps >= 0.70 * path.capacityKbps, qPrintable(
            QStringLiteral("mean %1 kbps of a %2 kbps path — collapsed")
                .arg(st.meanKbps).arg(path.capacityKbps)));
        QVERIFY2(st.meanDamagePct < 5.0, qPrintable(QStringLiteral(
            "%1%% of frames lost on average").arg(st.meanDamagePct)));
        QVERIFY2(st.configChanges <= 2, qPrintable(QStringLiteral(
            "%1 encoder rebuilds in 3 minutes").arg(st.configChanges)));
    }

    // Knee memory must not become a ceiling: when the path grows, the
    // share finds the new capacity.
    void aGrownPathIsFound() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 75000, 30, 1920);
        rc.setActive(true);
        PathModel path;
        path.capacityKbps = 12000;
        runPath(rc, path, 240, 1);
        QVERIFY(rc.targetKbps() <= 13000);
        path.capacityKbps = 30000;
        runPath(rc, path, 240, 1);   // two minutes
        QVERIFY2(rc.targetKbps() >= 22000, qPrintable(QStringLiteral(
            "stuck at %1 kbps after the path grew to 30 Mbps")
                .arg(rc.targetKbps())));
    }

    // ---- The spiral (2026-09-22): the four failures in the field log

    // 1. APPLICATION-LIMITED INFLATION. A mostly-static desktop sends
    //    ~2.2 Mbps and every report is clean, so the old law climbed
    //    ×1.25 a tick — 5769 → 7243 → … → 23883 kbps — an allowance ten
    //    times the real usage, carrying no information about the path
    //    and sizing both the encoder max and the pacer ceiling (1.5 ×
    //    target) for the burst that started the collapse.
    void anAppLimitedSenderDoesNotInflateTheAllowance() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 31000, 30, 1920);
        rc.setActive(true);
        const int start = rc.targetKbps();
        const double sent = 2200.0;      // the field's 5 s "send:" figure
        for (int i = 0; i < 120; ++i) {  // a full minute of clean reports
            rc.reportSendWindow(window(30, 30, 30, 8, true, 0.0, 4.0, 0.0,
                                       sent, i % 20 == 0 ? 1 : 0));
            rc.reportDelivery(kPeer, report(packetsPerTick(rc), 0.0));
            rc.tick();
        }
        // The old law reached the envelope maximum here — 31 Mbps of
        // allowance for a 2.2 Mbps share, and an encoder max and pacer
        // ceiling of 1.5 × that. The cap is a margin over what is
        // actually sent…
        QVERIFY2(rc.probeCapKbps()
                     <= qMax(int(sent * 1.5), int(sent) + 1000) + 1,
                 qPrintable(QStringLiteral("probe cap %1 kbps while sending "
                                           "%2").arg(rc.probeCapKbps())
                                .arg(sent)));
        // …so nothing was climbed; and nothing was taken away either,
        // because bits we are not sending are not evidence in either
        // direction. The share keeps the budget it opened with for its
        // next burst of motion.
        QCOMPARE(rc.networkKbps(), start);
        QCOMPARE(rc.targetKbps(), start);
        QCOMPARE(rc.longEdge(), 1920);
        QCOMPARE(rc.fps(), 30);

        // The cap is about being application-limited, not a general
        // freeze: a sender that fills its allowance still probes all
        // the way to the envelope maximum on the same clean path.
        VideoRateController full(VideoStreamId::Screen);
        full.setEnvelope(250, 31000, 30, 1920);
        full.setActive(true);
        for (int i = 0; i < 120; ++i) {
            full.reportSendWindow(fullWindow(full, packetsAt(full)));
            full.reportDelivery(kPeer, report(packetsPerTick(full), 0.0));
            full.tick();
        }
        QCOMPARE(full.targetKbps(), 31000);
    }

    // 2. ONE KEYFRAME BURST. The field log cut 23883 → 16718 → 11702 →
    //    8191 → 5733 … on the loss from a single scene change, because
    //    ~8 % packet loss on 17-packet frames grades as ~99 % of frames
    //    and every such tick cut ×0.70. One burst must now cost a few
    //    percent at most — the goodput the path just carried is the
    //    floor — and the share must not lose resolution over it.
    void oneKeyframeBurstDoesNotCollapseTheShare() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 31000, 30, 1920);
        rc.setActive(true);
        const double sent = 2200.0;
        for (int i = 0; i < 40; ++i) {
            rc.reportSendWindow(window(30, 30, 30, 8, true, 0.0, 4.0, 0.0, sent));
            rc.reportDelivery(kPeer, report(packetsPerTick(rc), 0.0));
            rc.tick();
        }
        const int before = rc.targetKbps();
        const int edge = rc.longEdge();

        // The burst: a keyframe window at 9 Mbps, 8 % of its packets
        // lost (the field's "loss=8.41% frames=98.8%"), then the tail
        // of it in the next window.
        rc.reportSendWindow(window(30, 30, 30, 31, true, 0.0, 4.0, 0.0,
                                   9000.0, /*keyframes=*/1));
        rc.reportDelivery(kPeer, report(920, 8.41));
        rc.tick();
        rc.reportSendWindow(window(30, 30, 30, 10, true, 0.0, 4.0, 0.0, 3000.0));
        rc.reportDelivery(kPeer, report(300, 3.0));
        rc.tick();
        QVERIFY2(rc.targetKbps() >= before * 7 / 10, qPrintable(
            QStringLiteral("one keyframe burst took %1 → %2 kbps")
                .arg(before).arg(rc.targetKbps())));

        // Five clean seconds later it is still there, at full size.
        for (int i = 0; i < 10; ++i) {
            rc.reportSendWindow(window(30, 30, 30, 8, true, 0.0, 4.0, 0.0, sent));
            rc.reportDelivery(kPeer, report(packetsPerTick(rc), 0.0));
            rc.tick();
        }
        QVERIFY(rc.targetKbps() >= before * 7 / 10);
        QCOMPARE(rc.longEdge(), edge);
    }

    // 3. …and the bounded, persistence-gated back-off must still find a
    //    real capacity drop. A 20 Mbps path that becomes a 3 Mbps one.
    void sustainedCapacityLossStillConverges() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 31000, 30, 1920);
        rc.setActive(true);
        PathModel path;
        path.capacityKbps = 20000;
        runPath(rc, path, 120, 1);
        const int settled = rc.targetKbps();
        QVERIFY2(settled >= 12000, qPrintable(QStringLiteral(
            "only reached %1 kbps of a 20 Mbps path").arg(settled)));

        path.capacityKbps = 3000;
        const RunStats st = runPath(rc, path, 60, 30);   // 30 s, last 15 s
        qInfo("20 → 3 Mbps: %d kbps after 30 s (range %d-%d), %d back-offs",
              rc.targetKbps(), st.minKbps, st.maxKbps, st.lossEvents);
        QVERIFY2(rc.targetKbps() <= 3600, qPrintable(QStringLiteral(
            "still asking for %1 kbps of a 3 Mbps path").arg(rc.targetKbps())));
        QVERIFY2(rc.targetKbps() >= 1500, qPrintable(QStringLiteral(
            "overshot the drop and collapsed to %1 kbps").arg(rc.targetKbps())));
    }

    // 4. RESOLUTION FLAPPING. The field session ran 1440 → 1920 → 480 →
    //    960 px; every change rebuilds the encoder session and costs an
    //    IDR, which is itself the burst that starts the next cut. Under
    //    a bitrate that oscillates around a rung boundary the ladder
    //    must sit still.
    void theLadderDoesNotFlapWhenTheBitrateOscillates() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 31000, 30, 1920);
        rc.setActive(true);
        // A path that alternates between comfortably above and just
        // below what 1080p30 needs, every 3 s.
        PathModel path;
        int changes = 0;
        int lastEdge = rc.longEdge(), lastFps = rc.fps();
        for (int i = 0; i < 400; ++i) {
            path.capacityKbps = (i / 6) % 2 == 0 ? 8000 : 3200;
            const int out = rc.targetKbps();
            rc.reportSendWindow(fullWindow(rc, packetsAt(rc), i % 20 == 0 ? 1 : 0));
            rc.reportDelivery(kPeer, path.reportFor(out, packetsPerTick(rc)));
            rc.tick();
            if (rc.longEdge() != lastEdge || rc.fps() != lastFps) {
                ++changes;
                lastEdge = rc.longEdge();
                lastFps = rc.fps();
            }
        }
        qInfo("oscillating path: %d ladder changes in 200 s, ended at %d px "
              "@ %d fps, %d kbps", changes, rc.longEdge(), rc.fps(),
              rc.targetKbps());
        QVERIFY2(changes <= 4, qPrintable(QStringLiteral(
            "ladder changed %1 times in 200 s — that is flapping")
                .arg(changes)));
    }

    // ---- Sender input: frame-rate awareness ------------------------

    // The owner's ask, literally: bitrate follows the frame rate that
    // is actually going out, down and back up — and the network
    // allowance underneath is neither cut (the network did nothing
    // wrong) nor probed (bits not sent prove nothing).
    void bitrateFollowsTheSentFrameRateBothWays() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 20000, 30, 1920);
        rc.setActive(true);
        for (int i = 0; i < 60; ++i) {
            rc.reportSendWindow(window(30, 30, 30, 10));
            rc.reportDelivery(kPeer, report(800, 0.0));
            rc.tick();
        }
        QCOMPARE(rc.targetKbps(), 20000);
        const int allowance = rc.networkKbps();

        // Capture collapses to 15 of 30 (encode keeps up with what it
        // gets). Within ~2 s the bitrate is about halved.
        for (int i = 0; i < 4; ++i) {
            rc.reportSendWindow(window(rc.fps(), 15, 15, 10));
            rc.reportDelivery(kPeer, report(800, 0.0));
            rc.tick();
        }
        QVERIFY2(rc.targetKbps() <= 20000 * 6 / 10, qPrintable(QStringLiteral(
            "sending ~15 of 30 fps at %1 kbps").arg(rc.targetKbps())));
        QVERIFY(rc.targetKbps() >= 20000 * 4 / 10);
        QCOMPARE(rc.networkKbps(), allowance);

        // Capture recovers: bitrate comes straight back to the allowance
        // it had validated (and the fps cap lifts after its dwell).
        for (int i = 0; i < 80; ++i) {
            rc.reportSendWindow(window(rc.fps(), rc.fps(), rc.fps(), 10));
            rc.reportDelivery(kPeer, report(800, 0.0));
            rc.tick();
        }
        QCOMPARE(rc.fps(), 30);
        QCOMPARE(rc.targetKbps(), 20000);
    }

    void aStillScreenIsNotAnOverloadedSender() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 20000, 30, 1920);
        rc.setActive(true);
        // On-change capture, nothing moving: 2 fps, no pressure anywhere.
        for (int i = 0; i < 60; ++i) {
            rc.reportSendWindow(window(30, 2, 2, 2, /*polled=*/false));
            rc.reportDelivery(kPeer, report(10, 0.0));
            rc.tick();
        }
        QCOMPARE(rc.bitrateScale(), 1.0);
        QCOMPARE(rc.senderFpsCap(), 0);
        QCOMPARE(rc.senderEdgeStep(), 0);
        QCOMPARE(rc.fps(), 30);
    }

    // Capture-bound: pixels cannot help, so the frame rate is lowered
    // to what capture sustains — after 2 s of evidence, not on one
    // bad window.
    void captureBoundLowersTheFrameRateAfterTwoSeconds() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 20000, 30, 1920);
        rc.setActive(true);
        for (int i = 0; i < videorate::SenderPolicy::kDownTicks - 1; ++i) {
            rc.reportSendWindow(window(30, 18, 18, 10));
            rc.tick();
            QCOMPARE(rc.fps(), 30);
        }
        rc.reportSendWindow(window(30, 18, 18, 10));
        rc.tick();
        QCOMPARE(rc.senderFpsCap(), 15);   // snap(18) = 15
        QCOMPARE(rc.fps(), 15);
        QCOMPARE(rc.senderEdgeStep(), 0);  // resolution untouched
        QCOMPARE(rc.longEdge(), 1920);

        // Capture keeps up with 15: nothing more happens.
        for (int i = 0; i < 16; ++i) {
            rc.reportSendWindow(window(15, 15, 15, 10));
            rc.tick();
        }
        QCOMPARE(rc.fps(), 15);
    }

    // Encode-bound: cost tracks pixels, so resolution goes first and the
    // frame rate is defended until the resolution steps run out — each
    // step 10 s apart, never faster.
    void encodeBoundShedsResolutionBeforeFrameRate() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 20000, 30, 1920);
        rc.setActive(true);
        QList<int> stepAt;
        int lastEdge = rc.longEdge();
        for (int i = 0; i < 160; ++i) {
            rc.reportSendWindow(window(rc.fps(), 20, 30, 10, true,
                                       /*superseded*/ 10.0, /*work*/ 45.0));
            rc.tick();
            if (rc.longEdge() != lastEdge) { stepAt << i; lastEdge = rc.longEdge(); }
        }
        QCOMPARE(stepAt.size(), videorate::kSenderEdgeSteps - 1);
        for (int k = 1; k < stepAt.size(); ++k)
            QVERIFY2(stepAt[k] - stepAt[k - 1]
                         >= videorate::SenderPolicy::kDwellTicks,
                     "resolution steps faster than the dwell");
        QCOMPARE(rc.longEdge(), 960);        // 0.5 × 1920
        QVERIFY2(rc.fps() < 30, "only then does the frame rate give way");
    }

    // Recovery needs sustained, PREDICTED headroom; a failed attempt
    // doubles the wait before the next one. This is what keeps an
    // FPS-chasing loop from hunting on a machine that sits right at
    // its limit.
    void upshiftWaitsForHeadroomAndBacksOffWhenItFails() {
        using SP = videorate::SenderPolicy;
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 20000, 30, 1920);
        rc.setActive(true);
        auto encodeBound = [&]() {
            rc.reportSendWindow(window(rc.fps(), 0.6 * rc.fps(), 30, 10, true,
                                       10.0, 45.0));
            rc.tick();
        };
        // Headroom at the next size up: 10 ms of work at this size,
        // ×(1/0.75)² ≈ 17.8 ms there, under 0.6 × 33 ms.
        auto comfortable = [&]() {
            rc.reportSendWindow(window(rc.fps(), rc.fps(), rc.fps(), 10, true,
                                       0.0, 10.0));
            rc.tick();
        };
        for (int i = 0; i < SP::kDownTicks; ++i) encodeBound();
        QCOMPARE(rc.senderEdgeStep(), 1);

        // Upshift: not before dwell and the wait have both elapsed.
        int ticks = 0;
        while (rc.senderEdgeStep() == 1 && ticks < 200) { comfortable(); ++ticks; }
        QCOMPARE(rc.senderEdgeStep(), 0);
        QVERIFY2(ticks >= SP::kUpWaitTicks, qPrintable(QStringLiteral(
            "upshifted after %1 ticks").arg(ticks)));
        const int firstWait = ticks;

        // …and it did not fit: overloaded again straight away.
        for (int i = 0; i < SP::kDownTicks + SP::kDwellTicks; ++i) encodeBound();
        QCOMPARE(rc.senderEdgeStep(), 1);
        ticks = 0;
        while (rc.senderEdgeStep() == 1 && ticks < 400) { comfortable(); ++ticks; }
        QCOMPARE(rc.senderEdgeStep(), 0);
        QVERIFY2(ticks >= 2 * SP::kUpWaitTicks && ticks > firstWait,
                 qPrintable(QStringLiteral("second attempt after %1 ticks "
                                           "(first %2)").arg(ticks).arg(firstWait)));

        // Work that would NOT fit at the bigger size never upshifts,
        // however long it is "meeting the rate" at the smaller one.
        for (int i = 0; i < SP::kDownTicks + SP::kDwellTicks; ++i) encodeBound();
        QCOMPARE(rc.senderEdgeStep(), 1);
        for (int i = 0; i < 300; ++i) {
            rc.reportSendWindow(window(rc.fps(), rc.fps(), rc.fps(), 10, true,
                                       0.0, 16.0));   // ×1.78 = 28 ms > 20
            rc.tick();
        }
        QCOMPARE(rc.senderEdgeStep(), 1);
    }

    // A sender wobbling around the threshold must not re-key the
    // encoder over and over: every (size, rate) change is a session
    // rebuild and an IDR, i.e. a burst of exactly the kind of large
    // frame the rest of this work is trying to avoid.
    void aWobblingSenderDoesNotReKeyTheEncoder() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 20000, 30, 1920);
        rc.setActive(true);
        int changes = 0;
        int lastEdge = rc.longEdge(), lastFps = rc.fps();
        // 5 minutes: alternating good and bad seconds.
        for (int i = 0; i < 600; ++i) {
            const bool bad = (i / 2) % 2 == 0;
            const double sent = bad ? 0.7 * rc.fps() : rc.fps();
            rc.reportSendWindow(window(rc.fps(), sent, sent, 10, true,
                                       bad ? 6.0 : 0.0, bad ? 34.0 : 12.0));
            rc.reportDelivery(kPeer, report(800, 0.0));
            rc.tick();
            if (rc.longEdge() != lastEdge || rc.fps() != lastFps) {
                ++changes;
                lastEdge = rc.longEdge();
                lastFps = rc.fps();
            }
        }
        QVERIFY2(changes <= 6, qPrintable(QStringLiteral(
            "%1 encoder rebuilds in 5 minutes").arg(changes)));
    }

    // ---- How the inputs combine ------------------------------------

    // Minimum of what each input allows, always inside the envelope
    // the user and the server set.
    void theInputsCombineAsAMinimumInsideTheEnvelope() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 8000, 24, 1280);   // e.g. server caps
        rc.setActive(true);
        for (int i = 0; i < 80; ++i) {
            rc.reportSendWindow(window(rc.fps(), rc.fps(), rc.fps(), 4));
            rc.reportDelivery(kPeer, report(400, 0.0));
            rc.tick();
            QVERIFY(rc.targetKbps() <= 8000);
            QVERIFY(rc.maxKbps() <= 8000);
            QVERIFY(rc.fps() <= 24);
            QVERIFY(rc.longEdge() <= 1280);
        }
        QCOMPARE(rc.targetKbps(), 8000);

        // Network trouble AND a capture-bound sender at once: each
        // lowers what it governs; neither raises what the other lowered.
        // (Six ticks: a back-off needs loss on two consecutive ones.)
        for (int i = 0; i < 6; ++i) {
            rc.reportSendWindow(window(rc.fps(), 12, 12, 4));
            rc.reportDelivery(kPeer, report(400, 25.0));
            rc.tick();
        }
        QVERIFY(rc.networkKbps() < 8000);
        QVERIFY(rc.senderFpsCap() > 0 && rc.senderFpsCap() <= 12);
        QVERIFY(rc.fps() <= rc.senderFpsCap());
        QVERIFY(rc.targetKbps()
                <= int(double(rc.networkKbps()) * rc.bitrateScale()) + 1);
        QVERIFY(rc.targetKbps() <= rc.networkKbps());
    }

    // A restarted share gets the whole envelope back, even if the last
    // one ended overloaded.
    void aRestartedShareForgetsSenderCaps() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 20000, 30, 1920);
        rc.setActive(true);
        for (int i = 0; i < 8; ++i) {
            rc.reportSendWindow(window(rc.fps(), 10, 10, 10));
            rc.tick();
        }
        QVERIFY(rc.senderFpsCap() > 0);
        rc.setActive(false);
        rc.setActive(true);
        QCOMPARE(rc.senderFpsCap(), 0);
        QCOMPARE(rc.bitrateScale(), 1.0);
        QCOMPARE(rc.fps(), 30);
    }

    // The pull source is used when wired, with the fps being asked for.
    void theWindowSourceIsPulledEachTick() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 20000, 30, 1920);
        rc.setActive(true);
        int calls = 0, asked = 0;
        rc.setSendWindowSource([&](int fps) {
            ++calls;
            asked = fps;
            return window(fps, 15, 15, 10);
        });
        for (int i = 0; i < 5; ++i) rc.tick();
        QCOMPARE(calls, 5);
        QCOMPARE(asked, rc.fps());
        QVERIFY(rc.bitrateScale() < 1.0);
    }

    // ---- The counters come from the real encode worker -------------

    void thePipelineCountsWhatItEncodesAndWhatItCouldNot() {
        VideoSendPipeline pipe(VideoStreamId::Screen);
        QSignalSpy spy(&pipe, &VideoSendPipeline::encodedFrameReady);
        EncoderConfig cfg;
        cfg.codec = VideoCodecKind::H264;
        cfg.width = cfg.height = 320;
        cfg.fps = 30;
        cfg.targetBitrateKbps = 1500;
        cfg.maxBitrateKbps = 2000;
        cfg.keyframeIntervalSec = 10;
        cfg.screenContent = true;
        pipe.configure(cfg);
        pipe.submitFrame(makeTestFrame(320, 240, 0), 0);
        {
            QElapsedTimer waited; waited.start();
            while (spy.isEmpty() && waited.elapsed() < kEncodeMs)
                QTest::qWait(10);
        }
        if (spy.isEmpty()) QSKIP("no usable H.264 encoder on this host");

        // Back-to-back submits with no event-loop turn: the depth-1
        // slot must overflow at least once, and each overflow counted.
        for (int i = 1; i <= 40; ++i)
            pipe.submitFrame(makeTestFrame(320, 240, i), i * 33000);
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 2, kEncodeMs);
        QTest::qWait(200);

        const videosend::Counters c = pipe.takeCounters();
        QCOMPARE(c.submitted, quint64(41));
        QVERIFY2(c.superseded > 0, "back-to-back submits never overflowed");
        QCOMPARE(c.encoded, quint64(spy.count()));
        QVERIFY(c.encoded + c.superseded <= c.submitted);
        quint64 bytes = 0, packets = 0, kfs = 0;
        for (const auto& args : spy) {
            const auto f = args.at(1).value<EncodedFrame>();
            bytes += quint64(f.data.size());
            packets += videosend::packetsFor(f.data.size());
            if (f.keyframe) ++kfs;
        }
        QCOMPARE(c.bytes, bytes);
        QCOMPARE(c.packets, packets);
        QCOMPARE(c.keyframes, kfs);
        QVERIFY(c.workUs > 0);
        QVERIFY(c.maxWorkUs > 0);
        // The per-window maximum resets on read.
        QCOMPARE(pipe.takeCounters().maxWorkUs, quint32(0));
    }

    // ---- The pacer: throughput must not depend on frame rate --------

    // THE fresh-install case: 5 fps at the default 4 Mbps (100 KB per
    // frame). The old schedule — drain only when a frame arrives, at
    // most 50 ms of budget each time — can send 6 Mbps × 5 × 0.05 =
    // 1.5 Mbps of a 4 Mbps stream. The backlog grows until the cap
    // throws packets away, and nearly every frame loses some. This case
    // runs the core the OLD way to pin that down, and the new way to
    // show it gone.
    void thePacerNoLongerStarvesLowFrameRates() {
        const PacerRun before = simulatePacer(5, 4000, 150000, 50, 20.0, false);
        const PacerRun after = simulatePacer(5, 4000, 150000, 50, 20.0, true);
        qInfo("5 fps / 4 Mbps, drain-on-frame only: %d of %d frames complete, "
              "%llu packets dropped, sent %.0f kbps",
              before.complete, before.frames,
              (unsigned long long)before.droppedPackets, before.sentKbps);
        qInfo("5 fps / 4 Mbps, drain thread:        %d of %d frames complete, "
              "%llu packets dropped, sent %.0f kbps, worst frame %.0f ms",
              after.complete, after.frames,
              (unsigned long long)after.droppedPackets, after.sentKbps,
              after.worstLatencyMs);
        QVERIFY2(before.complete < before.frames / 2,
                 "the old schedule should reproduce the starvation");
        QCOMPARE(after.complete, after.frames);
        QCOMPARE(after.droppedPackets, quint64(0));
        // A 100 KB P-frame at a 6 Mbps ceiling needs ~83 ms (the first
        // 50 ms of budget leaves at once); nothing queues behind it.
        QVERIFY2(after.pLatencyMs < 120.0, qPrintable(QStringLiteral(
            "P-frames %1 ms late at 5 fps").arg(after.pLatencyMs)));
    }

    // At 20 fps and above the old schedule already refilled a full frame
    // interval of budget per call, so it ran at the ceiling; the
    // keyframe delay the loopback probe measured at 30 fps (~165 ms for
    // a 150 KB IDR at 4 Mbps) is the ceiling itself: (150 KB − the 50 ms
    // burst) / 750 KB/s ≈ 150 ms, plus the frames queued behind it. The
    // drain thread must not make that WORSE, and it bounds the latency
    // any receive buffer has to absorb. These numbers are what the
    // playout buffer's default delay should be sized from.
    void keyframeDeliveryLatencyIsTheCeilingNotTheFrameRate() {
        struct Case { int fps, kbps, idr; };
        for (const Case c : {Case{30, 4000, 150000}, Case{30, 7700, 293000},
                             Case{60, 4000, 150000}, Case{30, 30000, 293000},
                             Case{15, 4000, 150000}}) {
            const PacerRun oldRun = simulatePacer(c.fps, c.kbps, c.idr, 60, 20.0, false);
            const PacerRun newRun = simulatePacer(c.fps, c.kbps, c.idr, 60, 20.0, true);
            qInfo("%2d fps %5d kbps IDR %3d KB: IDR delivered %.0f ms (old %.0f), "
                  "P %.1f ms (old %.1f), worst %.0f ms (old %.0f), "
                  "complete %d/%d (old %d/%d)",
                  c.fps, c.kbps, c.idr / 1000, newRun.idrLatencyMs,
                  oldRun.idrLatencyMs, newRun.pLatencyMs, oldRun.pLatencyMs,
                  newRun.worstLatencyMs, oldRun.worstLatencyMs,
                  newRun.complete, newRun.frames, oldRun.complete, oldRun.frames);
            QCOMPARE(newRun.complete, newRun.frames);
            QVERIFY(newRun.idrLatencyMs <= oldRun.idrLatencyMs + 1.0);
            // Physics bound: IDR bytes beyond the burst, at the ceiling,
            // plus one 5 ms tick of quantisation.
            const double ceilingBps = double(c.kbps + c.kbps / 2) * 1000.0 / 8.0;
            const double bound = (double(videosend::packetsFor(c.idr))
                                  * videosend::kRtpPayloadBytes
                                  - 0.05 * ceilingBps) / ceilingBps * 1000.0 + 6.0;
            QVERIFY2(newRun.idrLatencyMs <= bound, qPrintable(QStringLiteral(
                "IDR %1 ms, bound %2 ms").arg(newRun.idrLatencyMs).arg(bound)));
        }
    }

    // ---- Opt-in benchmark: what an encode-bound sender should shed --
    //
    // Not a pass/fail test (timings depend on the machine and its
    // load); run it by hand when the policy's premise needs checking:
    //
    //   BSFCHAT_VIDEO_BENCH=1 ./tests/test_video_send_rate encodeCostBenchmark
    //
    // It measures convert + encode time per frame from a 1920x1080
    // screen-like source across long edges and bitrates, for the
    // hardware backend (if any) and openh264. The sender policy sheds
    // RESOLUTION for an encode-bound sender on the premise that cost
    // tracks pixels and barely tracks bitrate; this is where that
    // premise is measured.
    void encodeCostBenchmark() {
        if (qEnvironmentVariableIsEmpty("BSFCHAT_VIDEO_BENCH"))
            QSKIP("opt-in: set BSFCHAT_VIDEO_BENCH=1");
        constexpr int kFrames = 45;
        QList<QVideoFrame> src;
        for (int i = 0; i < kFrames; ++i) src << makeScreenFrame(1920, 1080, i);
        for (bool hw : {true, false}) {
            for (int edge : {1920, 1440, 960}) {
                for (int kbps : {4000, 16000, 40000}) {
                    auto enc = VideoEncoder::create(VideoCodecKind::H264, hw);
                    if (!enc) continue;
                    EncoderConfig cfg;
                    cfg.codec = VideoCodecKind::H264;
                    const PlanarFrame probe =
                        FrameConverter::toI420(src[0], edge, 0);
                    cfg.width = probe.width;
                    cfg.height = probe.height;
                    cfg.fps = 30;
                    cfg.targetBitrateKbps = kbps;
                    cfg.maxBitrateKbps = kbps + kbps / 2;
                    cfg.keyframeIntervalSec = 10;
                    cfg.screenContent = true;
                    if (!enc->init(cfg)) continue;
                    if (hw && !enc->caps().hardware) continue;  // fell back
                    double convertMs = 0.0, encodeMs = 0.0;
                    quint64 bytes = 0, packets = 0;
                    int n = 0;
                    for (int i = 0; i < kFrames; ++i) {
                        QElapsedTimer t; t.start();
                        const PlanarFrame pf =
                            FrameConverter::toI420(src[i], edge, i * 33333);
                        const qint64 c = t.nsecsElapsed();
                        EncodedFrame out;
                        if (!enc->encode(pf, i == 0, out)) continue;
                        const qint64 e = t.nsecsElapsed() - c;
                        if (i < 5) continue;   // skip warm-up and the IDR
                        convertMs += double(c) / 1e6;
                        encodeMs += double(e) / 1e6;
                        bytes += quint64(out.data.size());
                        packets += videosend::packetsFor(out.data.size());
                        ++n;
                    }
                    if (n == 0) continue;
                    qInfo("%-9s %4dx%-4d %5d kbps: convert %5.2f ms  encode "
                          "%6.2f ms  total %6.2f ms/frame  %6.0f B/frame  "
                          "%5.1f pkt/frame",
                          hw ? "hardware" : "openh264", cfg.width, cfg.height,
                          kbps, convertMs / n, encodeMs / n,
                          (convertMs + encodeMs) / n, double(bytes) / n,
                          double(packets) / n);
                }
            }
        }
    }
};

QTEST_MAIN(TestVideoSendRate)
#include "test_video_send_rate.moc"
