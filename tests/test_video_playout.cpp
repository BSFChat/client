// Receive-side video playout buffer (VideoPlayoutBuffer) and its wiring
// into VideoReceivePipeline.
//
// The report: "streams are really good quality, but very choppy". Frames
// were shown the instant they decoded, so arrival jitter — above all the
// sender pacer queueing every frame behind each keyframe, which the
// loopback probe measured at ~165 ms per IDR at 30 fps — reached the
// screen as freeze-then-clump. The buffer shows frames on their CAPTURE
// cadence instead, adapting its delay to measured jitter under a user
// ceiling.
//
// The buffer itself is driven with a fake clock and synthetic frames, so
// every timing assertion here is exact and deterministic. Only the two
// pipeline cases at the bottom use wall time, and they assert shapes
// ("spread out, not clumped", "emitted from the worker") with margins
// only a broken pipeline could miss.
//
// What each property protects:
//   * steady cadence under jitter    — the point of the feature;
//   * bounded delay                  — latency is the cost; the ceiling
//                                      is a promise to the user;
//   * zero = today's path            — "Off" must mean off;
//   * loss / long gaps               — a time-scheduled buffer must never
//                                      wait on a frame that is not coming;
//   * drift                          — a fixed anchor fills or drains
//                                      forever; this one must not;
//   * slewed shrink                  — giving latency back must not skip;
//   * late wake-up                   — a busy GUI thread or a deferred
//                                      timer must cost latency, not
//                                      frames (macOS CI caught this);
//   * clock unwrap                   — the 90 kHz RTP clock wraps at 13 h.

#include "voice/video/MediaClockUnwrapper.h"
#include "voice/video/VideoDecoder.h"
#include "voice/video/VideoPlayoutBuffer.h"
#include "voice/video/VideoReceivePipeline.h"
#include "core/AppProfile.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QVideoFrameFormat>

#include <cmath>
#include <random>
#include <vector>

namespace {

constexpr qint64 kFrameUs = 33'333;   // 30 fps
constexpr qint64 kMs = 1000;

struct Shown {
    int index;
    qint64 mediaUs;
    qint64 presentUs;
    qint64 arrivalUs;
};

struct Arrival {
    int index;
    qint64 mediaUs;
    qint64 arrivalUs;
};

// Event-driven replay with an ideal presentation timer: the buffer is
// asked for a frame exactly at each due time, or at arrival for a frame
// that was already late — which is what VideoReceivePipeline's timer
// does in production, minus its 1 ms rounding.
std::vector<Shown> replay(std::vector<Arrival> arrivals, qint64 ceilingUs,
                          VideoPlayoutBuffer<Arrival>* bufOut = nullptr) {
    std::sort(arrivals.begin(), arrivals.end(),
              [](const Arrival& a, const Arrival& b) { return a.arrivalUs < b.arrivalUs; });
    VideoPlayoutBuffer<Arrival> local;
    VideoPlayoutBuffer<Arrival>& buf = bufOut ? *bufOut : local;
    buf.setMaxDelayUs(ceilingUs);
    std::vector<Shown> out;
    size_t i = 0;
    qint64 clock = 0;
    while (i < arrivals.size() || buf.queued() > 0) {
        const qint64 next = i < arrivals.size() ? arrivals[i].arrivalUs
                                                : std::numeric_limits<qint64>::max();
        const auto due = buf.nextDueUs();
        if (due && *due <= next) {
            const qint64 t = std::max(*due, clock);
            clock = t;
            if (auto a = buf.popDue(t)) out.push_back({a->index, a->mediaUs, t, a->arrivalUs});
        } else {
            clock = arrivals[i].arrivalUs;
            buf.push(arrivals[i], arrivals[i].mediaUs, arrivals[i].arrivalUs);
            ++i;
        }
    }
    return out;
}

// What the screen showed before the buffer: every frame at its arrival.
std::vector<Shown> presentOnArrival(const std::vector<Arrival>& arrivals) {
    std::vector<Shown> out;
    for (const auto& a : arrivals) out.push_back({a.index, a.mediaUs, a.arrivalUs, a.arrivalUs});
    return out;
}

// Worst departure of on-screen spacing from capture spacing, skipping
// the first `warmup` frames.
qint64 worstCadenceErrorUs(const std::vector<Shown>& shown, size_t warmup = 0) {
    qint64 worst = 0;
    for (size_t k = std::max<size_t>(1, warmup); k < shown.size(); ++k) {
        const qint64 gap = shown[k].presentUs - shown[k - 1].presentUs;
        const qint64 mediaGap = shown[k].mediaUs - shown[k - 1].mediaUs;
        worst = std::max(worst, std::llabs(gap - mediaGap));
    }
    return worst;
}

qint64 worstHeldUs(const std::vector<Shown>& shown) {
    qint64 worst = 0;
    for (const auto& s : shown) worst = std::max(worst, s.presentUs - s.arrivalUs);
    return worst;
}

// A share as the probe measured it on loopback, plus network noise:
// every `idrEvery` frames the keyframe and the frames queued behind it
// are held by the pacer for `idrHoldUs`, draining one frame interval
// apart; on top, uniform 0..noiseUs of per-frame jitter. Sender clock
// may run `ppm` fast (positive) or slow relative to the receiver.
std::vector<Arrival> share(int frames, qint64 idrHoldUs, int idrEvery,
                           qint64 noiseUs, double ppm = 0, unsigned seed = 7,
                           qint64 transitUs = 20 * kMs) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<qint64> noise(0, std::max<qint64>(0, noiseUs));
    std::vector<Arrival> out;
    const qint64 mediaBase = qint64(1) << 40;   // like MediaClockUnwrapper
    for (int i = 0; i < frames; ++i) {
        const qint64 captureUs = qint64(i) * kFrameUs;   // sender clock
        // Receiver-clock time of the capture instant.
        const qint64 recvClockCapture = qint64(double(captureUs) * (1.0 - ppm * 1e-6));
        qint64 hold = 0;
        if (idrEvery > 0) {
            const int sinceIdr = i % idrEvery;
            // The IDR arrives idrHoldUs late; each frame queued behind it
            // one frame interval less late, until the queue has drained.
            hold = std::max<qint64>(0, idrHoldUs - qint64(sinceIdr) * kFrameUs / 2);
        }
        qint64 arrival = 5'000'000 + recvClockCapture + transitUs + hold + noise(rng);
        // One path, first in first out: a frame cannot overtake the one
        // before it (and the depacketizer + in-order decoder would not
        // hand the buffer one that did).
        if (!out.empty()) arrival = std::max(arrival, out.back().arrivalUs);
        out.push_back({i, mediaBase + captureUs, arrival});
    }
    return out;
}

