// Receive-side cadence probe: WHERE does video judder come from?
//
// Not a pass/fail test (it is built, never registered with ctest). It
// exists so the choppiness report ("really good quality, but very
// choppy") could be diagnosed with numbers instead of guesses, and so
// the next person to touch the video path can re-run the same numbers.
//
// What it does, per scenario:
//   1. Two real PeerConnectionManagers over loopback DTLS-SRTP — the same
//      bring-up as test_video_rtp_loopback, so the whole production RTP
//      chain is in play: packetizer, PacedRtpSender at the ceiling
//      ScreenShareController would set (1.5 x target), SRTP, the
//      depacketizer, and the queued hop onto the GUI thread.
//   2. A sender on a precise timer produces access units sized like a
//      real share at the given fps / bitrate, with periodic IDRs of a
//      measured field size, stamping each with the real capture clock.
//   3. The receiver records when each AU reaches the GUI thread — the
//      moment production hands it to VideoReceivePipeline — and the
//      media timestamp it arrived under.
//   4. Those arrivals are replayed through VideoPlayoutBuffer at several
//      ceilings with an ideal presentation timer, which is what the
//      screen would have shown (decode time is measured separately, in
//      `decode` mode, and is small against the arrival jitter).
//
// Usage:  probe_video_cadence            (all transport scenarios)
//         probe_video_cadence decode     (decode time, real codecs)
//
// Loopback has no network jitter, so every irregularity measured here
// is added by OUR pipeline, not by the internet. A real path only adds
// to it.
//
// Findings, development machine (Apple Silicon, 2026-09-21):
//
//   * Decode is not the problem: 1080p H.264 decodes in ~1 ms (P) and
//     2-3 ms (IDR), hardware or software.
//   * 30 fps, 4 Mbps (pacer ceiling 6 Mbps), 150 KB IDRs: every IDR
//     reaches the GUI thread ~165 ms late and the P-frames queued behind
//     it drain in a clump — 5 stalls and 21 clumped frames in 16 s,
//     worst on-screen irregularity 165 ms, on a link with NO jitter.
//     Same at 7.7 Mbps with 293 KB IDRs. 60 fps: ~96 ms per IDR.
//   * Through the playout buffer: ceiling 150 ms -> worst irregularity
//     16-30 ms (one frame per keyframe); 250 ms -> < 2 ms, holding
//     ~170 ms; 100 ms -> 65 ms; 50 ms -> 115 ms.
//   * 5 fps (the fresh-install default: Settings::screenShareFps falls
//     back to legacy preset 1 = 5 fps) at 4 Mbps: 86 of 110 frames never
//     arrive and the survivors are seconds late. PacedRtpSender only
//     sends on outgoing() and refills at most kMaxBurstSeconds (50 ms)
//     of budget per call, so its throughput is ceiling x fps x 0.05 —
//     below the ceiling at any fps under 20. That is send-side, and no
//     receive buffer can recover frames that were never sent.

#include "voice/PeerConnectionManager.h"
#include "voice/video/VideoDecoder.h"
#include "voice/video/VideoEncoder.h"
#include "voice/video/FrameConverter.h"
#include "voice/video/VideoPlayoutBuffer.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QPainter>
#include <QThread>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <vector>

