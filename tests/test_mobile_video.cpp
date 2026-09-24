// The parts of the iOS camera-video port that can be tested without
// hardware, a camera, or a phone.
//
// Deliberately narrow. Nothing here pretends to exercise VideoToolbox,
// an AVCaptureSession or a TCC prompt — a unit test cannot answer a
// permission dialog, and a test that "verified" a hardware encoder by
// calling it on the build machine would be verifying the Mac. What IS
// testable is everything the port added AROUND the hardware:
//
//   1. The orientation rule (VideoFrameOrientation.h) and its effect on
//      FrameConverter — pure arithmetic and a libyuv plane rotation.
//   2. The I420 -> NV12 packing the VideoToolbox encoder now feeds its
//      pixel buffers with (NV12Pack.h) — pure memory, stride-sensitive,
//      and exactly the kind of thing that is wrong by one row for a
//      month before anyone notices a green band.
//   3. The lazy camera-permission rule (CameraPermissionPolicy.h) —
//      pure, and the thing store review actually rejects builds over.
//   4. What this build advertises and negotiates for video. On an Apple
//      host this runs through the same BSFCHAT_HAVE_VIDEOTOOLBOX path an
//      iOS build takes, which is the closest a desktop test can get to
//      "the phone advertises h264".
//
// The device-only half is listed in docs/ios-voice.md.

#include <QtTest/QtTest>
#include <QImage>
#include <QPainter>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include "voice/CameraPermissionPolicy.h"
#include "voice/PeerCaps.h"
#include "voice/video/FrameConverter.h"
#include "voice/video/NV12Pack.h"
#include "voice/video/VideoCodec.h"
#include "voice/video/VideoDecoder.h"
#include "voice/video/VideoEncoder.h"
#include "voice/video/VideoFrameOrientation.h"

#include <vector>

// A missing platform branch in this target must break the BUILD, not one
// platform's test run.
//
// This target compiles VideoCodecFactory.cpp and then asserts what it
// advertises, so it needs the same backend the app gets. It shipped
// without the WIN32 arm: openh264 is excluded on Windows and APPLE is
// false there, so on that one platform it compiled with no backend at
// all, h264EncodeProfiles() returned an empty list, and two cases failed
// — in CI, on a platform nobody can reproduce on a Mac, reported by name
// only. The diagnosis cost a round trip.
//
// A compile-time check costs nothing, fires on the machine that made the
// mistake, and says which file to edit. Note this is the OPPOSITE of the
// codec-less shape the advertising cases deliberately construct below:
// that one is a PeerCaps object built by hand, which is data, not a
// build configuration.
#if !defined(BSFCHAT_HAVE_VIDEOTOOLBOX) \
    && !defined(BSFCHAT_HAVE_MEDIAFOUNDATION) \
    && !defined(BSFCHAT_HAVE_OPENH264)
#error "test_mobile_video has no video backend on this platform. Its target \
in tests/CMakeLists.txt is missing this platform's arm of the backend \
if/elseif chain — mirror the one in the top-level CMakeLists.txt."
#endif

namespace {

// A frame with an unmistakable corner: everything dark except a white
// block in the TOP-LEFT. Rotation is the one property a symmetric test
// pattern cannot detect, so the pattern has to have a corner.
QVideoFrame makeCorneredFrame(int w, int h,
                              QtVideo::Rotation rotation = QtVideo::Rotation::None,
                              bool mirrored = false) {
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(QColor(0, 0, 0));
    QPainter p(&img);
    // A block a fifth of the frame across, inset so that scaling and the
    // even-dimension chop cannot clip it away.
    p.fillRect(w / 20, h / 20, w / 5, h / 5, QColor(255, 255, 255));
    p.end();

    QVideoFrameFormat fmt(img.size(),
        QVideoFrameFormat::pixelFormatFromImageFormat(img.format()));
    QVideoFrame frame(fmt);
    if (!frame.map(QVideoFrame::WriteOnly)) return {};
    for (int row = 0; row < img.height(); ++row)
        memcpy(frame.bits(0) + row * frame.bytesPerLine(0),
               img.constScanLine(row),
               size_t(qMin(int(img.bytesPerLine()), frame.bytesPerLine(0))));
    frame.unmap();
    frame.setRotation(rotation);
    frame.setMirrored(mirrored);
    return frame;
}

// Mean luma of a quadrant of an I420 frame. 0 = top-left, 1 = top-right,
// 2 = bottom-left, 3 = bottom-right.
double quadrantLuma(const PlanarFrame& f, int quadrant) {
    const int halfW = f.width / 2;
    const int halfH = f.height / 2;
    const int x0 = (quadrant % 2) ? halfW : 0;
    const int y0 = (quadrant / 2) ? halfH : 0;
    const auto* y = reinterpret_cast<const uint8_t*>(f.y.constData());
    double sum = 0;
    for (int row = y0; row < y0 + halfH; ++row)
        for (int col = x0; col < x0 + halfW; ++col)
            sum += y[row * f.strideY + col];
    return sum / double(halfW * halfH);
}

// Index of the brightest quadrant.
int brightestQuadrant(const PlanarFrame& f) {
    int best = 0;
    double bestVal = -1;
    for (int q = 0; q < 4; ++q) {
        const double v = quadrantLuma(f, q);
        if (v > bestVal) { bestVal = v; best = q; }
    }
    return best;
}

} // namespace