// ---- Pipeline helpers ----------------------------------------------

QByteArray idrAu() {
    QByteArray au;
    au.append(char(0)).append(char(0)).append(char(0)).append(char(1));
    au.append(char(0x65));
    au.append(QByteArray(32, char(0x42)));
    return au;
}

QByteArray pAu() {
    QByteArray au;
    au.append(char(0)).append(char(0)).append(char(0)).append(char(1));
    au.append(char(0x41));
    au.append(QByteArray(32, char(0x42)));
    return au;
}

class StubDecoder : public VideoDecoder {
public:
    bool init(VideoCodecKind) override { return true; }
    Result decode(const QByteArray&, QVideoFrame& out) override {
        out = QVideoFrame(QVideoFrameFormat(QSize(64, 48),
                                            QVideoFrameFormat::Format_ARGB8888));
        return out.isValid() ? Result::Ok : Result::Error;
    }
    void reset() override {}
};

} // namespace

class TestVideoPlayout : public QObject {
    Q_OBJECT

private slots:

    void initTestCase() {
        // VideoDecodeHealth persists through a profile-scoped QSettings;
        // never touch a real install's.
        bsfchat::setActiveProfile(QStringLiteral("test-videoplayout"));
    }

    // ---- Cadence --------------------------------------------------

    // The field shape: a keyframe every 5 s held ~165 ms by the pacer,
    // plus up to 15 ms of noise. Presented on arrival (the old path)
    // that is a multi-frame freeze and a clump at every keyframe; with
    // a 250 ms ceiling every frame after warm-up lands exactly on its
    // capture spacing. The first assertion is what the old behaviour
    // fails, and keeps the scenario honest: if it ever stops being
    // jittery, the second assertion proves nothing.
    void steadyCadenceUnderKeyframeBursts() {
        const auto arrivals = share(900, 165 * kMs, 150, 15 * kMs);
        QVERIFY2(worstCadenceErrorUs(presentOnArrival(arrivals)) > 100 * kMs,
                 "scenario is not jittery; the cadence assertion would be vacuous");

        const auto shown = replay(arrivals, 250 * kMs);
        QCOMPARE(int(shown.size()), 900);
        // Warm-up: the windows learn the burst at the first keyframe.
        // After that the only irregularity left is the buffer deepening
        // when a keyframe turns up a little later than any before it —
        // bounded by the noise (15 ms), and rare: at most one step per
        // keyframe. Everything else lands on capture spacing, give or
        // take the slewed shrink (5% of a frame) when an old maximum ages
        // out of the window.
        QVERIFY2(worstCadenceErrorUs(shown, 160) <= 16 * kMs,
                 qPrintable(QStringLiteral("cadence error %1 us")
                                .arg(worstCadenceErrorUs(shown, 160))));
        int irregular = 0;
        for (size_t k = 160; k < shown.size(); ++k) {
            const qint64 err = std::llabs((shown[k].presentUs - shown[k - 1].presentUs)
                                          - (shown[k].mediaUs - shown[k - 1].mediaUs));
            const qint64 slewStep = qint64(double(kFrameUs)
                                          * VideoPlayoutBuffer<Arrival>::kSlewRatio);
            if (err > slewStep + 1 * kMs) ++irregular;
        }
        QVERIFY2(irregular <= 5, qPrintable(QStringLiteral("%1 irregular frames").arg(irregular)));
    }

