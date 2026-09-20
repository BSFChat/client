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
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include <atomic>
#include <cmath>
#include <memory>

#include "core/AppProfile.h"
#include "voice/PeerCaps.h"
#include "voice/video/ReceiverReportEstimator.h"
#include "voice/video/VideoDecodeHealth.h"
#include "voice/video/VideoDecoder.h"
#include "voice/video/RtpSeqTracker.h"
#include "voice/video/VideoCodecSelect.h"
#include "voice/video/VideoRatePolicy.h"
#include "voice/video/VideoRateController.h"
#include "voice/video/VideoReceivePipeline.h"
#include "voice/video/VideoSendPipeline.h"

namespace {

// --- how long a QTRY_* is allowed to keep trying -----------------------------
//
// Every QTRY_*_WITH_TIMEOUT in this file is a LIVENESS wait: "the pipeline
// eventually emits a frame", "the drop counter eventually reaches ten". Not
// one of them defends a latency property — nothing here claims the encoder is
// fast, only that it works — so the deadline's only job is to stop a
// genuinely broken run from hanging forever.
//
// They were 2000 ms and 5000 ms. On a box that is also running a build, or
// another agent's test sweep, real H.264 encodes and Qt event-loop turns
// stretch past that, and the test failed for reasons that had nothing to do
// with the code. RUN_SERIAL does not help: ctest only serialises a test
// against OTHER CTEST JOBS, and the load that breaks these deadlines comes
// from outside the ctest process entirely.
//
// A healthy run touches neither number — it satisfies the predicate in
// milliseconds and QTRY returns immediately. All a larger deadline changes is
// how long a broken run takes to report itself, and a broken pipeline emits
// nothing at all rather than emitting slowly.
constexpr int kLivenessMs = 20000;   // pure-logic paths (counters, signals)
constexpr int kEncodeMs   = 30000;   // paths that encode or decode real frames

PeerCaps capsWith(bool videoRtp, const QStringList& codecs) {
    PeerCaps c;
    c.videoRtp = videoRtp;
    c.videoCodecs = codecs;
    return c;
}

// ---- S-18 codec-selection helpers ----------------------------------

videocodec::Viewer viewer(bool videoRtp, const QStringList& codecs,
                          bool capsKnown = true) {
    return {capsWith(videoRtp, codecs), capsKnown};
}
// A peer that can decode both — the ordinary current desktop build.
videocodec::Viewer hevcViewer() {
    return viewer(true, {QStringLiteral("h264"), QStringLiteral("h265")});
}
// rc.19 and earlier, or any Linux build: H.264 only.
videocodec::Viewer h264Viewer() {
    return viewer(true, {QStringLiteral("h264")});
}
// Advertises the protocol with no decoder at all — the JPEG fan-out's
// audience, which is a separate encode and must not get a vote.
videocodec::Viewer jpegViewer() { return viewer(true, {}); }
// Caps have not arrived yet.
videocodec::Viewer unknownViewer() {
    return viewer(true, {QStringLiteral("h264"), QStringLiteral("h265")}, false);
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

// HEVC access unit whose NAL is an IRAP picture (IDR_W_RADL, type 19).
// The receive pipeline scans H.265 units for types 16..23 in bits 1..6
// of the TWO-byte NAL header, so this is what lets an H.265 stream out
// of its keyframe wait — and therefore what gets it as far as trying to
// build a decoder.
QByteArray hevcIrap(int payloadBytes = 32) {
    QByteArray au;
    au.append(char(0)).append(char(0)).append(char(0)).append(char(1));
    au.append(char(19 << 1));   // nal_unit_type 19, layer id 0
    au.append(char(1));         // temporal_id_plus1
    au.append(QByteArray(payloadBytes, char(0x42)));
    return au;
}

// ---- Decoder factory stubs (HEVC decode fallback) ------------------

// A backend whose probe said yes and whose init says no — the Windows
// HEVC MFT from the field report, reproduced on any machine.
class RefusingDecoder : public VideoDecoder {
public:
    bool init(VideoCodecKind) override { return false; }
    Result decode(const QByteArray&, QVideoFrame&) override {
        return Result::Error;
    }
    void reset() override {}
};

// A backend that works, so "the fallback codec actually decodes" can be
// asserted without dragging a real encoder into the test.
class WorkingStubDecoder : public VideoDecoder {
public:
    bool init(VideoCodecKind) override { return true; }
    Result decode(const QByteArray&, QVideoFrame& out) override {
        QVideoFrameFormat fmt(QSize(64, 48),
                              QVideoFrameFormat::Format_ARGB8888);
        out = QVideoFrame(fmt);
        return out.isValid() ? Result::Ok : Result::Error;
    }
    void reset() override {}
};

// ---- Rate-controller test helpers (S-17) ---------------------------

const QString kPeerA = QStringLiteral("@a:x");
const QString kPeerB = QStringLiteral("@b:x");

// One graded window from a peer running a current build.
VideoDeliveryReport lossReport(quint64 expected, double lossPct,
                               double goodputKbps = 4000.0) {
    VideoDeliveryReport r;
    r.hasLoss = true;
    r.expected = expected;
    r.lost = quint64(std::llround(double(expected) * lossPct / 100.0));
    r.goodputKbps = goodputKbps;
    return r;
}

// What a peer on a build that predates the packet counters produces:
// bytes only, no opinion on loss.
VideoDeliveryReport legacyReport(double goodputKbps = 3000.0) {
    VideoDeliveryReport r;
    r.hasLoss = false;
    r.goodputKbps = goodputKbps;
    return r;
}

void run(VideoRateController& rc, int ticks, const VideoDeliveryReport& r,
         const QString& peer = kPeerA) {
    for (int i = 0; i < ticks; ++i) {
        rc.reportDelivery(peer, r);
        rc.tick();
    }
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

    void initTestCase() {
        // videohealth persists its hint through a QSettings built from
        // the profile-aware application name. Claim a profile of our
        // own BEFORE anything touches it, so this test cannot reach
        // into a real BSFChat install and switch H.265 off for the
        // user — which, given the hint survives restarts, would be a
        // very quiet way to break their video.
        bsfchat::setActiveProfile(QStringLiteral("test-videopipeline"));
    }

    void cleanup() {
        // Whatever a case installed, the next one starts clean.
        VideoDecoder::setFactoryForTest({});
        videohealth::resetForTest();
    }

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

    // The reliable control channel may only be opened toward a peer
    // that says it knows the label. An older build dispatches any
    // unknown channel into its AUDIO binding (proved in
    // test_media_loopback), so opening one unasked fixes video by
    // damaging audio.
    // ---- S-18: which codec a stream is encoded in -------------------
    //
    // The whole truth table, because a mesh encodes ONCE per stream and
    // fans the same bytes to everyone: getting this wrong does not
    // degrade one viewer, it blacks out one viewer while everyone else
    // sees a perfect picture, which is the hardest kind of bug to be
    // told about.

    void hevcNeedsThePreference_theEncoder_andEveryViewer() {
        using namespace videocodec;
        const QList<Viewer> allHevc{hevcViewer(), hevcViewer()};

        // The happy path, under both spellings of "yes".
        QCOMPARE(select(Preference::Auto, true, allHevc), VideoCodecKind::H265);
        QCOMPARE(select(Preference::PreferHevc, true, allHevc),
                 VideoCodecKind::H265);

        // Each single "no" is sufficient on its own.
        QCOMPARE(select(Preference::H264Only, true, allHevc),
                 VideoCodecKind::H264);
        QCOMPARE(select(Preference::Auto, /*localEncoder=*/false, allHevc),
                 VideoCodecKind::H264);
        QCOMPARE(select(Preference::Auto, true, {hevcViewer(), h264Viewer()}),
                 VideoCodecKind::H264);
        // ...including "prefer", which is a preference and not an order:
        // there is no second encode to give the H.264 viewer.
        QCOMPARE(select(Preference::PreferHevc, true,
                        {hevcViewer(), h264Viewer()}),
                 VideoCodecKind::H264);
    }

    void unknownCapsVoteNo() {
        using namespace videocodec;
        // A peer mid-handshake may turn out to be an rc.19 build.
        // Starting on H.265 and rebuilding the encoder a second later
        // is worse than starting correctly.
        QCOMPARE(select(Preference::Auto, true, {unknownViewer()}),
                 VideoCodecKind::H264);
        QCOMPARE(select(Preference::Auto, true,
                        {hevcViewer(), unknownViewer()}),
                 VideoCodecKind::H264);
    }

    void jpegOnlyPeersDoNotGetAVote() {
        using namespace videocodec;
        // One Android in the room would otherwise pin every desktop
        // viewer to H.264 for nothing: that peer is served by the JPEG
        // fan-out, which is a separate encode entirely.
        QCOMPARE(select(Preference::Auto, true, {hevcViewer(), jpegViewer()}),
                 VideoCodecKind::H265);
        // Same for a peer that does not speak RTP video at all.
        QCOMPARE(select(Preference::Auto, true,
                        {hevcViewer(), viewer(false, {})}),
                 VideoCodecKind::H265);
    }

    void withNoViewersTheConditionIsVacuouslyTrue() {
        using namespace videocodec;
        // "Every viewer supports it" over an empty set. Costs nothing:
        // the send path only encodes once a video-capable peer exists,
        // and the first joiner that cannot decode flips it back.
        QCOMPARE(select(Preference::Auto, true, {}), VideoCodecKind::H265);
        QCOMPARE(select(Preference::Auto, false, {}), VideoCodecKind::H264);
    }

    // The transitions are the point — the setting is read once, the
    // room changes constantly.
    void joinAndLeaveFlipTheCodecBothWays() {
        using namespace videocodec;
        QList<Viewer> room{hevcViewer()};
        QCOMPARE(select(Preference::Auto, true, room), VideoCodecKind::H265);

        // An old build joins: everyone drops to H.264 on the next tick.
        room.append(h264Viewer());
        QCOMPARE(select(Preference::Auto, true, room), VideoCodecKind::H264);

        // A third, capable peer joins — still H.264, one veto is enough.
        room.append(hevcViewer());
        QCOMPARE(select(Preference::Auto, true, room), VideoCodecKind::H264);

        // The old build leaves: back up to H.265.
        room.removeAt(1);
        QCOMPARE(select(Preference::Auto, true, room), VideoCodecKind::H265);

        // Everyone leaves.
        room.clear();
        QCOMPARE(select(Preference::Auto, true, room), VideoCodecKind::H265);
    }

    // The HEVC decode fallback, at the selector.
    //
    // The field failure: a Windows client's Media Foundation probe
    // found an HEVC MFT, so its caps claimed "h265", so BOTH Mac
    // senders flipped their screen shares to H.265 by payload type.
    // Its real decoder init then failed and it painted two black tiles
    // for the rest of the call, because caps were a join-time fact and
    // nothing could contradict them afterwards.
    //
    // The safety net makes that viewer RETRACT h265 mid-call. From the
    // selector's side that is just a viewer whose caps changed, and the
    // only thing that must be true is that the retraction is as binding
    // as the original claim.
    void aViewerThatRetractsHevcPutsTheMeshBackOnH264() {
        using namespace videocodec;
        // Two Macs and the Windows client, all claiming H.265.
        QList<Viewer> room{hevcViewer(), hevcViewer(), hevcViewer()};
        QCOMPARE(select(Preference::Auto, true, room), VideoCodecKind::H265);

        // Its decoder refused; it re-announced caps without h265.
        room[2] = h264Viewer();
        QCOMPARE(select(Preference::Auto, true, room), VideoCodecKind::H264);
        // "Prefer" is still a preference, not an override — there is no
        // second encode to give the viewer that cannot decode.
        QCOMPARE(select(Preference::PreferHevc, true, room),
                 VideoCodecKind::H264);

        // And it stays H.264 while that viewer is in the room, however
        // many capable peers join afterwards.
        room.append(hevcViewer());
        QCOMPARE(select(Preference::Auto, true, room), VideoCodecKind::H264);

        // Only its LEAVING may lift the veto.
        room.removeAt(2);
        QCOMPARE(select(Preference::Auto, true, room), VideoCodecKind::H265);
    }

    // The retraction travels as a caps JSON round trip (announceLocalCaps
    // → the peer's `t == "caps"` branch → PeerCaps::fromJson), so the
    // dropped codec has to survive that, not just the in-memory struct.
    void theRetractionSurvivesTheCapsJsonRoundTrip() {
        using namespace videocodec;
        PeerCaps before = capsWith(true, {QStringLiteral("h264"),
                                          QStringLiteral("h265")});
        QVERIFY(PeerCaps::fromJson(before.toJson()).decodes(videoCodecIdH265()));
        QCOMPARE(select(Preference::Auto, true,
                        {Viewer{PeerCaps::fromJson(before.toJson()), true}}),
                 VideoCodecKind::H265);

        // What localCapsJson() now produces on a machine whose H.265
        // decoder has been proven broken: h264 alone.
        PeerCaps after = capsWith(true, {QStringLiteral("h264")});
        const PeerCaps wire = PeerCaps::fromJson(after.toJson());
        QVERIFY(!wire.decodes(videoCodecIdH265()));
        QVERIFY(wire.decodes(videoCodecIdH264()));
        // Still a full RTP video peer — the fallback is H.264, not JPEG.
        QVERIFY(peerCanReceiveRtpVideo(wire, true, videoCodecIdH264()));
        QCOMPARE(select(Preference::Auto, true, {Viewer{wire, true}}),
                 VideoCodecKind::H264);
    }

    void capsArrivingIsATransitionToo() {
        using namespace videocodec;
        QList<Viewer> room{unknownViewer()};
        QCOMPARE(select(Preference::Auto, true, room), VideoCodecKind::H264);
        // Same peer, caps now known and capable.
        room[0] = hevcViewer();
        QCOMPARE(select(Preference::Auto, true, room), VideoCodecKind::H265);
    }

    void thePreferenceStringRoundTripsAndNormalises() {
        using namespace videocodec;
        for (const char* v : {"auto", "preferHevc", "h264Only"}) {
            const QString str = QString::fromLatin1(v);
            QCOMPARE(preferenceToString(preferenceFromString(str)), str);
        }
        // Anything unrecognised — an empty settings key, a value from a
        // future build, a typo — must read as the safe default rather
        // than as "never use H.265" or as undefined behaviour.
        QCOMPARE(preferenceFromString(QString()), Preference::Auto);
        QCOMPARE(preferenceFromString(QStringLiteral("hevc-please")),
                 Preference::Auto);
        // Case tolerance, same as the codec-id matching above.
        QCOMPARE(preferenceFromString(QStringLiteral("H264ONLY")),
                 Preference::H264Only);
    }

    void controlChannelIsOpenedOnlyWhenThePeerAdvertisesIt() {
        PeerCaps updated = capsWith(true, {QStringLiteral("h264")});
        updated.controlDc = true;
        QVERIFY(peerUsesControlChannel(updated, true));
        // Caps not exchanged yet — control stays on the audio channel.
        QVERIFY(!peerUsesControlChannel(updated, false));

        // A build that speaks RTP video but predates the channel.
        PeerCaps olderRtpPeer = capsWith(true, {QStringLiteral("h264")});
        QVERIFY(!olderRtpPeer.controlDc);
        QVERIFY(!peerUsesControlChannel(olderRtpPeer, true));

        // ...and it must still be a full video peer: the fallback is
        // the old behaviour, not a downgrade to JPEG.
        QVERIFY(peerCanReceiveRtpVideo(olderRtpPeer, true, videoCodecIdH264()));
    }

    // The flag has to survive the caps round-trip or the gate above is
    // decided on a field nobody ever sets.
    void controlChannelCapSurvivesJsonRoundTrip() {
        PeerCaps out = capsWith(true, {QStringLiteral("h264")});
        out.controlDc = true;
        const PeerCaps back = PeerCaps::fromJson(out.toJson());
        QVERIFY(back.controlDc);
        QVERIFY(back.videoRtp);

        // Absent field (an older peer's caps) reads as false, never as
        // "probably supported".
        nlohmann::json legacy;
        legacy["video_rtp"] = 1;
        legacy["video_codecs"] = nlohmann::json::array({"h264"});
        QVERIFY(!PeerCaps::fromJson(legacy).controlDc);
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
        QTRY_VERIFY_WITH_TIMEOUT(pipe.keyframeRequests() >= 3, kLivenessMs);
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
        // 60s, not 5s: long enough that a loaded box stretching the submit
        // loop below cannot let the throttle expire and fire a second
        // request. The property is that no request fires INSIDE the
        // interval, so a wider interval tests it more strictly.
        pipe.setKeyframeRequestIntervalMs(60000);
        // Below the decode backlog cap, so every drop here is the
        // keyframe gate rather than an overflow flush.
        for (int i = 0; i < 10; ++i) {
            pipe.submitAccessUnit(annexB(1));
            QTest::qWait(2);
        }
        QTRY_COMPARE_WITH_TIMEOUT(pipe.droppedAus(), quint64(10), kLivenessMs);
        QCOMPARE(pipe.keyframeRequests(), quint64(1));
    }

    // ---- HEVC decode fallback: the receive side --------------------

    // The receiver half of the field failure. A decoder that refuses to
    // come up must (1) say so exactly once on the wire, because its one
    // subscriber turns that into a caps correction broadcast to every
    // peer, and (2) stop rebuilding itself per access unit — the log
    // that reported this bug had `no decoder available` at 30 lines a
    // second per stream, which is how it stayed unread for a day.
    void aRefusedDecoderAnnouncesItselfExactlyOnce() {
        std::atomic<int> creates{0};
        VideoDecoder::setFactoryForTest([&creates](VideoCodecKind, bool) {
            creates.fetch_add(1);
            return std::unique_ptr<VideoDecoder>(new RefusingDecoder);
        });

        VideoReceivePipeline pipe(kPeerA, VideoStreamId::Screen,
                                  VideoCodecKind::H265);
        // 60s, not 5s: long enough that a loaded box stretching the submit
        // loop below cannot let the throttle expire and fire a second
        // request. The property is that no request fires INSIDE the
        // interval, so a wider interval tests it more strictly.
        pipe.setKeyframeRequestIntervalMs(60000);
        pipe.setDecoderRetryIntervalMs(5000);   // no retry within this test
        QSignalSpy spy(&pipe, &VideoReceivePipeline::decoderUnavailable);

        // A second of IRAP pictures: every one of them clears the
        // keyframe gate and reaches the decoder.
        for (int i = 0; i < 20; ++i) {
            pipe.submitAccessUnit(hevcIrap());
            QTest::qWait(2);
        }
        QTRY_COMPARE_WITH_TIMEOUT(pipe.decoderFailures(), quint64(1), kLivenessMs);
        QCOMPARE(creates.load(), 1);        // not one per access unit
        QCOMPARE(spy.count(), 1);           // and ONE announcement
        QCOMPARE(pipe.decodedFrames(), quint64(0));
        // Everything received was dropped rather than queued behind a
        // decoder that is never going to exist.
        QCOMPARE(pipe.droppedAus(), quint64(20));

        // The signal has to name the codec, or the subscriber cannot
        // tell "retract h265" from "retract the only codec we have".
        QCOMPARE(spy.at(0).at(0).toString(), kPeerA);
        QCOMPARE(spy.at(0).at(1).toInt(), int(VideoStreamId::Screen));
        QCOMPARE(spy.at(0).at(2).toInt(), int(VideoCodecKind::H265));
    }

    // The retry exists (a decoder can refuse because the GPU is
    // momentarily busy) but it is throttled, and a retry that fails
    // again must NOT produce a second announcement: the caps are
    // already corrected and re-broadcasting them is pure noise.
    void theDecoderRetryIsThrottledAndNeverReAnnounces() {
        std::atomic<int> creates{0};
        VideoDecoder::setFactoryForTest([&creates](VideoCodecKind, bool) {
            creates.fetch_add(1);
            return std::unique_ptr<VideoDecoder>(new RefusingDecoder);
        });

        VideoReceivePipeline pipe(kPeerA, VideoStreamId::Screen,
                                  VideoCodecKind::H265);
        // 60s, not 5s: long enough that a loaded box stretching the submit
        // loop below cannot let the throttle expire and fire a second
        // request. The property is that no request fires INSIDE the
        // interval, so a wider interval tests it more strictly.
        pipe.setKeyframeRequestIntervalMs(60000);
        pipe.setDecoderRetryIntervalMs(40);
        QSignalSpy spy(&pipe, &VideoReceivePipeline::decoderUnavailable);

        QElapsedTimer clock;
        clock.start();
        for (int i = 0; i < 40; ++i) {
            pipe.submitAccessUnit(hevcIrap());
            QTest::qWait(5);
        }
        QTRY_VERIFY_WITH_TIMEOUT(pipe.decoderFailures() >= 2, kLivenessMs);
        // The bound is relative to wall time, not to the access-unit
        // count: a 40 ms floor permits at most one retry per 40 ms of
        // elapsed time (+1 for the initial attempt, +1 for the edge),
        // however slowly a loaded CI runner paced the qWait()s. The
        // first cut of this test assumed the loop took ~200 ms and
        // failed on GitHub's macOS runner where it took over a second.
        const qint64 elapsedMs = clock.elapsed();
        const quint64 allowed = quint64(elapsedMs / 40) + 2;
        QVERIFY2(pipe.decoderFailures() <= allowed,
                 qPrintable(QStringLiteral("decoder rebuilt %1 times in %2 ms "
                                           "for 40 access units (allowed %3) "
                                           "— throttle is not holding")
                            .arg(pipe.decoderFailures()).arg(elapsedMs)
                            .arg(allowed)));
        QVERIFY(pipe.decoderFailures() < 40);
        QCOMPARE(quint64(creates.load()), pipe.decoderFailures());
        QCOMPARE(spy.count(), 1);
    }

    // The other half of self-healing: once the sender has re-selected
    // H.264, the access units arrive under a different payload type and
    // VoiceEngine::recvPipeline builds a pipeline keyed on the new
    // codec. That pipeline must be unaffected by the H.265 one's
    // failure — the latch is per pipeline, not per stream.
    void anH264StreamAfterAnH265FailureStillDecodes() {
        VideoDecoder::setFactoryForTest([](VideoCodecKind kind, bool)
                                        -> std::unique_ptr<VideoDecoder> {
            if (kind == VideoCodecKind::H265)
                return std::make_unique<RefusingDecoder>();
            return std::make_unique<WorkingStubDecoder>();
        });

        {
            VideoReceivePipeline hevc(kPeerA, VideoStreamId::Screen,
                                      VideoCodecKind::H265);
            hevc.setKeyframeRequestIntervalMs(60000);  // see above: never expire mid-test
            hevc.setDecoderRetryIntervalMs(5000);
            QSignalSpy spy(&hevc, &VideoReceivePipeline::decoderUnavailable);
            for (int i = 0; i < 5; ++i) {
                hevc.submitAccessUnit(hevcIrap());
                QTest::qWait(2);
            }
            QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, kLivenessMs);
            QCOMPARE(hevc.decodedFrames(), quint64(0));
        }

        // Same peer, same stream, new codec — exactly what
        // recvPipeline() constructs when the payload type flips to 96.
        VideoReceivePipeline h264(kPeerA, VideoStreamId::Screen,
                                  VideoCodecKind::H264);
        QSignalSpy frames(&h264, &VideoReceivePipeline::frameDecoded);
        QSignalSpy unavailable(&h264,
                               &VideoReceivePipeline::decoderUnavailable);
        for (int i = 0; i < 4; ++i) {
            // IDR first: a stream never starts on a delta.
            h264.submitAccessUnit(annexB(i == 0 ? 5 : 1));
            QTest::qWait(2);
        }
        QTRY_VERIFY_WITH_TIMEOUT(h264.decodedFrames() >= 4, kLivenessMs);
        QVERIFY(frames.count() >= 4);
        QCOMPARE(unavailable.count(), 0);
        QCOMPARE(h264.decoderFailures(), quint64(0));
    }

    // The process-wide latch is what stops the caps from re-advertising
    // h265 on the NEXT call in this process — the bug was not that one
    // stream went black, it was that every later stream did too.
    void theH265LatchIsProcessWideAndFiresItsEdgeOnce() {
        QVERIFY(!videohealth::h265DecodeBroken());
        // Only the caller that flips it gets the edge. That is the rate
        // limit on the caps broadcast: two streams × N peers all reach
        // this, one message goes out.
        QVERIFY(videohealth::markH265DecodeBroken());
        QVERIFY(!videohealth::markH265DecodeBroken());
        QVERIFY(!videohealth::markH265DecodeBroken());
        QVERIFY(videohealth::h265DecodeBroken());

        videohealth::resetForTest();
        QVERIFY(!videohealth::h265DecodeBroken());
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

    // ---- S-17: loss counting in the sequence tracker ---------------
    //
    // `expected` is derived as received + confirmed-lost rather than
    // from a sequence-number span, so wraparound needs no case of its
    // own — and a reordered packet must not show up as loss, because
    // the sender would then cut the bitrate for a path that dropped
    // nothing at all.

    void lossCounterIgnoresReorderingAndWraparound() {
        RtpSeqTracker clean;
        for (int i = 0; i < 12; ++i)
            clean.observe(uint16_t(65530 + i), i);   // wraps through 0
        QCOMPARE(clean.received(), quint64(12));
        QCOMPARE(clean.lost(), quint64(0));
        QCOMPARE(clean.expected(), quint64(12));

        RtpSeqTracker reordered;
        reordered.observe(1, 0);
        reordered.observe(2, 1);
        reordered.observe(4, 2);      // gap opens
        reordered.observe(3, 3);      // …and fills itself
        reordered.observe(5, 4);
        reordered.poll(5);
        QCOMPARE(reordered.lost(), quint64(0));
        QCOMPARE(reordered.expected(), quint64(5));
    }

    void lossCounterCountsRealHoles() {
        RtpSeqTracker t;
        t.observe(1, 0);
        t.observe(2, 1);
        t.observe(4, 2);              // 3 is really gone
        t.observe(5, 3);
        t.observe(6, 4);              // confirmed
        QCOMPARE(t.lost(), quint64(1));
        QCOMPARE(t.received(), quint64(5));
        QCOMPARE(t.expected(), quint64(6));

        // A gap too wide to be reordering is counted in full.
        RtpSeqTracker big;
        big.observe(100, 0);
        big.observe(141, 1);
        QCOMPARE(big.lost(), quint64(40));
        QCOMPARE(big.expected(), quint64(42));

        // Wraparound around a genuine hole: 65534, [65535 lost], 0, 1…
        RtpSeqTracker wrap;
        wrap.observe(65534, 0);
        wrap.observe(0, 1);
        wrap.observe(1, 2);
        wrap.observe(2, 3);
        QCOMPARE(wrap.lost(), quint64(1));
    }

    // ---- S-17: receiver-report estimator ---------------------------

    // The field sequence, exactly: the peer's first rr is written
    // before its first packet lands, so a naive difference reads
    // "everything was lost". It cut 2338 → 985 kbps in 1.5 s before a
    // single packet had been graded.
    void theFirstReportsAfterASeedAreNeverLoss() {
        ReceiverReportEstimator e;
        const auto seed = e.update(0, 0, 0, true, 0);
        QVERIFY2(!seed.governs(), "the first report is a baseline, not a verdict");
        const auto warm = e.update(0, 0, 0, true, 500);
        QVERIFY2(!warm.governs(), "the first difference must be discarded");

        const auto first = e.update(50000, 40, 0, true, 1000);
        QVERIFY(first.governs());
        QCOMPARE(first.lossPct(), 0.0);
        QVERIFY(first.goodputKbps > 0.0);
    }

    void aCounterResetReSeedsInsteadOfPanicking() {
        ReceiverReportEstimator e;
        e.update(100000, 800, 0, true, 0);
        e.update(150000, 1200, 0, true, 500);
        const auto graded = e.update(200000, 1600, 0, true, 1000);
        QVERIFY(graded.governs());

        // Peer rebuilt its receive pipeline: counters go backwards.
        QVERIFY(!e.update(0, 0, 0, true, 1500).governs());
        QVERIFY(!e.update(20000, 160, 0, true, 2000).governs());
        QVERIFY(e.update(40000, 320, 0, true, 2500).governs());
    }

    void realLossSurvivesTheEstimator() {
        ReceiverReportEstimator e;
        e.update(0, 0, 0, true, 0);
        e.update(50000, 400, 0, true, 500);
        const auto r = e.update(85000, 800, 80, true, 1000);
        QVERIFY(r.governs());
        QVERIFY2(qAbs(r.lossPct() - 20.0) < 0.001,
                 qPrintable(QString::number(r.lossPct())));
    }

    void aPeerWithoutTheNewFieldsNeverGoverns() {
        ReceiverReportEstimator e;
        e.update(0, 0, 0, false, 0);
        e.update(50000, 0, 0, false, 500);
        const auto r = e.update(100000, 0, 0, false, 1000);
        QVERIFY2(!r.governs(), "an old peer's report is not a 100 % loss report");
        QVERIFY2(r.goodputKbps > 0.0, "…but its goodput is still usable");
    }

    // ---- S-17: the policy table ------------------------------------

    void policyFloorsMatchWhatIsActuallyWatchable() {
        using namespace videorate;
        QVERIFY2(minKbpsFor(Content::Screen, 1920, 30) >= 4000,
                 "1080p30 gameplay below ~4 Mbps is not a usable picture");
        QVERIFY2(minKbpsFor(Content::Screen, 1280, 30) >= 1200,
                 "720p30 screen content needs at least ~1.2 Mbps");
        QVERIFY2(minKbpsFor(Content::Camera, 1280, 30) >= 1200, "720p camera");
        QVERIFY2(minKbpsFor(Content::Camera, 1920, 30) >= 2500, "1080p camera");
        // Comfort sits clearly above floor — that gap IS the anti-flap.
        QVERIFY(comfortKbpsFor(Content::Screen, 1280, 30)
                > minKbpsFor(Content::Screen, 1280, 30) * 5 / 4);
    }

    void hevcFloorsAreLowerThanH264AtEverySizeAndRung() {
        using namespace videorate;
        // The floor is what the ladder uses to decide "this bitrate
        // cannot carry this size, step down". Leaving H.264's floors in
        // place under HEVC would drop resolution while the picture was
        // still sharp — spending the codec win on a smaller image
        // instead of a better one.
        for (Content c : {Content::Screen, Content::Camera}) {
            for (int edge : {1920, 1280, 960, 640}) {
                const int h264 = minKbpsFor(c, edge, 30, VideoCodecKind::H264);
                const int h265 = minKbpsFor(c, edge, 30, VideoCodecKind::H265);
                QVERIFY2(h265 < h264, "HEVC must ask for fewer bits");
                // ~0.6x, allowing for the integer truncation.
                QVERIFY(h265 >= h264 * 55 / 100);
                QVERIFY(h265 <= h264 * 65 / 100);
            }
        }
        // The anti-flap gap survives the scaling: comfort still sits
        // clearly above floor for HEVC, or the ladder would oscillate.
        QVERIFY(comfortKbpsFor(Content::Screen, 1280, 30, VideoCodecKind::H265)
                > minKbpsFor(Content::Screen, 1280, 30, VideoCodecKind::H265)
                      * 5 / 4);
        // And an HEVC floor is never so low it undercuts the bottom
        // rung's usefulness: 1080p30 HEVC screen still wants > 2 Mbps.
        QVERIFY(minKbpsFor(Content::Screen, 1920, 30, VideoCodecKind::H265)
                > 2000);
    }

    void theDefaultCodecArgumentIsStillH264() {
        using namespace videorate;
        // Every existing caller passes no codec. If that default ever
        // moved, every H.264 stream would silently be judged by HEVC's
        // floors and would stop laddering down when it should.
        QCOMPARE(minKbpsFor(Content::Screen, 1920, 30),
                 minKbpsFor(Content::Screen, 1920, 30, VideoCodecKind::H264));
        QCOMPARE(rungMinKbps(Content::Camera, 1280, 30, 3),
                 rungMinKbps(Content::Camera, 1280, 30, 3,
                             VideoCodecKind::H264));
    }

    void theLadderGivesUpTheRightThingFirst() {
        using namespace videorate;
        // Screen: fps first, long edge defended.
        QCOMPARE(rungAt(Content::Screen, 1).resScale, 1.0);
        QVERIFY(rungAt(Content::Screen, 1).fpsScale < 1.0);
        // Camera: resolution first, frame rate defended.
        QVERIFY(rungAt(Content::Camera, 1).resScale < 1.0);
        QCOMPARE(rungAt(Content::Camera, 1).fpsScale, 1.0);
        // Both ladders end somewhere still watchable, not at zero.
        QVERIFY(edgeForRung(Content::Screen, 1920, kLadderRungs - 1) >= 400);
        QVERIFY(fpsForRung(Content::Screen, 30, kLadderRungs - 1) >= 15);
    }

    // ---- S-17: the control law -------------------------------------

    // THE case. A clean LAN whose byte counts are bursty (goodput
    // swings wildly, report windows hold anything from 120 to 900
    // packets) but whose packet loss is zero. The old byte-ratio law
    // read 0.77-0.96 here and walked to the floor; this must reach the
    // configured maximum and stay there.
    void aCleanLanClimbsToTheConfiguredMaximumAndStays() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 20000, 30, 1920);
        rc.setActive(true);
        const int start = rc.targetKbps();
        QVERIFY2(start >= 4000, qPrintable(QStringLiteral(
            "a 1080p30 share must not open at %1 kbps").arg(start)));

        // 30 ticks = 15 s of wall clock at the 500 ms evaluation rate.
        static const quint64 kBursty[] = {900, 120, 640, 210, 880, 150};
        for (int i = 0; i < 30; ++i) {
            rc.reportDelivery(kPeerA,
                lossReport(kBursty[i % 6], 0.0,
                           /*goodput swings by 8x*/ 900.0 * double(1 + i % 8)));
            rc.tick();
        }
        QCOMPARE(rc.targetKbps(), 20000);
        QCOMPARE(rc.longEdge(), 1920);
        QCOMPARE(rc.fps(), 30);

        // …and STAYS. The old law's 0.97 threshold made every window a
        // back-off, so "stays" is half the bug.
        for (int i = 0; i < 40; ++i) {
            rc.reportDelivery(kPeerA, lossReport(kBursty[i % 6], 0.0));
            rc.tick();
        }
        QCOMPARE(rc.targetKbps(), 20000);
        QCOMPARE(rc.longEdge(), 1920);
    }

