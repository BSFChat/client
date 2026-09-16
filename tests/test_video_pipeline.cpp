// Video pipeline logic that had no coverage: the routing decision that
// decides who gets H.264 and who gets JPEG, the receive side's keyframe
// recovery, the RTP reorder window, the rate controller's control law
// and the delivery-ratio estimate that feeds it, and the send pipeline's
// session rebuild.
//
// Everything here is either pure logic or a worker-thread pipeline, so
// there is no peer connection, no network, no capture device and no
// window. The codec-backed cases need a software H.264 encoder, which
// is the same condition test_video_codec builds under.

#include <QtTest/QtTest>
#include <QImage>
#include <QPainter>
#include <QSignalSpy>
#include <QVideoFrame>

#include "voice/PeerCaps.h"
#include "voice/video/DeliveryRatioEstimator.h"
#include "voice/video/RtpSeqTracker.h"
#include "voice/video/VideoRateController.h"
#include "voice/video/VideoReceivePipeline.h"
#include "voice/video/VideoSendPipeline.h"

namespace {

PeerCaps capsWith(bool videoRtp, const QStringList& codecs) {
    PeerCaps c;
    c.videoRtp = videoRtp;
    c.videoCodecs = codecs;
    return c;
}

// Minimal Annex-B access unit with one NAL of `nalType`. The receive
// pipeline scans for an IDR (type 5) to decide whether a unit may end a
// keyframe wait, so type 1 (non-IDR slice) is a P-frame as far as it is
// concerned.
QByteArray annexB(quint8 nalType, int payloadBytes = 32) {
    QByteArray au;
    au.append(char(0)).append(char(0)).append(char(0)).append(char(1));
    au.append(char(nalType & 0x1F));
    au.append(QByteArray(payloadBytes, char(0x42)));
    return au;
}

QVideoFrame makeTestFrame(int w, int h, int index) {
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(QColor(30, 32, 38));
    QPainter p(&img);
    p.fillRect(20 + index * 7, 40, 120, 80, QColor(200, 60, 60));
    p.fillRect(150, 60 + index * 5, 100, 60, QColor(60, 200, 120));
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

class TestVideoPipeline : public QObject {
    Q_OBJECT

private slots:

    // ---- S-1: who gets RTP video, who gets JPEG --------------------

    // The bug: "legacy" was keyed on the RTP track being closed. Video
    // m-lines are in every initial offer, so a peer with no H.264
    // decoder (Android without openh264) had an OPEN track, was treated
    // as capable, was skipped by the JPEG fan-out, and was sent H.264 it
    // cannot decode — i.e. it received nothing at all once a desktop
    // shared.
    void peerWithoutH264DecodeGetsJpegNeverH264() {
        const QString h264 = videoCodecIdH264();
        // The exact case from the audit: advertises the protocol
        // (video_rtp:1) with an EMPTY codec list.
        const PeerCaps android = capsWith(true, {});

        // Its track is open — that is the whole trap.
        QVERIFY(!peerCanReceiveRtpVideo(android, true, h264));
        QVERIFY(peerNeedsLegacyJpeg(android, true, /*trackOpen=*/true, h264));
        QVERIFY(peerNeedsLegacyJpeg(android, true, /*trackOpen=*/false, h264));
    }

    void capablePeerGetsH264OnceItsTrackIsOpen() {
        const QString h264 = videoCodecIdH264();
        const PeerCaps desktop = capsWith(true, {QStringLiteral("h264")});

        QVERIFY(peerCanReceiveRtpVideo(desktop, true, h264));
        // Transition gap: capable, but the renegotiation has not
        // finished — JPEG covers it so nobody stares at a placeholder.
        QVERIFY(peerNeedsLegacyJpeg(desktop, true, /*trackOpen=*/false, h264));
        QVERIFY(!peerNeedsLegacyJpeg(desktop, true, /*trackOpen=*/true, h264));
    }

    void legacyAndUnknownPeersAreJpeg() {
        const QString h264 = videoCodecIdH264();
        const PeerCaps legacy = capsWith(false, {});
        QVERIFY(!peerCanReceiveRtpVideo(legacy, true, h264));
        QVERIFY(peerNeedsLegacyJpeg(legacy, true, true, h264));

        // Caps not yet exchanged: assume nothing.
        const PeerCaps capable = capsWith(true, {QStringLiteral("h264")});
        QVERIFY(!peerCanReceiveRtpVideo(capable, /*capsKnown=*/false, h264));
        QVERIFY(peerNeedsLegacyJpeg(capable, false, true, h264));
    }

    void codecMatchingIgnoresCase() {
        const PeerCaps shouty = capsWith(true, {QStringLiteral("H264")});
        QVERIFY(peerCanReceiveRtpVideo(shouty, true, videoCodecIdH264()));
        const PeerCaps other = capsWith(true, {QStringLiteral("vp8")});
        QVERIFY(!peerCanReceiveRtpVideo(other, true, videoCodecIdH264()));
    }

    // ---- S-2: keyframe recovery ------------------------------------

    // A keyframe request can be lost (it used to ride a channel with
    // maxRetransmits=0). Nothing re-fired it, so the viewer stayed
    // frozen until the sender's periodic IDR, 10-30 s later. Every
    // P-frame dropped while waiting must now re-ask, throttled.
    void receiverReRequestsKeyframeWhileWaiting() {
        VideoReceivePipeline pipe(QStringLiteral("@bob:example.org"),
                                  VideoStreamId::Screen, VideoCodecKind::H264);
        pipe.setKeyframeRequestIntervalMs(20);   // 700 ms in production
        QSignalSpy spy(&pipe, &VideoReceivePipeline::keyframeNeeded);

        // A steady run of P-frames: the stream cannot start on any of
        // them, so all are dropped and the request keeps being re-sent.
        for (int i = 0; i < 12; ++i) {
            pipe.submitAccessUnit(annexB(/*nalType=*/1));
            QTest::qWait(12);
        }
        QTRY_VERIFY_WITH_TIMEOUT(pipe.keyframeRequests() >= 3, 2000);
        QCOMPARE(pipe.decodedFrames(), quint64(0));
        QCOMPARE(pipe.droppedAus(), quint64(12));
        QVERIFY(spy.count() >= 3);
    }

    // The throttle is the other half: under sustained loss every gap
    // would otherwise fire a request, and request bursts are read by
    // the sender as congestion.
    void keyframeRequestsAreThrottled() {
        VideoReceivePipeline pipe(QStringLiteral("@bob:example.org"),
                                  VideoStreamId::Screen, VideoCodecKind::H264);
        pipe.setKeyframeRequestIntervalMs(5000);
        // Below the decode backlog cap, so every drop here is the
        // keyframe gate rather than an overflow flush.
        for (int i = 0; i < 10; ++i) {
            pipe.submitAccessUnit(annexB(1));
            QTest::qWait(2);
        }
        QTRY_COMPARE_WITH_TIMEOUT(pipe.droppedAus(), quint64(10), 2000);
        QCOMPARE(pipe.keyframeRequests(), quint64(1));
    }

    // ---- S-4: reorder window ---------------------------------------

    void inOrderPacketsNeverReportLoss() {
        RtpSeqTracker t;
        for (uint16_t seq = 1000; seq < 1100; ++seq)
            QVERIFY(!t.observe(seq, seq));
        QVERIFY(!t.hasPendingGap());
    }

    // The point of the window: 1,2,4,3,5 is one packet arriving late,
    // not a loss. It used to cost a dropped access unit and an IDR.
    void singlePacketReorderIsNotLoss() {
        RtpSeqTracker t;
        QVERIFY(!t.observe(1, 0));
        QVERIFY(!t.observe(2, 1));
        QVERIFY(!t.observe(4, 2));   // gap opens, verdict held
        QVERIFY(t.hasPendingGap());
        QVERIFY(!t.observe(3, 3));   // the hole fills itself
        QVERIFY(!t.hasPendingGap());
        QVERIFY(!t.observe(5, 4));
        QVERIFY(!t.poll(5));
    }

    void genuineLossIsStillReportedOnce() {
        RtpSeqTracker t;
        QVERIFY(!t.observe(1, 0));
        QVERIFY(!t.observe(2, 1));
        QVERIFY(!t.observe(4, 2));   // 3 is really gone
        QVERIFY(!t.observe(5, 3));   // one packet into the window
        QVERIFY(t.observe(6, 4));    // two packets: confirmed
        QVERIFY(!t.hasPendingGap());
        // Not re-reported for the same hole.
        QVERIFY(!t.observe(7, 5));
        QVERIFY(!t.observe(8, 6));
    }

    // A gap at the tail of a burst must not wait for the next burst.
    void pendingGapExpiresOnTime() {
        RtpSeqTracker t;
        QVERIFY(!t.observe(1, 0));
        QVERIFY(!t.observe(3, 0));
        QVERIFY(!t.poll(5));        // inside the 10 ms window
        QVERIFY(t.poll(11));        // window elapsed → loss
        QVERIFY(!t.poll(50));       // reported once
    }

    void duplicatesAndLargeGapsBehave() {
        RtpSeqTracker dup;
        QVERIFY(!dup.observe(10, 0));
        QVERIFY(!dup.observe(11, 1));
        QVERIFY(!dup.observe(11, 2));   // duplicate
        QVERIFY(!dup.observe(10, 3));   // very late duplicate
        QVERIFY(!dup.observe(12, 4));

        RtpSeqTracker big;
        QVERIFY(!big.observe(100, 0));
        // 40 packets missing is not reordering under any scheduler.
        QVERIFY(big.observe(141, 1));
    }

    void sequenceWraparoundIsNotAGap() {
        RtpSeqTracker t;
        QVERIFY(!t.observe(65534, 0));
        QVERIFY(!t.observe(65535, 1));
        QVERIFY(!t.observe(0, 2));
        QVERIFY(!t.observe(1, 3));
        QVERIFY(!t.hasPendingGap());
    }

    // ---- S-3: delivery ratio ---------------------------------------

    void steadyDeliveryReadsAsClean() {
        DeliveryRatioEstimator e;
        double ratio = 0.0;
        quint64 rx = 0, tx = 0;
        QVERIFY(!e.update(rx, tx, ratio));          // first report: no basis
        for (int i = 0; i < 5; ++i) {
            tx += 50000;
            rx += 50000;
            if (e.update(rx, tx, ratio)) QCOMPARE(ratio, 1.0);
        }
    }

    // The S-3 case, exactly: a keyframe lands near the end of a report
    // window, so its bytes are counted as SENT in that window but are
    // still in flight when the peer's report is written. The naive
    // ratio reads ~0.5 and the controller cuts the bitrate 25 % and
    // steps the resolution down — on every keyframe.
    void keyframeStraddlingAReportBoundaryIsNotLoss() {
        DeliveryRatioEstimator e;
        double ratio = 0.0;
        quint64 rx = 0, tx = 0;

        tx += 40000; rx += 40000;
        e.update(rx, tx, ratio);                    // priming report

        tx += 40000; rx += 40000;
        QVERIFY(e.update(rx, tx, ratio));
        QCOMPARE(ratio, 1.0);

        // Window 3: 40 KB of P-frames plus a 60 KB IDR at the very end.
        // The peer has only received the P-frames when it reports.
        const quint64 windowTx = 100000;
        tx += windowTx; rx += 40000;
        QVERIFY(e.update(rx, tx, ratio));
        const double naive = 40000.0 / double(windowTx);
        QVERIFY2(naive < 0.5, "the naive ratio really would have panicked");
        QVERIFY2(ratio > 0.97, qPrintable(QStringLiteral(
            "in-flight IDR read as loss: ratio=%1").arg(ratio)));

        // Window 4: the IDR lands. Graded against window 3's 100 KB.
        tx += 40000; rx += 60000 + 40000;
        QVERIFY(e.update(rx, tx, ratio));
        QCOMPARE(ratio, 1.0);
    }

    void realLossStillShowsUp() {
        DeliveryRatioEstimator e;
        double ratio = 0.0;
        quint64 rx = 0, tx = 0;
        tx += 50000; rx += 50000;
        e.update(rx, tx, ratio);
        // A path dropping 30 % of everything, steadily.
        for (int i = 0; i < 4; ++i) {
            tx += 50000;
            rx += 35000;
            if (e.update(rx, tx, ratio) && i > 0)
                QVERIFY2(ratio < 0.75, qPrintable(QString::number(ratio)));
        }
    }

    void counterResetIsNotGradedAsTotalLoss() {
        DeliveryRatioEstimator e;
        double ratio = 0.0;
        e.update(100000, 100000, ratio);
        e.update(150000, 150000, ratio);
        // Peer restarted its stream: counters go backwards.
        QVERIFY(!e.update(0, 150000, ratio));
        QVERIFY(!e.update(5000, 155000, ratio));   // rebuilding the basis
    }

    // ---- Rate controller control law -------------------------------

    void cleanReportsClimbAndLossBacksOff() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 6000, 30, 1920);
        rc.setActive(true);
        const int start = rc.targetKbps();

        // Two clean ticks are the probe threshold.
        for (int i = 0; i < 4; ++i) {
            rc.reportDeliveryRatio(QStringLiteral("@a:x"), 1.0);
            rc.tick();
        }
        QVERIFY2(rc.targetKbps() > start, "clean delivery must probe upward");

        const int beforeCut = rc.targetKbps();
        rc.reportDeliveryRatio(QStringLiteral("@a:x"), 0.4);
        rc.tick();
        QVERIFY2(rc.targetKbps() < beforeCut, "measured loss must back off");
    }