    // Presentation order is capture order, always.
    void framesAreShownInOrder() {
        const auto shown = replay(share(600, 120 * kMs, 90, 40 * kMs), 150 * kMs);
        for (size_t k = 1; k < shown.size(); ++k)
            QVERIFY(shown[k].index > shown[k - 1].index);
    }

    // ---- Latency bound --------------------------------------------

    // Jitter far beyond the ceiling (300 ms keyframe holds, 100 ms
    // ceiling): no frame may be held longer than the ceiling, and frames
    // that are late anyway are shown (or superseded) rather than piling
    // up. This is the promise the setting makes to the user.
    void delayNeverExceedsTheCeiling() {
        VideoPlayoutBuffer<Arrival> buf;
        const auto shown = replay(share(900, 300 * kMs, 60, 40 * kMs), 100 * kMs, &buf);
        QVERIFY2(worstHeldUs(shown) <= 100 * kMs,
                 qPrintable(QStringLiteral("held %1 us").arg(worstHeldUs(shown))));
        QVERIFY(buf.queued() == 0);
        // Nothing lost that was not superseded.
        QCOMPARE(quint64(shown.size()) + buf.stats().skippedLate + buf.stats().overflowed,
                 quint64(900));
    }

    // On a perfectly regular stream the buffer must give the latency
    // back: after the startup allowance expires the added delay is the
    // margin, not the ceiling. (Adaptive, not a fixed delay.)
    void aSteadyStreamCostsOnlyTheMargin() {
        const auto shown = replay(share(900, 0, 0, 0), 400 * kMs);
        qint64 tailWorst = 0;
        for (size_t k = 600; k < shown.size(); ++k)
            tailWorst = std::max(tailWorst, shown[k].presentUs - shown[k].arrivalUs);
        QVERIFY2(tailWorst <= VideoPlayoutBuffer<Arrival>::kMarginUs,
                 qPrintable(QStringLiteral("steady-state hold %1 us").arg(tailWorst)));
    }

    // ---- Zero ------------------------------------------------------

    // A zero ceiling holds nothing: every frame is due the instant it is
    // pushed. (The pipeline never even reaches the buffer at zero — see
    // smoothingOffIsTheOldPath — this is the belt to that brace.)
    void aZeroCeilingHoldsNothing() {
        VideoPlayoutBuffer<int> buf;
        buf.setMaxDelayUs(0);
        qint64 now = 1'000'000;
        for (int i = 0; i < 50; ++i) {
            now += (i % 7) * 11 * kMs;   // irregular arrivals
            buf.push(i, qint64(i) * kFrameUs, now);
            const auto due = buf.nextDueUs();
            QVERIFY(due.has_value());
            QVERIFY(*due <= now);
            QCOMPARE(buf.popDue(now).value_or(-1), i);
        }
    }

    // ---- Loss, keyframe waits, gaps --------------------------------