class TestMobileVideo : public QObject {
    Q_OBJECT
private slots:
    // ---- 1. Orientation ---------------------------------------------
    void normaliseFoldsAnyAngle();
    void wireRotationMatchesPresentation();
    void swapsAxesOnlyOnQuarterTurns();
    void wireIsNeverMirrored();
    void converterRotatesPortraitFrameUpright();
    void converterLeavesUnrotatedFrameAlone();
    void converterScalesBeforeRotating();

    // ---- 2. NV12 packing --------------------------------------------
    void nv12InterleavesChroma();
    void nv12HonoursPaddedDestinationStrides();
    void nv12RoundsOddDimensionsDown();
    void nv12RefusesImpossibleStrides();

    // ---- 3. Lazy camera permission ----------------------------------
    void permissionActionPerStatus();
    void permissionRefusalNamesARemedy();

    // ---- 4. What this build advertises ------------------------------
    void buildAdvertisesH264BothWays();
    void advertisedCodecsAreWhatPeersNegotiateOn();
    void softwareH264IsNotCompiledInOnApple();
};

// =====================================================================
// 1. Orientation
// =====================================================================

void TestMobileVideo::normaliseFoldsAnyAngle() {
    QCOMPARE(videoorient::normalise(0), 0);
    QCOMPARE(videoorient::normalise(90), 90);
    QCOMPARE(videoorient::normalise(180), 180);
    QCOMPARE(videoorient::normalise(270), 270);
    // Over-wound and negative — an angle arithmetic bug upstream must
    // not turn into an out-of-range rotation mode.
    QCOMPARE(videoorient::normalise(360), 0);
    QCOMPARE(videoorient::normalise(450), 90);
    QCOMPARE(videoorient::normalise(-90), 270);
    QCOMPARE(videoorient::normalise(-450), 270);
    // Off-axis rounds to the nearest quadrant rather than failing.
    QCOMPARE(videoorient::normalise(44), 0);
    QCOMPARE(videoorient::normalise(46), 90);
    QCOMPARE(videoorient::normalise(359), 0);
}

void TestMobileVideo::wireRotationMatchesPresentation() {
    // Not the inverse. Qt's angle says "turn it this way to show it
    // upright"; baking it upright means performing that same turn. An
    // inverted sign here is the classic upside-down-video bug and would
    // pass every symmetric test pattern.
    QCOMPARE(videoorient::wireRotation(0), 0);
    QCOMPARE(videoorient::wireRotation(90), 90);
    QCOMPARE(videoorient::wireRotation(180), 180);
    QCOMPARE(videoorient::wireRotation(270), 270);
}

void TestMobileVideo::swapsAxesOnlyOnQuarterTurns() {
    QVERIFY(!videoorient::swapsAxes(0));
    QVERIFY(videoorient::swapsAxes(90));
    QVERIFY(!videoorient::swapsAxes(180));
    QVERIFY(videoorient::swapsAxes(270));
    QVERIFY(videoorient::swapsAxes(-90));
}

void TestMobileVideo::wireIsNeverMirrored() {
    // The far end is looking at you, not at themselves. See the long
    // note in VideoFrameOrientation.h.
    QVERIFY(!videoorient::mirrorForWire(true));
    QVERIFY(!videoorient::mirrorForWire(false));
}