namespace {

QElapsedTimer g_clock;
qint64 nowUs() { return g_clock.nsecsElapsed() / 1000; }

bool pumpUntil(const std::function<bool()>& predicate, int timeoutMs) {
    QDeadlineTimer deadline(timeoutMs);
    while (!predicate()) {
        if (deadline.hasExpired()) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

void pumpFor(int ms) {
    QDeadlineTimer deadline(ms);
    while (!deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

PeerCaps videoCaps() {
    PeerCaps caps;
    caps.videoRtp = true;
    caps.controlDc = true;
    caps.videoCodecs = {QStringLiteral("h264")};
    caps.h264ProfilesDecode = {QStringLiteral("cb")};
    caps.h264ProfilesEncode = {QStringLiteral("cb")};
    return caps;
}

void wire(PeerConnectionManager* a, PeerConnectionManager* b) {
    QObject::connect(a, &PeerConnectionManager::localDescriptionReady, b,
        [b](const std::string& type, const std::string& sdp) {
            if (type == "offer") {
                if (b->initialNegotiationDone()) b->applyNegotiateOffer(sdp);
                else b->applyOffer(sdp);
            } else {
                if (b->initialNegotiationDone()) b->applyNegotiateAnswer(sdp);
                else b->applyAnswer(sdp);
            }
        });
    QObject::connect(a, &PeerConnectionManager::localCandidateReady, b,
        [b](const std::string& c, const std::string& mid) {
            b->addRemoteCandidate(c, mid);
        });
}

// Synthetic Annex-B AU of `bytes` total, frame index encoded in the
// first payload bytes (7-bit clean, so no accidental start codes).
QByteArray makeAu(bool keyframe, int bytes, int index) {
    QByteArray au;
    auto sc = [&au]() { au.append(3, '\0'); au.append(char(1)); };
    if (keyframe) {
        sc();
        for (int b : {0x67, 0x42, 0x00, 0x1E, 0x8D, 0x68, 0x05, 0x00, 0x5B, 0xA1})
            au.append(char(b));
        sc();
        for (int b : {0x68, 0xCE, 0x3C, 0x80}) au.append(char(b));
    }
    sc();
    au.append(char(keyframe ? 0x65 : 0x41));
    au.append(char(0x40 | ((index >> 12) & 0x3F)));
    au.append(char(0x40 | ((index >> 6) & 0x3F)));
    au.append(char(0x40 | (index & 0x3F)));
    const int pad = std::max(0, bytes - int(au.size()));
    for (int i = 0; i < pad; ++i) au.append(char(((i * 31) & 0x3F) | 0x40));
    return au;
}

int auIndex(const QByteArray& au) {
    // Find the slice NAL header (0x65 / 0x41 after a start code), read
    // the three index bytes behind it.
    for (int i = 0; i + 7 < au.size(); ++i) {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 0 && au[i + 3] == 1
            && (au[i + 4] == char(0x65) || au[i + 4] == char(0x41))) {
            return ((au[i + 5] & 0x3F) << 12) | ((au[i + 6] & 0x3F) << 6)
                 | (au[i + 7] & 0x3F);
        }
    }
    return -1;
}

struct Summary {
    double mean = 0, sd = 0, p50 = 0, p95 = 0, max = 0;
};

Summary summarise(std::vector<double> v) {
    Summary s;
    if (v.empty()) return s;
    double sum = 0;
    for (double x : v) sum += x;
    s.mean = sum / double(v.size());
    double var = 0;
    for (double x : v) var += (x - s.mean) * (x - s.mean);
    s.sd = std::sqrt(var / double(v.size()));
    std::sort(v.begin(), v.end());
    s.p50 = v[v.size() / 2];
    s.p95 = v[std::min(v.size() - 1, size_t(double(v.size()) * 0.95))];
    s.max = v.back();
    return s;
}

struct Arrival {
    int index;
    qint64 mediaUs;     // sender capture clock
    qint64 arrivalUs;   // GUI-thread arrival (same clock: one process)
    bool keyframe;
};

struct Scenario {
    const char* name;
    int fps;
    int targetKbps;
    int idrBytes;
    int gopSec;
    int seconds;
    // Synthetic GUI-thread load: every loadPeriodMs, block the main
    // thread for loadMs (a QML relayout / message-list scroll stand-in).
    int loadPeriodMs = 0;
    int loadMs = 0;
};

// Replay arrivals through the playout buffer with an ideal timer.
// Returns presented (mediaUs, presentUs) pairs.
struct Presented { qint64 mediaUs; qint64 presentUs; qint64 arrivalUs; };

std::vector<Presented> replay(const std::vector<Arrival>& arrivals,
                              qint64 ceilingUs, quint64* skipped) {
    std::vector<Presented> out;
    if (ceilingUs == 0) {
        for (const auto& a : arrivals) out.push_back({a.mediaUs, a.arrivalUs, a.arrivalUs});
        *skipped = 0;
        return out;
    }
    struct P { qint64 media; qint64 arrival; };
    VideoPlayoutBuffer<P> buf;
    buf.setMaxDelayUs(ceilingUs);
    size_t i = 0;
    qint64 clock = 0;
    while (i < arrivals.size() || buf.queued() > 0) {
        const qint64 nextArrival = i < arrivals.size()
            ? arrivals[i].arrivalUs : std::numeric_limits<qint64>::max();
        const auto due = buf.nextDueUs();
        if (due && *due <= nextArrival) {
            // A frame that was already late when it arrived is shown at
            // once, which is what the pipeline's timer does too.
            const qint64 t = std::max(*due, clock);
            if (auto p = buf.popDue(t)) out.push_back({p->media, t, p->arrival});
        } else {
            buf.push({arrivals[i].mediaUs, arrivals[i].arrivalUs},
                     arrivals[i].mediaUs, arrivals[i].arrivalUs);
            clock = arrivals[i].arrivalUs;
            ++i;
        }
    }
    *skipped = buf.stats().skippedLate + buf.stats().overflowed;
    return out;
}

void report(const char* label, const std::vector<Presented>& shown,
            int fps, quint64 skipped) {
    const double nominal = 1000.0 / fps;
    std::vector<double> gaps, cadenceErr, held;
    int clumped = 0, stalls = 0;
    for (size_t k = 1; k < shown.size(); ++k) {
        const double gap = double(shown[k].presentUs - shown[k - 1].presentUs) / 1000.0;
        const double mediaGap = double(shown[k].mediaUs - shown[k - 1].mediaUs) / 1000.0;
        gaps.push_back(gap);
        // How far the on-screen spacing departs from the capture spacing
        // — zero is perfectly smooth motion whatever the frame rate.
        cadenceErr.push_back(std::fabs(gap - mediaGap));
        if (gap < nominal * 0.25) ++clumped;
        if (gap > nominal * 1.5) ++stalls;
    }
    for (const auto& p : shown) held.push_back(double(p.presentUs - p.arrivalUs) / 1000.0);
    const Summary g = summarise(gaps), e = summarise(cadenceErr), h = summarise(held);
    std::printf("  %-14s shown=%4zu skipped=%3llu | interval ms mean %6.1f sd %5.1f max %6.1f"
                " | cadence err ms p50 %5.1f p95 %6.1f max %6.1f | clumped %3d stalls %3d"
                " | added delay ms p50 %5.1f max %5.1f\n",
                label, shown.size(), static_cast<unsigned long long>(skipped),
                g.mean, g.sd, g.max, e.p50, e.p95, e.max, clumped, stalls, h.p50, h.max);
}

int runTransport(const Scenario& sc) {
    rtc::Configuration cfg;
    cfg.bindAddress = "127.0.0.1";
    auto* tx = new PeerConnectionManager(QStringLiteral("@rx:probe"),
                                         QStringLiteral("call-probe"), cfg);
    auto* rx = new PeerConnectionManager(QStringLiteral("@tx:probe"),
                                         QStringLiteral("call-probe"), cfg);
    wire(tx, rx);
    wire(rx, tx);
    tx->setRemoteCaps(videoCaps());
    rx->setRemoteCaps(videoCaps());
    tx->createOffer();
    if (!pumpUntil([&] { return tx->isChannelOpen() && rx->isChannelOpen(); }, 45000)) {
        std::fprintf(stderr, "bring-up failed\n");
        return 2;
    }
    tx->ensureVideoTracks();
    if (!pumpUntil([&] {
            return tx->hasVideoTrackOpen(VideoStreamId::Screen)
                && rx->hasVideoTrackOpen(VideoStreamId::Screen);
        }, 15000)) {
        std::fprintf(stderr, "video track never opened\n");
        return 2;
    }
    // What ScreenShareController sets: maxBitrate = 1.5 x target.
    const int ceilingKbps = sc.targetKbps + sc.targetKbps / 2;
    tx->setVideoPacingCeilingKbps(VideoStreamId::Screen, ceilingKbps);

    std::map<int, std::pair<qint64, bool>> sent;   // index -> (capture, kf)
    std::vector<Arrival> arrivals;
    int rawRx = 0;
    QObject::connect(rx, &PeerConnectionManager::videoFrameReceived, rx,
        [&](int stream, const QByteArray& au, bool, int) {
            if (stream != int(VideoStreamId::Screen)) return;
            ++rawRx;
            const int idx = auIndex(au);
            auto it = sent.find(idx);
            if (it == sent.end()) return;
            arrivals.push_back({idx, it->second.first, nowUs(), it->second.second});
        });

    const int pBytes = sc.targetKbps * 1000 / 8 / sc.fps;
    const int gopFrames = sc.gopSec * sc.fps;
    int index = 0;
    QTimer sender;
    sender.setTimerType(Qt::PreciseTimer);
    sender.setInterval(1000 / sc.fps);
    QObject::connect(&sender, &QTimer::timeout, [&]() {
        const bool kf = index % gopFrames == 0;
        EncodedFrame f;
        f.codec = VideoCodecKind::H264;
        f.keyframe = kf;
        f.captureTimeUs = nowUs();
        f.data = makeAu(kf, kf ? sc.idrBytes : pBytes, index);
        f.width = 1920;
        f.height = 1080;
        sent[index] = {f.captureTimeUs, kf};
        tx->sendVideoFrame(VideoStreamId::Screen, f);
        ++index;
    });
    QTimer load;
    if (sc.loadPeriodMs > 0) {
        load.setInterval(sc.loadPeriodMs);
        QObject::connect(&load, &QTimer::timeout, [&]() {
            QThread::msleep(sc.loadMs);
        });
        load.start();
    }
    sender.start();
    pumpFor(sc.seconds * 1000);
    sender.stop();
    load.stop();
    pumpFor(1500);

    std::printf("\n== %s: %d fps, target %d kbps (pacer ceiling %d), P %d B, "
                "IDR %d B every %d s, %d s%s\n",
                sc.name, sc.fps, sc.targetKbps, ceilingKbps, pBytes, sc.idrBytes,
                sc.gopSec, sc.seconds,
                sc.loadPeriodMs ? " + synthetic GUI load" : "");
    std::sort(arrivals.begin(), arrivals.end(),
              [](const Arrival& a, const Arrival& b) { return a.arrivalUs < b.arrivalUs; });
    std::vector<double> transit;
    for (const auto& a : arrivals) transit.push_back(double(a.arrivalUs - a.mediaUs) / 1000.0);
    const Summary t = summarise(transit);
    std::printf("  tx frames %llu, raw rx %d\n",
                static_cast<unsigned long long>(tx->videoTxFrames(VideoStreamId::Screen)), rawRx);
    std::printf("  sent %d, arrived %zu (lost %d); capture->GUI-thread transit ms: "
                "p50 %.1f p95 %.1f max %.1f\n",
                index, arrivals.size(), index - int(arrivals.size()), t.p50, t.p95, t.max);
    // Transit right after each keyframe: how long the pacer holds the
    // frames queued behind an IDR.
    for (size_t k = 0; k < arrivals.size(); ++k) {
        if (!arrivals[k].keyframe) continue;
        double worst = 0;
        int n = 0;
        for (size_t m = k; m < arrivals.size() && n < sc.fps; ++m, ++n)
            worst = std::max(worst, double(arrivals[m].arrivalUs - arrivals[m].mediaUs) / 1000.0);
        std::printf("  IDR #%d: its transit %.1f ms; worst transit in the next second %.1f ms\n",
                    arrivals[k].index,
                    double(arrivals[k].arrivalUs - arrivals[k].mediaUs) / 1000.0, worst);
    }
    for (int ceilingMs : {0, 50, 100, 150, 250, 400}) {
        quint64 skipped = 0;
        const auto shown = replay(arrivals, qint64(ceilingMs) * 1000, &skipped);
        char label[32];
        std::snprintf(label, sizeof label, ceilingMs ? "buffer<=%dms" : "today (off)", ceilingMs);
        report(label, shown, sc.fps, skipped);
    }
    delete tx;
    delete rx;
    pumpFor(300);
    return 0;
}

QVideoFrame syntheticFrame(int w, int h, int i) {
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(QColor(30, 32, 38));
    QPainter p(&img);
    for (int y = 0; y < h; y += 18)
        p.fillRect(0, y, w, 2, QColor(220, 220, 225));
    p.fillRect((i * 23) % (w - 300), 200, 300, 200, QColor(200, 60, 60));
    p.setPen(Qt::white);
    for (int line = 0; line < 30; ++line)
        p.drawText(40, 40 + line * 30, QStringLiteral("frame %1 line %2 the quick brown fox").arg(i).arg(line));
    p.end();
    QVideoFrameFormat fmt(img.size(), QVideoFrameFormat::pixelFormatFromImageFormat(img.format()));
    QVideoFrame frame(fmt);
    if (!frame.map(QVideoFrame::WriteOnly)) return {};
    for (int row = 0; row < img.height(); ++row)
        memcpy(frame.bits(0) + row * frame.bytesPerLine(0), img.constScanLine(row),
               size_t(img.bytesPerLine()));
    frame.unmap();
    return frame;
}

int runDecode() {
    const int w = 1920, h = 1080, n = 90;
    auto enc = VideoEncoder::create(VideoCodecKind::H264, /*preferHardware=*/true);
    EncoderConfig cfg;
    cfg.width = w; cfg.height = h; cfg.fps = 30;
    cfg.targetBitrateKbps = 6000; cfg.maxBitrateKbps = 9000;
    if (!enc || !enc->init(cfg)) { std::fprintf(stderr, "no encoder\n"); return 2; }
    std::vector<EncodedFrame> aus;
    for (int i = 0; i < n; ++i) {
        PlanarFrame pf = FrameConverter::toI420(syntheticFrame(w, h, i), w, qint64(i) * 33333);
        EncodedFrame out;
        if (enc->encode(pf, i % 30 == 0, out) && !out.data.isEmpty()) aus.push_back(out);
    }
    for (bool hw : {true, false}) {
        auto dec = VideoDecoder::create(VideoCodecKind::H264, hw);
        if (!dec || !dec->init(VideoCodecKind::H264)) continue;
        std::vector<double> idr, p;
        for (const auto& au : aus) {
            QVideoFrame f;
            QElapsedTimer t; t.start();
            dec->decode(au.data, f);
            (au.keyframe ? idr : p).push_back(double(t.nsecsElapsed()) / 1e6);
        }
        const Summary si = summarise(idr), sp = summarise(p);
        std::printf("decode 1080p H.264 (%s): IDR ms p50 %.2f max %.2f | P ms p50 %.2f p95 %.2f max %.2f"
                    " (%zu AUs, IDR ~%d KB)\n",
                    hw ? "hardware-preferred" : "software", si.p50, si.max, sp.p50, sp.p95,
                    sp.max, aus.size(), aus.empty() ? 0 : int(aus.front().data.size() / 1024));
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // QGuiApplication for QPainter's text in `decode` mode; run it with
    // QT_QPA_PLATFORM=offscreen (no window is ever created).
    QGuiApplication app(argc, argv);
    g_clock.start();
    if (argc > 1 && QByteArray(argv[1]) == "decode") return runDecode();

    // IDR sizes are the field numbers recorded in PacedRtpSender's
    // comment (133 KB and 293 KB at 7.7 Mbps for a 1080p screen).
    const Scenario scenarios[] = {
        {"default fps", 5, 4000, 150'000, 10, 22},
        {"30 fps", 30, 4000, 150'000, 5, 16},
        {"30 fps big IDR", 30, 7700, 293'000, 5, 16},
        {"60 fps", 60, 8000, 200'000, 5, 16},
        {"30 fps GUI load", 30, 4000, 150'000, 5, 16, 50, 12},
    };
    int rc = 0;
    for (const auto& sc : scenarios) rc |= runTransport(sc);
    rtc::Cleanup().wait();
    return rc;
}