    // Steady moderate loss must find a resting place. A law with no
    // hold band multiplies itself to the floor no matter how gentle
    // each step is.
    void fivePercentLossSettlesInsteadOfCollapsing() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 20000, 30, 1920);
        rc.setActive(true);
        const int start = rc.targetKbps();
        run(rc, 60, lossReport(600, 5.0));
        QVERIFY2(rc.targetKbps() >= start * 9 / 10,
                 qPrintable(QStringLiteral("5 %% loss walked %1 → %2 kbps")
                                .arg(start).arg(rc.targetKbps())));
        QCOMPARE(rc.longEdge(), 1920);
        QVERIFY2(rc.targetKbps() >= 4000, "still a watchable 1080p rate");
    }

    void twentyPercentLossCutsThenRecoversWhenItClears() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 20000, 30, 1920);
        rc.setActive(true);
        const int start = rc.targetKbps();

        run(rc, 8, lossReport(600, 20.0));
        const int trough = rc.targetKbps();
        QVERIFY2(trough < start / 2, qPrintable(QStringLiteral(
            "20 %% loss must actually back off: %1 → %2").arg(start).arg(trough)));
        QVERIFY2(rc.longEdge() < 1920 || rc.fps() < 30,
                 "and must trade quality, not just bits");

        run(rc, 90, lossReport(600, 0.0));
        QCOMPARE(rc.targetKbps(), 20000);
        QCOMPARE(rc.longEdge(), 1920);
        QCOMPARE(rc.fps(), 30);
    }

    // An old peer sends rr without the packet fields. Reading that as
    // total loss collapses the share; reading it as zero loss licenses
    // a climb on no evidence. It must simply hold.
    void reportsWithoutTheNewFieldsHold() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 20000, 30, 1920);
        rc.setActive(true);
        run(rc, 6, lossReport(600, 0.0));
        const int held = rc.targetKbps();
        QVERIFY(held > 0);

        run(rc, 30, legacyReport());
        QCOMPARE(rc.targetKbps(), held);
        QCOMPARE(rc.longEdge(), 1920);
    }

    // Mesh: the worst GOVERNING peer sets the rate, but a peer with no
    // opinion is not the worst peer — it is not a peer at all for this
    // purpose.
    void theWorstGoverningPeerSetsTheRate() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 20000, 30, 1920);
        rc.setActive(true);
        run(rc, 6, lossReport(600, 0.0));
        const int before = rc.targetKbps();
        rc.reportDelivery(kPeerA, lossReport(600, 0.0));
        rc.reportDelivery(kPeerB, lossReport(600, 25.0));
        rc.tick();
        QVERIFY2(rc.targetKbps() < before, "the worst receiver governs");
    }

    void aSilentOldPeerDoesNotDragTheRateDown() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 20000, 30, 1920);
        rc.setActive(true);
        for (int i = 0; i < 30; ++i) {
            rc.reportDelivery(kPeerA, lossReport(600, 0.0));   // current build
            rc.reportDelivery(kPeerB, legacyReport());         // old build
            rc.tick();
        }
        QCOMPARE(rc.targetKbps(), 20000);
    }

    void keyframeStormCountsAsCongestion() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 20000, 30, 1920);
        rc.setActive(true);
        run(rc, 4, lossReport(600, 0.0));
        const int before = rc.targetKbps();
        rc.reportDelivery(kPeerA, lossReport(600, 0.0));
        for (int i = 0; i < 3; ++i) rc.reportKeyframeRequest();
        rc.tick();
        QVERIFY2(rc.targetKbps() < before,
                 "a keyframe storm is a congestion signal even at zero loss");
    }

    // ---- S-17: ladder hysteresis -----------------------------------

    void theLadderDwellsAfterADownshiftAndNeedsSustainedComfortToRise() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 20000, 30, 1920);
        rc.setActive(true);
        // Two hard cuts take 1080p30 below its floor → fps gives way.
        run(rc, 2, lossReport(600, 25.0));
        QVERIFY2(rc.fps() < 30, "screen content spends fps first");
        QCOMPARE(rc.longEdge(), 1920);
        const int downFps = rc.fps();

        // Minimum dwell: the path is instantly clean again and the
        // bitrate climbs straight back, but the ladder must not.
        for (int i = 0; i < videorate::Thresholds::kDwellTicks; ++i) {
            rc.reportDelivery(kPeerA, lossReport(600, 0.0));
            rc.tick();
            QCOMPARE(rc.fps(), downFps);
        }
        run(rc, 40, lossReport(600, 0.0));
        QCOMPARE(rc.fps(), 30);
        QCOMPARE(rc.longEdge(), 1920);
    }

    void theLadderDoesNotFlapOnAnIntermittentPath() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 20000, 30, 1920);
        rc.setActive(true);

        int changes = 0;
        int lastEdge = rc.longEdge(), lastFps = rc.fps();
        // 200 ticks = 100 s: mostly clean with a loss spike every 5 s.
        for (int i = 0; i < 200; ++i) {
            rc.reportDelivery(kPeerA,
                lossReport(600, (i % 10 == 9) ? 20.0 : 0.0));
            rc.tick();
            if (rc.longEdge() != lastEdge || rc.fps() != lastFps) {
                ++changes;
                lastEdge = rc.longEdge();
                lastFps = rc.fps();
            }
        }
        QVERIFY2(changes <= 6, qPrintable(QStringLiteral(
            "ladder changed %1 times in 100 s — that is flapping")
                .arg(changes)));
        QCOMPARE(rc.longEdge(), 1920);
    }

    // ---- S-17: start-up and restart --------------------------------

    // A camera target of 1500 kbps used to open at 1500/2 = 750 kbps of
    // 1280x720 — below the rate at which 720p is worth encoding — so
    // the share was mush before a single report had arrived.
    void aFreshShareOpensAtAWatchableRate() {
        VideoRateController cam(VideoStreamId::Camera);
        cam.setEnvelope(150, 1500, 30, 1280);
        cam.setActive(true);
        QCOMPARE(cam.longEdge(), 1280);
        QVERIFY2(cam.targetKbps() >= 1200, qPrintable(QStringLiteral(
            "720p camera opened at %1 kbps").arg(cam.targetKbps())));

        VideoRateController screen(VideoStreamId::Screen);
        screen.setEnvelope(250, 46000, 30, 1920);
        screen.setActive(true);
        QVERIFY2(screen.targetKbps() >= 4000, qPrintable(QStringLiteral(
            "1080p30 screen opened at %1 kbps").arg(screen.targetKbps())));
    }

    void aRestartedShareDoesNotInheritTheCollapse() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 20000, 30, 1920);
        rc.setActive(true);
        const int start = rc.targetKbps();

        run(rc, 20, lossReport(600, 30.0));
        QVERIFY(rc.targetKbps() < start);
        QVERIFY(rc.longEdge() < 1920);

        rc.setActive(false);
        rc.setActive(true);
        QCOMPARE(rc.targetKbps(), start);
        QCOMPARE(rc.longEdge(), 1920);
        QCOMPARE(rc.fps(), 30);
    }

    // ---- S-17: blind mode ------------------------------------------

    // Without reports the controller has no evidence, so it must hold
    // rather than climb on faith — but the ceiling is for a path that
    // has NEVER answered, not for one that answered and went quiet.
    void blindModeHoldsBelowTheCeiling() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 40000, 30, 1920);
        rc.setActive(true);
        rc.setNowForTest(QDateTime::currentMSecsSinceEpoch());
        rc.setNowForTest(QDateTime::currentMSecsSinceEpoch() + 4000);
        rc.tick();
        rc.tick();
        QVERIFY2(rc.targetKbps() <= 8000,
                 qPrintable(QStringLiteral("blind at %1 kbps")
                                .arg(rc.targetKbps())));
    }

    void aProvenPathIsNotClampedWhenReportsGoQuiet() {
        VideoRateController rc(VideoStreamId::Screen);
        rc.setEnvelope(250, 40000, 30, 1920);
        const qint64 t0 = 1000000;
        rc.setNowForTest(t0);
        rc.setActive(true);
        // Prove the path, reaching well above the blind ceiling.
        for (int i = 0; i < 40; ++i) {
            rc.setNowForTest(t0 + i * 500);
            rc.reportDelivery(kPeerA, lossReport(600, 0.0));
            rc.tick();
        }
        const int proven = rc.targetKbps();
        QVERIFY2(proven > 8000, qPrintable(QStringLiteral(
            "expected to climb past the blind ceiling, got %1").arg(proven)));

        // Reports stop. Hold — do not descend past a ceiling that was
        // only ever meant for a path nobody has heard from.
        for (int i = 0; i < 20; ++i) {
            rc.setNowForTest(t0 + 20000 + i * 500);
            rc.tick();
        }
        QCOMPARE(rc.targetKbps(), proven);
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
        // Deliberately NOT QTRY_VERIFY. On a host with no usable H.264
        // encoder this case must SKIP, but QVERIFY returns out of the slot
        // the moment it fails, so the QSKIP below was unreachable and such a
        // host got a hard failure instead. Spin manually and let both
        // outcomes stay live.
        {
            QElapsedTimer waited; waited.start();
            while (spy.isEmpty() && waited.elapsed() < kEncodeMs)
                QTest::qWait(10);
        }
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
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 3, kEncodeMs);
        bool sawDelta = false;
        for (int i = 1; i < spy.count(); ++i)
            if (!spy.at(i).at(1).value<EncodedFrame>().keyframe) sawDelta = true;
        QVERIFY2(sawDelta, "every frame a keyframe means inter-coding is off");

        // S-11's mechanism: an explicit request produces an IDR, which
        // is what a share restart (and a late joiner) relies on.
        const int beforeForce = spy.count();
        pipe.forceKeyframe();
        pipe.submitFrame(makeTestFrame(320, 240, 7), 500000);
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() > beforeForce, kEncodeMs);
        QVERIFY2(spy.at(beforeForce).at(1).value<EncodedFrame>().keyframe,
                 "forceKeyframe() must produce an IDR on the next frame");

        // A source resize rebuilds the session, which must also open on
        // an IDR — receivers cannot carry references across it.
        const int beforeResize = spy.count();
        EncoderConfig bigger = cfg;
        bigger.width = bigger.height = 480;
        pipe.configure(bigger);
        pipe.submitFrame(makeTestFrame(480, 360, 8), 600000);
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() > beforeResize, kEncodeMs);
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
        // Deliberately NOT QTRY_VERIFY. On a host with no usable H.264
        // encoder this case must SKIP, but QVERIFY returns out of the slot
        // the moment it fails, so the QSKIP below was unreachable and such a
        // host got a hard failure instead. Spin manually and let both
        // outcomes stay live.
        {
            QElapsedTimer waited; waited.start();
            while (spy.isEmpty() && waited.elapsed() < kEncodeMs)
                QTest::qWait(10);
        }
        if (spy.isEmpty()) QSKIP("no usable H.264 encoder on this host");

        for (int round = 0; round < 20; ++round) {
            const int before = spy.count();
            // Back-to-back submits with no event-loop turn between
            // them: the worker is mid-drain for at least some of these.
            for (int i = 0; i < 4; ++i)
                pipe.submitFrame(makeTestFrame(160, 120, round * 4 + i),
                                 (round * 4 + i) * 33000);
            QTRY_VERIFY_WITH_TIMEOUT(spy.count() > before, kEncodeMs);
        }
    }
};

QTEST_MAIN(TestVideoPipeline)
#include "test_video_pipeline.moc"