void TestMobileVideo::converterRotatesPortraitFrameUpright() {
    // A phone held in portrait: the sensor delivers landscape pixels and
    // Qt stamps Clockwise90 on the frame for the display path.
    QVideoFrame in = makeCorneredFrame(640, 360, QtVideo::Rotation::Clockwise90);
    QVERIFY(in.isValid());

    PlanarFrame f = FrameConverter::toI420(in, 0, 0);
    QVERIFY(f.isValid());
    // Dimensions must SWAP. VideoSendPipeline sizes the encoder session
    // from these, so getting this wrong is a session that rejects every
    // frame rather than a cosmetic problem.
    QCOMPARE(f.width, 360);
    QCOMPARE(f.height, 640);
    // A clockwise quarter turn moves the top-left corner to the top-right.
    QCOMPARE(brightestQuadrant(f), 1);
}

void TestMobileVideo::converterLeavesUnrotatedFrameAlone() {
    // The desktop screen-share path: rotation is always None, and this
    // must stay a no-op rather than quietly paying for a plane rotation
    // 30 times a second.
    QVideoFrame in = makeCorneredFrame(640, 360);
    PlanarFrame f = FrameConverter::toI420(in, 0, 0);
    QVERIFY(f.isValid());
    QCOMPARE(f.width, 640);
    QCOMPARE(f.height, 360);
    QCOMPARE(brightestQuadrant(f), 0);

    // 180° is the other case that does NOT swap axes, and is the one an
    // "if (rotation) swap" bug would get wrong.
    QVideoFrame flipped =
        makeCorneredFrame(640, 360, QtVideo::Rotation::Clockwise180);
    PlanarFrame g = FrameConverter::toI420(flipped, 0, 0);
    QVERIFY(g.isValid());
    QCOMPARE(g.width, 640);
    QCOMPARE(g.height, 360);
    QCOMPARE(brightestQuadrant(g), 3);   // top-left -> bottom-right
}

void TestMobileVideo::converterScalesBeforeRotating() {
    // Both constraints at once: the long edge is capped AND the frame is
    // turned. The cap applies to the long edge, which a quarter turn
    // does not change, so the result is the rotated shape at the capped
    // size — not the capped shape rotated into an over-size frame.
    QVideoFrame in = makeCorneredFrame(1920, 1080, QtVideo::Rotation::Clockwise90);
    PlanarFrame f = FrameConverter::toI420(in, 640, 0);
    QVERIFY(f.isValid());
    QCOMPARE(f.height, 640);
    QCOMPARE(f.width, 360);
    QCOMPARE(qMax(f.width, f.height), 640);
    QCOMPARE(brightestQuadrant(f), 1);
}

// =====================================================================
// 2. NV12 packing
// =====================================================================

void TestMobileVideo::nv12InterleavesChroma() {
    const int w = 8, h = 4;
    std::vector<uint8_t> y(w * h), u((w / 2) * (h / 2)), v((w / 2) * (h / 2));
    for (int i = 0; i < w * h; ++i) y[size_t(i)] = uint8_t(i);
    for (int i = 0; i < (w / 2) * (h / 2); ++i) {
        u[size_t(i)] = uint8_t(100 + i);
        v[size_t(i)] = uint8_t(200 + i);
    }

    std::vector<uint8_t> dstY(w * h, 0xAA);
    std::vector<uint8_t> dstUV(w * (h / 2), 0xAA);
    QVERIFY(nv12::packFromI420(y.data(), w, u.data(), w / 2, v.data(), w / 2,
                               dstY.data(), w, dstUV.data(), w, w, h));

    // Luma copies straight through.
    for (int i = 0; i < w * h; ++i) QCOMPARE(dstY[size_t(i)], uint8_t(i));
    // Chroma interleaves U,V,U,V — the whole point of the format, and
    // the byte order a swapped pair would turn into a purple picture.
    for (int row = 0; row < h / 2; ++row) {
        for (int col = 0; col < w / 2; ++col) {
            const int src = row * (w / 2) + col;
            QCOMPARE(dstUV[size_t(row * w + col * 2 + 0)], uint8_t(100 + src));
            QCOMPARE(dstUV[size_t(row * w + col * 2 + 1)], uint8_t(200 + src));
        }
    }
}