    // A share is only as smooth as its worst receiver.
    void worstPeerGovernsTheRate() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 6000, 30, 1920);
        rc.setActive(true);
        for (int i = 0; i < 4; ++i) {
            rc.reportDeliveryRatio(QStringLiteral("@a:x"), 1.0);
            rc.tick();
        }
        const int before = rc.targetKbps();
        rc.reportDeliveryRatio(QStringLiteral("@a:x"), 1.0);
        rc.reportDeliveryRatio(QStringLiteral("@b:x"), 0.5);
        rc.tick();
        QVERIFY(rc.targetKbps() < before);
    }

    void keyframeStormCountsAsCongestion() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 6000, 30, 1920);
        rc.setActive(true);
        rc.reportDeliveryRatio(QStringLiteral("@a:x"), 1.0);
        rc.tick();
        const int before = rc.targetKbps();
        rc.reportDeliveryRatio(QStringLiteral("@a:x"), 1.0);
        for (int i = 0; i < 3; ++i) rc.reportKeyframeRequest();
        rc.tick();
        QVERIFY2(rc.targetKbps() < before,
                 "a keyframe storm is a congestion signal even at ratio 1.0");
    }

    void resolutionLadderStepsDownWhenBitrateCannotSustainTheSize() {
        VideoRateController rc(VideoStreamId::Screen);
        // A ceiling far too low for 1920 px at 30 fps: the floor
        // bits-per-pixel is unreachable, so the size must give way.
        rc.setEnvelope(250, 400, 30, 1920);
        rc.setActive(true);
        const int fullEdge = rc.longEdge();
        for (int i = 0; i < 6; ++i) {
            rc.reportDeliveryRatio(QStringLiteral("@a:x"), 1.0);
            rc.tick();
        }
        QVERIFY2(rc.longEdge() < fullEdge,
                 "resolution, not fps, is what gives way on screen content");
        QVERIFY(rc.longEdge() >= 160);
    }

    // Without reports the controller has no evidence, so it must hold
    // rather than climb on faith.
    void blindModeHoldsBelowTheCeiling() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 40000, 30, 1920);
        rc.setActive(true);
        // Past the startup grace, with no delivery reports at all.
        QTest::qWait(3200);
        rc.tick();
        rc.tick();
        QVERIFY2(rc.targetKbps() <= 8000,
                 qPrintable(QStringLiteral("blind at %1 kbps")
                                .arg(rc.targetKbps())));
    }

    // ---- S-11 / S-16: send pipeline --------------------------------

    void sendPipelineOpensOnAKeyframeAndRebuildsOnResize() {
        VideoSendPipeline pipe(VideoStreamId::Screen);
        QSignalSpy spy(&pipe, &VideoSendPipeline::encodedFrameReady);

        EncoderConfig cfg;
        cfg.codec = VideoCodecKind::H264;
        cfg.width = cfg.height = 320;
        cfg.fps = 15;
        cfg.targetBitrateKbps = 1500;
        cfg.maxBitrateKbps = 2000;
        cfg.keyframeIntervalSec = 10;
        cfg.screenContent = true;
        pipe.configure(cfg);

        pipe.submitFrame(makeTestFrame(320, 240, 0), 0);
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 5000);
        if (spy.isEmpty()) QSKIP("no usable H.264 encoder on this host");
        {
            const auto first = spy.at(0).at(1).value<EncodedFrame>();
            QVERIFY2(first.keyframe, "a new encode session must open on an IDR");
        }

        // Steady state produces deltas.
        for (int i = 1; i < 6; ++i) {
            pipe.submitFrame(makeTestFrame(320, 240, i), i * 66000);
            QTest::qWait(40);
        }
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 3, 5000);
        bool sawDelta = false;
        for (int i = 1; i < spy.count(); ++i)
            if (!spy.at(i).at(1).value<EncodedFrame>().keyframe) sawDelta = true;
        QVERIFY2(sawDelta, "every frame a keyframe means inter-coding is off");

        // S-11's mechanism: an explicit request produces an IDR, which
        // is what a share restart (and a late joiner) relies on.
        const int beforeForce = spy.count();
        pipe.forceKeyframe();
        pipe.submitFrame(makeTestFrame(320, 240, 7), 500000);
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() > beforeForce, 5000);
        QVERIFY2(spy.at(beforeForce).at(1).value<EncodedFrame>().keyframe,
                 "forceKeyframe() must produce an IDR on the next frame");

        // A source resize rebuilds the session, which must also open on
        // an IDR — receivers cannot carry references across it.
        const int beforeResize = spy.count();
        EncoderConfig bigger = cfg;
        bigger.width = bigger.height = 480;
        pipe.configure(bigger);
        pipe.submitFrame(makeTestFrame(480, 360, 8), 600000);
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() > beforeResize, 5000);
        const auto rebuilt = spy.at(beforeResize).at(1).value<EncodedFrame>();
        QVERIFY2(rebuilt.keyframe, "a rebuilt session must open on an IDR");
    }

    // S-16: with the coalescing flag cleared inside the lock that
    // guards the pending slot, a frame submitted while the worker is
    // draining can no longer be stranded there. Hammer the slot from
    // the submitting side and require that the LAST frame handed over
    // is always encoded.
    void lastSubmittedFrameIsNeverStranded() {
        VideoSendPipeline pipe(VideoStreamId::Screen);
        QSignalSpy spy(&pipe, &VideoSendPipeline::encodedFrameReady);
        EncoderConfig cfg;
        cfg.codec = VideoCodecKind::H264;
        cfg.width = cfg.height = 160;
        cfg.fps = 30;
        cfg.targetBitrateKbps = 800;
        cfg.maxBitrateKbps = 1000;
        cfg.screenContent = true;
        pipe.configure(cfg);

        pipe.submitFrame(makeTestFrame(160, 120, 0), 0);
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 5000);
        if (spy.isEmpty()) QSKIP("no usable H.264 encoder on this host");

        for (int round = 0; round < 20; ++round) {
            const int before = spy.count();
            // Back-to-back submits with no event-loop turn between
            // them: the worker is mid-drain for at least some of these.
            for (int i = 0; i < 4; ++i)
                pipe.submitFrame(makeTestFrame(160, 120, round * 4 + i),
                                 (round * 4 + i) * 33000);
            QTRY_VERIFY_WITH_TIMEOUT(spy.count() > before, 5000);
        }
    }
};

QTEST_MAIN(TestVideoPipeline)
#include "test_video_pipeline.moc"