    // Frames 300..310 never arrive (dropped as loss-suspect upstream).
    // The buffer must not wait for them: frame 311 is shown at its own
    // due time, one capture gap after 299, and cadence carries on.
    void aLostRunIsSkippedNotWaitedFor() {
        auto arrivals = share(600, 0, 0, 10 * kMs);
        arrivals.erase(arrivals.begin() + 300, arrivals.begin() + 311);
        const auto shown = replay(arrivals, 150 * kMs);
        QCOMPARE(int(shown.size()), 589);
        size_t at = 0;
        while (shown[at].index != 311) ++at;
        QCOMPARE(shown[at - 1].index, 299);
        const qint64 gap = shown[at].presentUs - shown[at - 1].presentUs;
        QVERIFY(std::llabs(gap - 12 * kFrameUs) <= 1 * kMs);
        QVERIFY(shown[at].presentUs - shown[at].arrivalUs <= 150 * kMs);
    }

    // A keyframe wait: 1.5 s of nothing (loss, PLI, the IDR's round
    // trip), then the stream resumes. No resync, no stall, and the first
    // frame back is held no longer than normal.
    void aLongKeyframeWaitResumesCleanly() {
        auto arrivals = share(600, 0, 0, 10 * kMs);
        arrivals.erase(arrivals.begin() + 200, arrivals.begin() + 245);
        VideoPlayoutBuffer<Arrival> buf;
        const auto shown = replay(arrivals, 150 * kMs, &buf);
        QCOMPARE(buf.stats().resyncs, quint64(0));
        QCOMPARE(int(shown.size()), 555);
        QVERIFY(worstHeldUs(shown) <= 150 * kMs);
    }

    // The sender restarted its capture clock (media time jumps back an
    // hour). Re-anchor; do not hold frames for an hour.
    void aTimelineRestartReanchors() {
        auto arrivals = share(300, 0, 0, 5 * kMs);
        for (size_t k = 150; k < arrivals.size(); ++k)
            arrivals[k].mediaUs -= qint64(3600) * 1'000'000;
        VideoPlayoutBuffer<Arrival> buf;
        const auto shown = replay(arrivals, 150 * kMs, &buf);
        QCOMPARE(buf.stats().resyncs, quint64(1));
        QCOMPARE(int(shown.size()), 300);
        QVERIFY(worstHeldUs(shown) <= 150 * kMs);
    }

    // ---- Drift -----------------------------------------------------

    // Thirty minutes at 30 fps with the sender's clock 500 ppm fast,
    // then 500 ppm slow — five times worse than any real crystal — plus
    // jitter. A buffer anchored once would drift 900 ms off over this
    // run (filling up, or showing every frame late). The delay must stay
    // under the ceiling throughout, nothing may pile up, and the
    // adaptive target must not ratchet up with elapsed time.
    void clockDriftNeverGrowsTheBuffer_data() {
        QTest::addColumn<double>("ppm");
        QTest::newRow("sender fast") << 500.0;
        QTest::newRow("sender slow") << -500.0;
    }
    void clockDriftNeverGrowsTheBuffer() {
        QFETCH(double, ppm);
        const int frames = 30 * 60 * 30;
        VideoPlayoutBuffer<Arrival> buf;
        const auto shown = replay(share(frames, 0, 0, 20 * kMs, ppm), 150 * kMs, &buf);
        QVERIFY(worstHeldUs(shown) <= 150 * kMs);
        QCOMPARE(buf.stats().overflowed, quint64(0));
        QCOMPARE(buf.stats().resyncs, quint64(0));
        // Nearly everything shown: drift is absorbed by moving the
        // anchor, not by skipping frames.
        QVERIFY2(shown.size() >= size_t(frames) * 99 / 100,
                 qPrintable(QStringLiteral("%1 of %2 shown").arg(shown.size()).arg(frames)));
        // Same hold in the last minute as in the second minute.
        auto meanHold = [&](size_t from, size_t to) {
            double sum = 0;
            for (size_t k = from; k < to; ++k) sum += double(shown[k].presentUs - shown[k].arrivalUs);
            return sum / double(to - from);
        };
        const double early = meanHold(1800, 3600);
        const double late = meanHold(shown.size() - 1800, shown.size());
        QVERIFY2(std::fabs(late - early) <= 10.0 * kMs,
                 qPrintable(QStringLiteral("mean hold %1 us early vs %2 us late")
                                .arg(early).arg(late)));
    }

    // ---- Shrinking -------------------------------------------------