void TestMobileVideo::nv12HonoursPaddedDestinationStrides() {
    // A CVPixelBuffer's rows are padded to the hardware's alignment and
    // essentially never match the width. Writing at width-stride into a
    // padded buffer is the classic skewed-image bug.
    const int w = 8, h = 4;
    const int padY = 32, padUV = 48;
    std::vector<uint8_t> y(w * h), u((w / 2) * (h / 2)), v((w / 2) * (h / 2));
    for (int i = 0; i < w * h; ++i) y[size_t(i)] = uint8_t(i + 1);
    std::fill(u.begin(), u.end(), uint8_t(7));
    std::fill(v.begin(), v.end(), uint8_t(9));

    const uint8_t guard = 0x5A;
    std::vector<uint8_t> dstY(size_t(padY * h), guard);
    std::vector<uint8_t> dstUV(size_t(padUV * (h / 2)), guard);
    QVERIFY(nv12::packFromI420(y.data(), w, u.data(), w / 2, v.data(), w / 2,
                               dstY.data(), padY, dstUV.data(), padUV, w, h));

    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col)
            QCOMPARE(dstY[size_t(row * padY + col)], uint8_t(row * w + col + 1));
        // The padding past the visible width must be untouched.
        QCOMPARE(dstY[size_t(row * padY + w)], guard);
    }
    for (int row = 0; row < h / 2; ++row) {
        for (int col = 0; col < w / 2; ++col) {
            QCOMPARE(dstUV[size_t(row * padUV + col * 2 + 0)], uint8_t(7));
            QCOMPARE(dstUV[size_t(row * padUV + col * 2 + 1)], uint8_t(9));
        }
        QCOMPARE(dstUV[size_t(row * padUV + w)], guard);
    }
}

void TestMobileVideo::nv12RoundsOddDimensionsDown() {
    // 4:2:0 has no meaning at an odd size. Rounding down inside the
    // packer means a caller that hands over a real capture size cannot
    // walk off the end of a chroma plane.
    const int w = 9, h = 5;          // -> 8x4
    std::vector<uint8_t> y(size_t(w * h), 1), u(size_t(w * h), 2),
                         v(size_t(w * h), 3);
    std::vector<uint8_t> dstY(size_t(8 * 4), 0), dstUV(size_t(8 * 2), 0);
    QVERIFY(nv12::packFromI420(y.data(), w, u.data(), w / 2, v.data(), w / 2,
                               dstY.data(), 8, dstUV.data(), 8, w, h));
    QCOMPARE(dstY[0], uint8_t(1));
    QCOMPARE(dstUV[0], uint8_t(2));
    QCOMPARE(dstUV[1], uint8_t(3));
}

void TestMobileVideo::nv12RefusesImpossibleStrides() {
    const int w = 8, h = 4;
    std::vector<uint8_t> y(size_t(w * h), 0), u(size_t(w * h), 0),
                         v(size_t(w * h), 0);
    std::vector<uint8_t> dstY(size_t(w * h), 0), dstUV(size_t(w * h), 0);

    // A destination UV stride of w/2 is the tempting wrong answer: the
    // plane covers w/2 chroma COLUMNS but w interleaved BYTES.
    QVERIFY(!nv12::packFromI420(y.data(), w, u.data(), w / 2, v.data(), w / 2,
                                dstY.data(), w, dstUV.data(), w / 2, w, h));
    // Source stride below the width.
    QVERIFY(!nv12::packFromI420(y.data(), w - 1, u.data(), w / 2, v.data(), w / 2,
                                dstY.data(), w, dstUV.data(), w, w, h));
    // Null planes and empty sizes.
    QVERIFY(!nv12::packFromI420(nullptr, w, u.data(), w / 2, v.data(), w / 2,
                                dstY.data(), w, dstUV.data(), w, w, h));
    QVERIFY(!nv12::packFromI420(y.data(), w, u.data(), w / 2, v.data(), w / 2,
                                dstY.data(), w, dstUV.data(), w, 1, 1));
}

// =====================================================================
// 3. Lazy camera permission
// =====================================================================

void TestMobileVideo::permissionActionPerStatus() {
    using S = camperm::Status;
    using A = camperm::Action;
    // Undetermined is the ONLY status that prompts. That is what makes
    // the prompt lazy: it can fire at most once, and only from the one
    // call site behind the camera button.
    QCOMPARE(camperm::action(S::Undetermined), A::RequestThenStart);
    // Denied and Restricted both refuse — the OS will not ask again, so
    // there is nothing to wait for.
    QCOMPARE(camperm::action(S::Denied), A::Refuse);
    QCOMPARE(camperm::action(S::Restricted), A::Refuse);
    // Granted proceeds, and so does Unsupported: a platform with no
    // permission backend must behave exactly as it did before this
    // existed, not be refused by a query it cannot answer.
    QCOMPARE(camperm::action(S::Granted), A::Proceed);
    QCOMPARE(camperm::action(S::Unsupported), A::Proceed);
}

void TestMobileVideo::permissionRefusalNamesARemedy()
{
    // What this protects: a refusal must tell the user how to undo it. A
    // dead-end refusal is not hypothetical — the macOS prompt-denied
    // path used to say, in full, "Camera access denied."
    //
    // EVERY platform is checked here, not just the host's. The first
    // version of this case asserted that the message contains "Settings"
    // — right on iOS, Android, macOS and Windows, wrong on Linux, where
    // there is no per-app camera permission to send anyone to. It passed
    // on macOS and failed the Linux and Windows CI jobs. camperm takes
    // the platform as a parameter now precisely so that a Mac can catch
    // that.
    const camperm::Platform all[] = {
        camperm::Platform::IOS,     camperm::Platform::MacOS,
        camperm::Platform::Android, camperm::Platform::Windows,
        camperm::Platform::OtherDesktop,
    };

    for (camperm::Platform p : all) {
        const QString denied = camperm::refusalMessageFor(
            camperm::Status::Denied, p);
        const QString where = camperm::settingsPathFor(p);
        QVERIFY2(!denied.isEmpty(), "every platform needs a refusal");

        if (!where.isEmpty()) {
            // Not a tautology: settingsPathFor() and refusalMessageFor()
            // are separate, so a message that stopped interpolating the
            // location, or named a different one, fails here.
            QVERIFY2(denied.contains(where),
                     qPrintable(QStringLiteral(
                         "refusal must name where to change the decision "
                         "(expected \"%1\") but said: %2").arg(where, denied)));
            // A destination, not a description. "your system privacy
            // settings" is what Windows used to get.
            QVERIFY2(where.contains(QStringLiteral(">")),
                     qPrintable(QStringLiteral(
                         "settings path should be navigable, got: %1").arg(where)));
        } else {
            // No per-app permission exists, so naming a settings pane
            // would send the user after a switch that is not there. The
            // message must still be actionable — it names the two things
            // that actually stop a camera opening.
            QVERIFY2(denied.contains(QStringLiteral("other"))
                         && denied.contains(QStringLiteral("permission")),
                     qPrintable(QStringLiteral(
                         "with no settings pane to point at, the refusal "
                         "must still name a cause, but said: %1").arg(denied)));
        }

        // Restricted is the other half: there IS no switch, and saying so
        // is the remedy — otherwise the user hunts for one that does not
        // exist.
        const QString restricted = camperm::refusalMessageFor(
            camperm::Status::Restricted, p);
        QVERIFY(!restricted.isEmpty());
        QVERIFY(restricted != denied);
        QVERIFY(restricted.contains(QStringLiteral("restricted")));
        if (!where.isEmpty())
            QVERIFY2(!restricted.contains(where),
                     "a restricted device has no settings switch to offer, "
                     "so the message must not point at one");
    }

    // macOS is the one platform where "try again" is wrong: a TCC change
    // does not reach an already-running process, so the user must
    // relaunch. Checked explicitly because it is the kind of
    // platform-specific truth a later tidy-up would flatten away.
    QVERIFY(camperm::refusalMessageFor(camperm::Status::Denied,
                                       camperm::Platform::MacOS)
                .contains(QStringLiteral("restart")));
    QVERIFY(!camperm::refusalMessageFor(camperm::Status::Denied,
                                        camperm::Platform::IOS)
                 .contains(QStringLiteral("restart")));

    // And the host wiring is real — hostPlatform() must actually select
    // one of the above, not fall through to a default nobody meant.
    QCOMPARE(camperm::refusalMessage(camperm::Status::Denied),
             camperm::refusalMessageFor(camperm::Status::Denied,
                                        camperm::hostPlatform()));
#if defined(Q_OS_MACOS)
    QCOMPARE(camperm::hostPlatform(), camperm::Platform::MacOS);
#elif defined(Q_OS_WIN)
    QCOMPARE(camperm::hostPlatform(), camperm::Platform::Windows);
#elif defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    QCOMPARE(camperm::hostPlatform(), camperm::Platform::OtherDesktop);
#endif
}

// =====================================================================
// 4. What this build advertises and negotiates
// =====================================================================