    // One big burst early, then 30 s of calm: the buffer must shed the
    // delay it grew — but by playing slightly fast, never by jumping.
    // No on-screen interval may be shorter than its capture interval by
    // more than the slew ratio (plus 1 ms of rounding).
    void shrinkingIsSlewedNotSkipped() {
        auto arrivals = share(1200, 0, 0, 2 * kMs);
        for (int k = 100; k < 106; ++k)
            arrivals[size_t(k)].arrivalUs += (106 - k) * 30 * kMs;   // one clump
        VideoPlayoutBuffer<Arrival> buf;
        const auto shown = replay(arrivals, 300 * kMs, &buf);
        QCOMPARE(buf.stats().skippedLate, quint64(0));
        for (size_t k = 1; k < shown.size(); ++k) {
            const qint64 gap = shown[k].presentUs - shown[k - 1].presentUs;
            const qint64 mediaGap = shown[k].mediaUs - shown[k - 1].mediaUs;
            QVERIFY2(gap >= qint64(double(mediaGap) * (1.0 - VideoPlayoutBuffer<Arrival>::kSlewRatio)) - kMs,
                     qPrintable(QStringLiteral("frame %1 shown %2 us after the previous, captured %3 apart")
                                    .arg(shown[k].index).arg(gap).arg(mediaGap)));
        }
        // ...and the latency did come back down once the burst aged out.
        const Shown& last = shown.back();
        QVERIFY(last.presentUs - last.arrivalUs <= 20 * kMs);
    }

    // ---- A late presentation timer ---------------------------------

    // The shape aClumpIsSpreadBackOutToCaptureCadence drives through the
    // real pipeline, on the fake clock: one on-time frame at the start of
    // a stream, then ten frames captured 33 ms apart arriving together
    // 300 ms later. The wake-up that should show the first of the clump
    // comes `lateUs` late — the GUI thread was busy (QML, the message
    // list, a loaded CI runner) or the OS deferred the timer — and every
    // wake after it is on time.
    //
    // With an on-time timer (row "on time") this pins that the start of
    // a stream is not the problem: the clump's first frame raises the
    // measured jitter as it arrives, so it is due a margin after arrival
    // and nothing is overdue. A late wake used to show only the NEWEST
    // due frame and skip the ones before it — 40 ms late cost a frame,
    // 110 ms cost two (the 9-of-11 macOS CI saw). While the ceiling has
    // room, lateness must cost latency, not pictures: all eleven shown,
    // in order, the rest of the clump still on capture cadence, and no
    // frame held past the ceiling.
    void aLateWakeDelaysRatherThanSkips_data() {
        QTest::addColumn<qint64>("lateUs");
        QTest::newRow("on time") << qint64(0);
        QTest::newRow("under one frame late") << 20 * kMs;
        QTest::newRow("one frame late") << 40 * kMs;
        QTest::newRow("three frames late") << 110 * kMs;
    }
    void aLateWakeDelaysRatherThanSkips() {
        QFETCH(qint64, lateUs);
        const qint64 ceiling = 400 * kMs;
        VideoPlayoutBuffer<int> buf;
        buf.setMaxDelayUs(ceiling);
        const qint64 media0 = qint64(1) << 40;
        const qint64 t0 = 1'000'000;
        std::vector<Shown> shown;
        auto popAt = [&](qint64 now) {
            if (auto i = buf.popDue(now))
                shown.push_back({*i, media0 + qint64(*i) * kFrameUs, now, 0});
        };
        buf.push(0, media0, t0);
        popAt(*buf.nextDueUs());
        const qint64 t1 = t0 + 300 * kMs;
        for (int i = 1; i <= 10; ++i) buf.push(i, media0 + qint64(i) * kFrameUs, t1);
        popAt(*buf.nextDueUs() + lateUs);
        while (auto due = buf.nextDueUs()) popAt(std::max(*due, shown.back().presentUs));

        QCOMPARE(buf.stats().skippedLate, quint64(0));
        QCOMPARE(int(shown.size()), 11);
        for (size_t k = 1; k < shown.size(); ++k) {
            QCOMPARE(shown[k].index, shown[k - 1].index + 1);
            // Every frame arrived by t1; the ceiling bounds the hold.
            QVERIFY(shown[k].presentUs - t1 <= ceiling);
        }
        // The late wake shows frame 1 late; the rest of the clump then
        // keeps its capture spacing (to the µs: the fake clock wakes
        // exactly), with two exceptions. Frame 2 can follow sooner, by
        // no more than frame 1 was late: a wake late by less than a
        // frame costs nothing, and absorbing it would add latency for no
        // picture saved. And a frame the ceiling stops short is held
        // exactly the ceiling (at 110 ms late, frame 10 would otherwise
        // be held 415 ms).
        for (size_t k = 2; k < shown.size(); ++k) {
            const qint64 gap = shown[k].presentUs - shown[k - 1].presentUs;
            const bool onCadence = std::llabs(gap - kFrameUs) <= 1;
            const bool afterShortLateWake = k == 2 && gap > 0 && gap >= kFrameUs - lateUs;
            const bool heldToCeiling = shown[k].presentUs - t1 == ceiling;
            QVERIFY2(onCadence || afterShortLateWake || heldToCeiling,
                     qPrintable(QStringLiteral("frame %1 shown %2 us after the previous")
                                    .arg(k).arg(gap)));
        }
    }

    // Past what the ceiling allows, the latency promise still wins over
    // completeness. The clump above holds its last frame ~305 ms, so a
    // 400 ms ceiling leaves ~95 ms to absorb a late wake; a wake 250 ms
    // late cannot be absorbed without holding frames past the ceiling,
    // so frames that are overdue even after using that room are skipped
    // (the old rule) — and every frame shown on time is still inside the
    // ceiling. Nothing disappears without being counted.
    void aWakeLaterThanTheCeilingAllowsStillSkips() {
        const qint64 ceiling = 400 * kMs;
        VideoPlayoutBuffer<int> buf;
        buf.setMaxDelayUs(ceiling);
        const qint64 media0 = qint64(1) << 40;
        const qint64 t0 = 1'000'000;
        std::vector<qint64> presentAt;
        buf.push(0, media0, t0);
        QVERIFY(buf.popDue(*buf.nextDueUs()));
        const qint64 t1 = t0 + 300 * kMs;
        for (int i = 1; i <= 10; ++i) buf.push(i, media0 + qint64(i) * kFrameUs, t1);
        qint64 now = *buf.nextDueUs() + 250 * kMs;
        int shownCount = 0;
        int last = 0;
        for (;;) {
            if (auto i = buf.popDue(now)) {
                QVERIFY(*i > last);
                last = *i;
                ++shownCount;
                presentAt.push_back(now);
            }
            const auto due = buf.nextDueUs();
            if (!due) break;
            now = std::max(now, *due);
        }
        QVERIFY(buf.stats().skippedLate > 0);
        QCOMPARE(quint64(shownCount) + buf.stats().skippedLate, quint64(10));
        QCOMPARE(last, 10);
        // After the late wake itself, no frame is held past the ceiling.
        for (size_t k = 1; k < presentAt.size(); ++k)
            QVERIFY(presentAt[k] - t1 <= ceiling);
    }

    // A GUI thread that stalls now and then (a stall of one to three
    // frame intervals every two seconds) on an ordinary stream: with the
    // ceiling leaving room for it, none of it may cost a picture, and
    // the latency it adds must not ratchet up over three minutes. Frames
    // that arrive during a stall are pushed when it ends, so the jitter
    // estimate learns the stalls too and holds ~110 ms — hence a ceiling
    // with room for a 105 ms stall on top of that.
    void occasionalStallsCostNoFrames() {
        const auto arrivals = share(5400, 0, 0, 10 * kMs);
        const qint64 ceiling = 300 * kMs;
        VideoPlayoutBuffer<Arrival> buf;
        buf.setMaxDelayUs(ceiling);
        std::vector<Shown> shown;
        size_t i = 0;
        qint64 clock = 0;
        int wakes = 0;
        while (i < arrivals.size() || buf.queued() > 0) {
            const qint64 next = i < arrivals.size() ? arrivals[i].arrivalUs
                                                    : std::numeric_limits<qint64>::max();
            const auto due = buf.nextDueUs();
            if (due && *due <= next) {
                // Every 60th wake is late by 35, 70 or 105 ms in turn.
                ++wakes;
                const qint64 late = wakes % 60 == 0 ? qint64((wakes / 60) % 3 + 1) * 35 * kMs : 0;
                clock = std::max(*due + late, clock);
                // A stalled thread cannot take arrivals either: push
                // whatever came in meanwhile first, as the event loop
                // would once it is free.
                while (i < arrivals.size() && arrivals[i].arrivalUs <= clock) {
                    buf.push(arrivals[i], arrivals[i].mediaUs, clock);
                    ++i;
                }
                if (auto a = buf.popDue(clock)) shown.push_back({a->index, a->mediaUs, clock, a->arrivalUs});
            } else {
                clock = arrivals[i].arrivalUs;
                buf.push(arrivals[i], arrivals[i].mediaUs, clock);
                ++i;
            }
        }
        QCOMPARE(buf.stats().skippedLate, quint64(0));
        QCOMPARE(int(shown.size()), 5400);
        for (size_t k = 1; k < shown.size(); ++k) QVERIFY(shown[k].index > shown[k - 1].index);
        // A frame can be held past the ceiling only by the stall itself.
        QVERIFY2(worstHeldUs(shown) <= ceiling + 105 * kMs,
                 qPrintable(QStringLiteral("held %1 us").arg(worstHeldUs(shown))));
        // No ratchet: what a stall adds is slewed back off, so the third
        // minute holds no longer on average than the second (the stall
        // pattern repeats every 180 wakes; both minutes see the same).
        auto meanHold = [&](size_t from, size_t to) {
            double sum = 0;
            for (size_t k = from; k < to; ++k) sum += double(shown[k].presentUs - shown[k].arrivalUs);
            return sum / double(to - from);
        };
        const double second = meanHold(1800, 3600);
        const double third = meanHold(3600, 5400);
        QVERIFY2(third - second <= 10.0 * kMs,
                 qPrintable(QStringLiteral("mean hold %1 us in minute 2 vs %2 us in minute 3")
                                .arg(second).arg(third)));
    }