void TestMobileVideo::buildAdvertisesH264BothWays() {
    // The regression this guards is the one the iOS build shipped with:
    // no video backend compiled in at all, so both profile lists came
    // back empty and localCapsJson() advertised video_codecs: [].
    //
    // On an Apple host this exercises the same BSFCHAT_HAVE_VIDEOTOOLBOX
    // branch of VideoCodecFactory that an iOS build now takes, which is
    // as close as a machine without a phone attached can get.
    QVERIFY2(!VideoDecoder::h264DecodeProfiles().isEmpty(),
             "this build can decode no H.264 — every peer will be told to "
             "send JPEG stills. If this fires, the compiled-in backend "
             "(VideoToolbox / Media Foundation / openh264) reported no "
             "decode profiles.");
    QVERIFY2(!VideoEncoder::h264EncodeProfiles().isEmpty(),
             "this build can encode no H.264 — VideoSendPipeline will find "
             "no backend and send nothing. If this fires, the compiled-in "
             "backend reported no encode profiles.");

    auto encoder = VideoEncoder::create(VideoCodecKind::H264);
    QVERIFY2(encoder != nullptr, "no H.264 encoder backend");

#ifdef BSFCHAT_HAVE_VIDEOTOOLBOX
    // And it is the HARDWARE one. Software H.264 on a phone is the
    // outcome this port exists to avoid: it is battery, heat and
    // sustained thermal throttling that degrades the whole call.
    QVERIFY(VideoEncoder::queryCaps(VideoCodecKind::H264).hardware);
    QVERIFY(VideoEncoder::h264EncodeProfiles().contains(QStringLiteral("high")));
#endif
}

void TestMobileVideo::advertisedCodecsAreWhatPeersNegotiateOn() {
    // Build the advertisement the same way VoiceEngine::localCapsJson
    // does, then ask the two predicates that actually route video.
    PeerCaps caps;
    caps.videoRtp = true;
    caps.controlDc = true;
    caps.h264ProfilesEncode = VideoEncoder::h264EncodeProfiles();
    caps.h264ProfilesDecode = VideoDecoder::h264DecodeProfiles();
    if (!caps.h264ProfilesEncode.isEmpty() || !caps.h264ProfilesDecode.isEmpty())
        caps.videoCodecs << videoCodecIdH264();

    QVERIFY(caps.videoCodecs.contains(videoCodecIdH264()));

    // A peer with these caps and an open track receives real video...
    QVERIFY(peerCanReceiveRtpVideo(caps, /*capsKnown=*/true,
                                   videoCodecIdH264()));
    QVERIFY(!peerNeedsLegacyJpeg(caps, /*capsKnown=*/true,
                                 /*videoTrackOpen=*/true, videoCodecIdH264()));
    // ...and still gets JPEG during the pre-track gap, which is the
    // fallback doing its job rather than the bug.
    QVERIFY(peerNeedsLegacyJpeg(caps, /*capsKnown=*/true,
                                /*videoTrackOpen=*/false, videoCodecIdH264()));

    // The shape an iOS build used to have: no codecs advertised at all.
    // Everyone is pushed to JPEG. Kept as an explicit contrast so the
    // failure this port fixed stays legible.
    PeerCaps codecless;
    codecless.videoRtp = true;
    QVERIFY(!peerCanReceiveRtpVideo(codecless, true, videoCodecIdH264()));
    QVERIFY(peerNeedsLegacyJpeg(codecless, true, true, videoCodecIdH264()));
}

void TestMobileVideo::softwareH264IsNotCompiledInOnApple() {
#ifndef Q_OS_MACOS
    QSKIP("Apple-host assertion");
#else
    // Not a style preference — a build-shape assertion. openh264 is
    // vendored for desktop macOS and Linux and deliberately excluded on
    // both mobile platforms, and the VideoToolbox path this port opened
    // on iOS is what makes that exclusion survivable. If a future change
    // adds openh264 to the mobile dependency set, the hardware-first
    // ordering in VideoCodecFactory is the only thing left keeping a
    // phone off a CPU encoder.
    QVERIFY2(VideoEncoder::queryCaps(VideoCodecKind::H264).hardware,
             "VideoCodecFactory did not pick a hardware encoder on an "
             "Apple host — check the create() ordering before shipping "
             "this to a phone");
#endif
}

QTEST_GUILESS_MAIN(TestMobileVideo)
#include "test_mobile_video.moc"