    // ---- Clock unwrap ----------------------------------------------

    void rtpClockUnwrapsAcrossTheWrap() {
        MediaClockUnwrapper u(90000);
        uint32_t ts = 0xFFFFFFFFu - 3 * 3000;   // three frames before wrap
        qint64 prev = u.toUs(ts);
        for (int i = 0; i < 10; ++i) {
            ts += 3000;   // 33.3 ms at 90 kHz, wraps on the 4th step
            const qint64 now = u.toUs(ts);
            // 3000 ticks = 33 333.3 us; integer rounding alternates.
            QVERIFY2(std::llabs(now - prev - 33333) <= 1,
                     qPrintable(QStringLiteral("step %1 us").arg(now - prev)));
            prev = now;
        }
        // A reordered (older) frame comes out slightly earlier, not
        // thirteen hours later.
        QVERIFY(std::llabs(u.toUs(ts - 3000) - (prev - 33333)) <= 1);
        QVERIFY(prev > 0);
    }

    // ---- Policy ----------------------------------------------------

    void theCameraCeilingIsCappedForLipSync() {
        QCOMPARE(VideoReceivePipeline::effectivePlayoutCeilingMs(VideoStreamId::Screen, 400), 400);
        QCOMPARE(VideoReceivePipeline::effectivePlayoutCeilingMs(VideoStreamId::Camera, 400),
                 VideoReceivePipeline::kCameraPlayoutCapMs);
        QCOMPARE(VideoReceivePipeline::effectivePlayoutCeilingMs(VideoStreamId::Camera, 60), 60);
        QCOMPARE(VideoReceivePipeline::effectivePlayoutCeilingMs(VideoStreamId::Screen, 0), 0);
        QCOMPARE(VideoReceivePipeline::effectivePlayoutCeilingMs(VideoStreamId::Screen, -5), 0);
    }

    // ---- Pipeline wiring (wall clock) -------------------------------

    // Smoothing off must be the pre-buffer path: frames emitted straight
    // from the decode worker, as they decode — not bounced through the
    // GUI thread, not delayed.
    void smoothingOffIsTheOldPath() {
        VideoDecoder::setFactoryForTest([](VideoCodecKind, bool) {
            return std::make_unique<StubDecoder>();
        });
        VideoReceivePipeline pipe(QStringLiteral("@a:x"), VideoStreamId::Screen,
                                  VideoCodecKind::H264);
        pipe.setPlayoutMaxDelayMs(0);
        std::atomic<int> offGui{0};
        std::atomic<int> total{0};
        QThread* gui = QThread::currentThread();
        connect(&pipe, &VideoReceivePipeline::frameDecoded, &pipe,
                [&](const QString&, int, const QVideoFrame&) {
                    total.fetch_add(1);
                    if (QThread::currentThread() != gui) offGui.fetch_add(1);
                }, Qt::DirectConnection);
        for (int i = 0; i < 10; ++i)
            pipe.submitAccessUnit(i == 0 ? idrAu() : pAu(), false, false,
                                  MediaClockUnwrapper::kBaseUs + qint64(i) * kFrameUs);
        QTRY_COMPARE_WITH_TIMEOUT(total.load(), 10, 10000);
        QCOMPARE(offGui.load(), 10);
        VideoDecoder::setFactoryForTest({});
    }

    // Smoothing on, and ten frames arrive in ONE clump — the exact shape
    // of the queue behind a paced keyframe. They must come out on the GUI
    // thread, in order, spread over roughly their 300 ms of capture time
    // rather than all at once. (With smoothing off this clump reaches the
    // screen within a few milliseconds; that is the judder.)
    void aClumpIsSpreadBackOutToCaptureCadence() {
        VideoDecoder::setFactoryForTest([](VideoCodecKind, bool) {
            return std::make_unique<StubDecoder>();
        });
        VideoReceivePipeline pipe(QStringLiteral("@a:x"), VideoStreamId::Screen,
                                  VideoCodecKind::H264);
        pipe.setPlayoutMaxDelayMs(400);
        QThread* gui = QThread::currentThread();
        std::vector<qint64> starts;
        std::vector<qint64> at;
        bool allOnGui = true;
        QElapsedTimer clock;
        clock.start();
        connect(&pipe, &VideoReceivePipeline::frameDecoded, &pipe,
                [&](const QString&, int, const QVideoFrame& f) {
                    allOnGui = allOnGui && QThread::currentThread() == gui;
                    starts.push_back(f.startTime());
                    at.push_back(clock.elapsed());
                }, Qt::DirectConnection);
        // Prime the timeline with one on-time frame, then the clump: ten
        // frames captured 33 ms apart, all arriving 300 ms late together.
        const qint64 base = MediaClockUnwrapper::kBaseUs;
        pipe.submitAccessUnit(idrAu(), false, false, base);
        QTest::qWait(300);
        for (int i = 1; i <= 10; ++i)
            pipe.submitAccessUnit(pAu(), false, false, base + qint64(i) * kFrameUs);
        QTRY_COMPARE_WITH_TIMEOUT(int(starts.size()), 11, 10000);
        for (size_t k = 1; k < starts.size(); ++k) QVERIFY(starts[k] > starts[k - 1]);
        // Frames 1..10 span 300 ms of capture time. Presented on arrival
        // they span a few ms; smoothed, most of the 300. The bound is
        // loose on both sides on purpose — only a pipeline that clumps
        // (or stalls) can miss it.
        const qint64 span = at.back() - at[1];
        QVERIFY2(span >= 200 && span <= 2000,
                 qPrintable(QStringLiteral("clump presented over %1 ms").arg(span)));
        QVERIFY(allOnGui);
        VideoDecoder::setFactoryForTest({});
    }

    // Turning smoothing off mid-stream releases what is held at once
    // (the newest picture) instead of stranding it.
    void turningSmoothingOffReleasesTheNewestPicture() {
        VideoDecoder::setFactoryForTest([](VideoCodecKind, bool) {
            return std::make_unique<StubDecoder>();
        });
        VideoReceivePipeline pipe(QStringLiteral("@a:x"), VideoStreamId::Screen,
                                  VideoCodecKind::H264);
        pipe.setPlayoutMaxDelayMs(400);
        // Fake clock frozen: nothing ever falls due on its own.
        pipe.setPlayoutClockForTest([] { return qint64(0); });
        std::vector<qint64> starts;
        connect(&pipe, &VideoReceivePipeline::frameDecoded, &pipe,
                [&](const QString&, int, const QVideoFrame& f) { starts.push_back(f.startTime()); });
        const qint64 base = MediaClockUnwrapper::kBaseUs;
        // Media times far in the future of the frozen clock: all held.
        for (int i = 0; i < 4; ++i)
            pipe.submitAccessUnit(i == 0 ? idrAu() : pAu(), false, false,
                                  base + qint64(i) * kFrameUs);
        QTest::qWait(200);
        const size_t before = starts.size();
        pipe.setPlayoutMaxDelayMs(0);
        QCOMPARE(starts.size(), before + 1);
        QCOMPARE(starts.back(), base + 3 * kFrameUs);
        VideoDecoder::setFactoryForTest({});
    }
};

QTEST_MAIN(TestVideoPlayout)
#include "test_video_playout.moc"
