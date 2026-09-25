// The parts of the MOBILE camera-video ports — iOS/VideoToolbox first,
// then Android/MediaCodec — that can be tested without hardware, a
// camera, or a phone.
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
//   5. The two platform-free halves of the Android MediaCodec backend:
//      the buffer-geometry resolution (MediaCodecLayout.h) and the
//      Annex-B inspection (MediaCodecAnnexB.h). These get NO other
//      coverage anywhere — the Android CI job builds no tests and there
//      is no device in CI — and they are where the arithmetic that
//      silently shears a picture lives. Every case here runs on the
//      desktop host, with no NDK involved.
//
// The device-only half is listed in docs/ios-voice.md and
// docs/android-video.md.

#include <QtTest/QtTest>
#include <QImage>
#include <QPainter>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include "voice/CameraPermissionPolicy.h"
#include "voice/PeerCaps.h"
#include "voice/video/FrameConverter.h"
#include "voice/video/MediaCodecAnnexB.h"
#include "voice/video/MediaCodecLayout.h"
#include "voice/video/NV12Pack.h"
#include "voice/video/VideoCodec.h"
#include "voice/video/VideoDecoder.h"
#include "voice/video/VideoEncoder.h"
#include "voice/video/VideoFrameOrientation.h"

#include <algorithm>
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
    && !defined(BSFCHAT_HAVE_MEDIACODEC) \
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

    // ---- 5. Android MediaCodec: buffer geometry + bitstream ---------
    void layoutResolvesTheTwoPlanarFamilies();
    void layoutRefusesWhatCannotBeCopiedLinearly();
    void layoutDefaultsAbsentAndNonsensePadding();
    void packInputWritesNV12AtCodecStride();
    void packInputWritesI420AtCodecStride();
    void packInputRefusesAnUndersizedCodecBuffer();
    void unpackOutputDropsCodecPadding();
    void unpackOutputRoundTripsThroughBothFamilies();
    void requiredBytesIsWhatIsWrittenNotThePlaneExtent();
    void pixel6ProShortInputBufferIsAccepted();
    void requiredBytesRefusesAPictureBiggerThanItsGeometry();
    void annexBFindsSliceTypesAndParameterSets();
    void annexBHandlesThreeByteStartCodes();
    void annexBTakesTheLastParameterSetPair();
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

// =====================================================================
// 5. Android MediaCodec: buffer geometry + bitstream inspection
//
// Nothing below touches the NDK. MediaCodecLayout.cpp and
// MediaCodecAnnexB.cpp are the two halves of the Android backend that
// are pure arithmetic and memory, and they are the halves most likely to
// be quietly wrong — a stride mistaken for a width is a green band down
// one side, a slice height mistaken for a height is garbage across the
// bottom eighth of every 1080p frame, and both survive a code review.
//
// These run on the desktop host precisely because the Android build
// gets no test run anywhere: the CI job passes
// -DGAMECHAT_CLIENT_BUILD_TESTS=OFF and a hosted runner has no phone.
// =====================================================================

namespace {

// A source frame in the exact shape FrameConverter produces: tightly
// packed I420, strideY == w, strideU == strideV == w/2. Every plane
// gets a distinct, position-dependent ramp so a plane swap, a row
// offset or a stride confusion all show up as a value mismatch rather
// than as a test that still passes on uniform grey.
struct SourceI420 {
    std::vector<uint8_t> y, u, v;
    int w = 0, h = 0;
    const uint8_t* yp() const { return y.data(); }
    const uint8_t* up() const { return u.data(); }
    const uint8_t* vp() const { return v.data(); }
    uint8_t yAt(int r, int c) const { return y[size_t(r) * size_t(w) + size_t(c)]; }
    uint8_t uAt(int r, int c) const { return u[size_t(r) * size_t(w / 2) + size_t(c)]; }
    uint8_t vAt(int r, int c) const { return v[size_t(r) * size_t(w / 2) + size_t(c)]; }
};

SourceI420 makeSource(int w, int h) {
    SourceI420 f;
    f.w = w;
    f.h = h;
    f.y.resize(size_t(w) * size_t(h));
    f.u.resize(size_t(w / 2) * size_t(h / 2));
    f.v.resize(size_t(w / 2) * size_t(h / 2));
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c)
            f.y[size_t(r) * size_t(w) + size_t(c)] = uint8_t((r * 7 + c * 3) % 200);
    for (int r = 0; r < h / 2; ++r) {
        for (int c = 0; c < w / 2; ++c) {
            f.u[size_t(r) * size_t(w / 2) + size_t(c)] = uint8_t((r * 5 + c * 11) % 200);
            f.v[size_t(r) * size_t(w / 2) + size_t(c)] = uint8_t((r * 13 + c * 2) % 200);
        }
    }
    return f;
}

// The byte a MediaCodec buffer's padding is filled with before a test
// writes into it. Chosen above every ramp value above, so "did the
// padding leak into the picture" is a value comparison and not a guess.
constexpr uint8_t kPad = 0xFF;

QByteArray nal(uint8_t type, int payloadBytes = 3, bool shortStartCode = false) {
    QByteArray b;
    if (shortStartCode) b.append(QByteArray::fromHex("000001"));
    else                b.append(QByteArray::fromHex("00000001"));
    b.append(char(type & 0x1F));           // forbidden_zero=0, nal_ref_idc=0
    for (int i = 0; i < payloadBytes; ++i) b.append(char(0x40 + i));
    return b;
}

} // namespace

void TestMobileVideo::layoutResolvesTheTwoPlanarFamilies() {
    using mediacodec::BufferLayout;

    // Standard OMX planar and its "packed" sibling are both I420.
    QCOMPARE(mediacodec::layoutFor(mediacodec::kFormatYUV420Planar,
                                   640, 480, 640, 480).plane,
             BufferLayout::Plane::I420);
    QCOMPARE(mediacodec::layoutFor(mediacodec::kFormatYUV420PackedPlanar,
                                   640, 480, 640, 480).plane,
             BufferLayout::Plane::I420);

    // Standard semiplanar and the vendor spellings of it are all NV12.
    for (int32_t cf : {mediacodec::kFormatYUV420SemiPlanar,
                       mediacodec::kFormatYUV420PackedSemiPlanar,
                       mediacodec::kFormatTiYUV420PackedSemiPlanar,
                       mediacodec::kFormatQcomYUV420SemiPlanar,
                       mediacodec::kFormatQcomYUV420PackedSemiPlanar32m}) {
        QCOMPARE(mediacodec::layoutFor(cf, 640, 480, 640, 480).plane,
                 BufferLayout::Plane::NV12);
    }

    // Both families come to the same buffer size: Y is stride*slice and
    // chroma is exactly half of it, however it is arranged.
    const auto i420 = mediacodec::layoutFor(mediacodec::kFormatYUV420Planar,
                                            704, 488, 640, 480);
    const auto nv12 = mediacodec::layoutFor(mediacodec::kFormatYUV420SemiPlanar,
                                            704, 488, 640, 480);
    QCOMPARE(i420.sizeBytes(), size_t(704 * 488 * 3 / 2));
    QCOMPARE(nv12.sizeBytes(), i420.sizeBytes());
}

void TestMobileVideo::layoutRefusesWhatCannotBeCopiedLinearly() {
    using mediacodec::BufferLayout;

    // Qualcomm's 64x32 tiled layout sits numerically BETWEEN two vendor
    // semiplanar formats that are plain NV12, so the risk is not that
    // someone adds it deliberately — it is that a range check or a
    // careless default sweeps it in. A linear row copy of a tiled buffer
    // is confetti.
    QCOMPARE(mediacodec::layoutFor(
                 mediacodec::kFormatQcomYUV420PackedSemiPlanarTiled,
                 640, 480, 640, 480).plane,
             BufferLayout::Plane::Unsupported);

    // Flexible is the ASK, never an answer. A codec that echoes it back
    // has not told us what it chose, and guessing is how you ship a
    // backend that works on the reviewer's phone and nobody else's.
    QCOMPARE(mediacodec::layoutFor(mediacodec::kFormatYUV420Flexible,
                                   640, 480, 640, 480).plane,
             BufferLayout::Plane::Unsupported);

    // Surface/opaque and anything unrecognised.
    QCOMPARE(mediacodec::layoutFor(0x7F000789, 640, 480, 640, 480).plane,
             BufferLayout::Plane::Unsupported);
    QVERIFY(!mediacodec::layoutFor(0x7F000789, 640, 480, 640, 480).isValid());
    QCOMPARE(mediacodec::layoutFor(0x7F000789, 640, 480, 640, 480).sizeBytes(),
             size_t(0));

    // A degenerate picture is not a layout either.
    QVERIFY(!mediacodec::layoutFor(mediacodec::kFormatYUV420SemiPlanar,
                                   0, 0, 0, 0).isValid());
}

void TestMobileVideo::layoutDefaultsAbsentAndNonsensePadding() {
    // AMediaFormat_getInt32 leaves the out-param at 0 when the codec did
    // not set the key. Absent means "no padding", which is width and
    // height — not zero, which would make every buffer look empty.
    auto absent = mediacodec::layoutFor(mediacodec::kFormatYUV420SemiPlanar,
                                        /*stride=*/0, /*slice=*/0, 640, 480);
    QCOMPARE(absent.stride, 640);
    QCOMPARE(absent.sliceHeight, 480);

    // A key SMALLER than the picture is a codec reporting nonsense.
    // Honouring it would read or write outside the frame, so it is
    // treated exactly like an absent key.
    auto tooSmall = mediacodec::layoutFor(mediacodec::kFormatYUV420SemiPlanar,
                                          320, 240, 640, 480);
    QCOMPARE(tooSmall.stride, 640);
    QCOMPARE(tooSmall.sliceHeight, 480);

    // Real padding IS honoured — this is the 1080 -> 1088 case, the one
    // that puts a band of garbage across the bottom of every frame when
    // it is missed.
    auto padded = mediacodec::layoutFor(mediacodec::kFormatYUV420SemiPlanar,
                                        1920, 1088, 1920, 1080);
    QCOMPARE(padded.stride, 1920);
    QCOMPARE(padded.sliceHeight, 1088);
    QCOMPARE(padded.sizeBytes(), size_t(1920) * 1088 * 3 / 2);

    // The planar family halves the luma stride for its chroma planes, so
    // an odd stride would lose a column's worth of addressing.
    auto oddStride = mediacodec::layoutFor(mediacodec::kFormatYUV420Planar,
                                           641, 480, 640, 480);
    QCOMPARE(oddStride.stride % 2, 0);
}

void TestMobileVideo::packInputWritesNV12AtCodecStride() {
    const int w = 64, h = 32;
    const SourceI420 src = makeSource(w, h);
    // Padding on BOTH axes, and different amounts, so a test that passes
    // cannot be passing because the two happened to be interchangeable.
    const auto layout = mediacodec::layoutFor(
        mediacodec::kFormatYUV420SemiPlanar, /*stride=*/w + 32,
        /*slice=*/h + 8, w, h);
    QVERIFY(layout.isValid());

    std::vector<uint8_t> buf(layout.sizeBytes(), kPad);
    QVERIFY(mediacodec::packInput(layout, src.yp(), w, src.up(), w / 2,
                                  src.vp(), w / 2, buf.data(), buf.size(),
                                  w, h));

    // Luma sits at the codec's stride, not the frame's width.
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c)
            QCOMPARE(buf[size_t(r) * size_t(layout.stride) + size_t(c)],
                     src.yAt(r, c));

    // Chroma begins after stride*sliceHeight — NOT after stride*height,
    // which is the same number only when the codec asked for no vertical
    // padding.
    const size_t uv = size_t(layout.stride) * size_t(layout.sliceHeight);
    for (int r = 0; r < h / 2; ++r) {
        for (int c = 0; c < w / 2; ++c) {
            const size_t base = uv + size_t(r) * size_t(layout.stride)
                              + size_t(c) * 2;
            QCOMPARE(buf[base], src.uAt(r, c));       // U then V, NV12
            QCOMPARE(buf[base + 1], src.vAt(r, c));
        }
    }

    // The row padding was not written through.
    QCOMPARE(buf[size_t(layout.stride) - 1], kPad);
}

void TestMobileVideo::packInputWritesI420AtCodecStride() {
    const int w = 64, h = 32;
    const SourceI420 src = makeSource(w, h);
    const auto layout = mediacodec::layoutFor(
        mediacodec::kFormatYUV420Planar, w + 16, h + 4, w, h);
    QVERIFY(layout.isValid());

    std::vector<uint8_t> buf(layout.sizeBytes(), kPad);
    QVERIFY(mediacodec::packInput(layout, src.yp(), w, src.up(), w / 2,
                                  src.vp(), w / 2, buf.data(), buf.size(),
                                  w, h));

    const size_t lumaBytes = size_t(layout.stride) * size_t(layout.sliceHeight);
    const int chromaStride = layout.stride / 2;
    const size_t uOff = lumaBytes;
    const size_t vOff = lumaBytes
                      + size_t(chromaStride) * size_t(layout.sliceHeight / 2);

    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c)
            QCOMPARE(buf[size_t(r) * size_t(layout.stride) + size_t(c)],
                     src.yAt(r, c));
    for (int r = 0; r < h / 2; ++r) {
        for (int c = 0; c < w / 2; ++c) {
            QCOMPARE(buf[uOff + size_t(r) * size_t(chromaStride) + size_t(c)],
                     src.uAt(r, c));
            // V after U, and at the HALF slice height. Swapping these
            // two planes is the classic I420/YV12 mix-up and shows up as
            // a picture with the blues and reds exchanged.
            QCOMPARE(buf[vOff + size_t(r) * size_t(chromaStride) + size_t(c)],
                     src.vAt(r, c));
        }
    }
}

void TestMobileVideo::packInputRefusesAnUndersizedCodecBuffer() {
    const int w = 64, h = 32;
    const SourceI420 src = makeSource(w, h);
    const auto layout = mediacodec::layoutFor(
        mediacodec::kFormatYUV420SemiPlanar, w, h, w, h);
    std::vector<uint8_t> buf(layout.sizeBytes(), kPad);

    // One byte short of what the layout needs. The codec handing back a
    // buffer smaller than its own declared geometry should be
    // impossible; writing past the end of it if it happens must not be.
    QVERIFY(!mediacodec::packInput(layout, src.yp(), w, src.up(), w / 2,
                                   src.vp(), w / 2, buf.data(),
                                   layout.sizeBytes() - 1, w, h));
    // An unresolved layout takes nothing on trust either.
    const mediacodec::BufferLayout unusable;
    QVERIFY(!mediacodec::packInput(unusable, src.yp(), w, src.up(), w / 2,
                                   src.vp(), w / 2, buf.data(), buf.size(),
                                   w, h));
    // A source stride too small for its own row.
    QVERIFY(!mediacodec::packInput(layout, src.yp(), w - 1, src.up(), w / 2,
                                   src.vp(), w / 2, buf.data(), buf.size(),
                                   w, h));
    // Nulls.
    QVERIFY(!mediacodec::packInput(layout, nullptr, w, src.up(), w / 2,
                                   src.vp(), w / 2, buf.data(), buf.size(),
                                   w, h));
}

void TestMobileVideo::unpackOutputDropsCodecPadding() {
    // The decoder side of the 1080 -> 1088 problem, at test scale: the
    // codec hands back a buffer taller and wider than the picture, and
    // everything outside the picture is whatever was in that memory
    // before. If the unpack reads at the buffer's geometry instead of
    // the picture's, the padding lands on screen.
    const int w = 48, h = 24;
    const int stride = w + 24, slice = h + 6;
    const auto layout = mediacodec::layoutFor(
        mediacodec::kFormatYUV420SemiPlanar, stride, slice, w, h);
    QVERIFY(layout.isValid());

    std::vector<uint8_t> codecBuf(layout.sizeBytes(), kPad);
    // Write a picture into the valid region only; the padding stays kPad.
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c)
            codecBuf[size_t(r) * size_t(stride) + size_t(c)] =
                uint8_t((r * 7 + c * 3) % 200);
    const size_t uv = size_t(stride) * size_t(slice);
    for (int r = 0; r < h / 2; ++r)
        for (int c = 0; c < w; ++c)
            codecBuf[uv + size_t(r) * size_t(stride) + size_t(c)] =
                uint8_t((r * 5 + c * 11) % 200);

    std::vector<uint8_t> dstY(size_t(w) * size_t(h), 0);
    std::vector<uint8_t> dstUV(size_t(w) * size_t(h / 2), 0);
    QVERIFY(mediacodec::unpackOutputToNV12(layout, codecBuf.data(),
                                           codecBuf.size(), dstY.data(), w,
                                           dstUV.data(), w, w, h));

    for (int r = 0; r < h; ++r) {
        for (int c = 0; c < w; ++c) {
            QCOMPARE(dstY[size_t(r) * size_t(w) + size_t(c)],
                     uint8_t((r * 7 + c * 3) % 200));
        }
    }
    for (int r = 0; r < h / 2; ++r) {
        for (int c = 0; c < w; ++c) {
            QCOMPARE(dstUV[size_t(r) * size_t(w) + size_t(c)],
                     uint8_t((r * 5 + c * 11) % 200));
        }
    }
    // Belt and braces: not one padding byte reached the output.
    QVERIFY(std::find(dstY.begin(), dstY.end(), kPad) == dstY.end());
    QVERIFY(std::find(dstUV.begin(), dstUV.end(), kPad) == dstUV.end());
}

void TestMobileVideo::unpackOutputRoundTripsThroughBothFamilies() {
    // Pack a frame the way the ENCODER would, unpack it the way the
    // DECODER would, and require the result to equal what NV12Pack —
    // the independently written Apple-side packer already under test
    // above — produces from the same source. Both MediaCodec families
    // must land on the same pixels, or a call between two Android phones
    // whose codecs chose different formats shows one of them a different
    // picture.
    const int w = 32, h = 16;
    const SourceI420 src = makeSource(w, h);

    std::vector<uint8_t> refY(size_t(w) * size_t(h));
    std::vector<uint8_t> refUV(size_t(w) * size_t(h / 2));
    QVERIFY(nv12::packFromI420(src.yp(), w, src.up(), w / 2, src.vp(), w / 2,
                               refY.data(), w, refUV.data(), w, w, h));

    for (int32_t cf : {mediacodec::kFormatYUV420SemiPlanar,
                       mediacodec::kFormatYUV420Planar}) {
        const auto layout = mediacodec::layoutFor(cf, w + 8, h + 2, w, h);
        QVERIFY(layout.isValid());
        std::vector<uint8_t> codecBuf(layout.sizeBytes(), kPad);
        QVERIFY(mediacodec::packInput(layout, src.yp(), w, src.up(), w / 2,
                                      src.vp(), w / 2, codecBuf.data(),
                                      codecBuf.size(), w, h));

        std::vector<uint8_t> outY(size_t(w) * size_t(h), 0);
        std::vector<uint8_t> outUV(size_t(w) * size_t(h / 2), 0);
        QVERIFY(mediacodec::unpackOutputToNV12(layout, codecBuf.data(),
                                               codecBuf.size(), outY.data(), w,
                                               outUV.data(), w, w, h));
        QVERIFY2(outY == refY, "luma differs from the NV12Pack reference");
        QVERIFY2(outUV == refUV, "chroma differs from the NV12Pack reference");
    }
}

void TestMobileVideo::requiredBytesIsWhatIsWrittenNotThePlaneExtent() {
    // The distinction that cost all of the video on a Pixel 6 Pro.
    //
    // sizeBytes() is the geometry the codec REPORTED: every row of every
    // plane at full stride. requiredBytes() is the offset one past the
    // last byte libyuv actually writes, which is smaller whenever the
    // picture is narrower than the stride or shorter than the slice
    // height — i.e. almost always.
    const auto nv12 = mediacodec::layoutFor(
        mediacodec::kFormatYUV420SemiPlanar, 640, 640, 480, 640);
    QCOMPARE(nv12.sizeBytes(), size_t(614400));
    // Y: rows 0..639 x 480 bytes at stride 640, all below the chroma
    // offset. UV: base 640*640, rows 0..319 x 480 bytes at stride 640.
    // Last byte index 409600 + 319*640 + 480 - 1 = 614239.
    QCOMPARE(nv12.requiredBytes(480, 640), size_t(614240));
    QVERIFY(nv12.requiredBytes(480, 640) < nv12.sizeBytes());

    const auto i420 = mediacodec::layoutFor(
        mediacodec::kFormatYUV420Planar, 640, 640, 480, 640);
    // V plane base is 640*640 + 320*320; last row 319 x 240 bytes at
    // chroma stride 320.
    QCOMPARE(i420.requiredBytes(480, 640), size_t(614320));
    QVERIFY(i420.requiredBytes(480, 640) < i420.sizeBytes());

    // With no padding anywhere the two converge to within one row's
    // worth of padding — requiredBytes is never LARGER than the extent.
    for (int32_t cf : {mediacodec::kFormatYUV420SemiPlanar,
                       mediacodec::kFormatYUV420Planar}) {
        const auto tight = mediacodec::layoutFor(cf, 640, 480, 640, 480);
        QVERIFY(tight.requiredBytes(640, 480) <= tight.sizeBytes());
    }
}

void TestMobileVideo::pixel6ProShortInputBufferIsAccepted() {
    // The exact device case, pinned.
    //
    // Pixel 6 Pro (Tensor G1), camera portrait: FrameConverter rotates
    // the 640x480 sensor frame upright to 480x640 and VideoSendPipeline
    // sizes the encoder from the ROTATED dimensions. The codec resolved
    // to stride 640 / slice-height 640 and handed back an input buffer
    // of 614338 bytes.
    //
    // 614338 is not divisible by 3, so it is not a 4:2:0 frame size at
    // all — it is whatever the Codec2 graphic allocator rounded to, and
    // requiring it to equal stride*sliceHeight*3/2 = 614400 was never
    // justified. Every frame was refused by 62 bytes, for the length of
    // the call, and because the desktop peer CAN receive H.264 the JPEG
    // fallback stayed off: the phone sent nothing at all.
    constexpr size_t kDeviceCapacity = 614338;
    const int w = 480, h = 640;

    for (int32_t cf : {mediacodec::kFormatYUV420SemiPlanar,
                       mediacodec::kFormatYUV420Planar}) {
        const auto layout = mediacodec::layoutFor(cf, 640, 640, w, h);
        QVERIFY(layout.isValid());
        QVERIFY2(layout.sizeBytes() > kDeviceCapacity,
                 "the plane extent must still EXCEED the device's buffer — "
                 "if it stopped doing so this test would pass for the wrong "
                 "reason and stop guarding anything");
        QVERIFY2(layout.requiredBytes(w, h) <= kDeviceCapacity,
                 "this is the regression: the bytes actually written must "
                 "fit in the buffer the Pixel 6 Pro really hands back");

        // And the pack must genuinely succeed against a buffer of
        // exactly that size, writing nothing past its end.
        const SourceI420 src = makeSource(w, h);
        std::vector<uint8_t> buf(kDeviceCapacity + 1, kPad);
        QVERIFY2(mediacodec::packInput(layout, src.yp(), w, src.up(), w / 2,
                                       src.vp(), w / 2, buf.data(),
                                       kDeviceCapacity, w, h),
                 "packInput refused the real device buffer");
        // The guard byte one past the declared capacity is untouched.
        QCOMPARE(buf[kDeviceCapacity], kPad);
        // Spot-check that the picture really landed: first and last
        // luma row, and the last chroma row, which is the one the
        // 62-byte shortfall sat in.
        QCOMPARE(buf[0], src.yAt(0, 0));
        QCOMPARE(buf[size_t(h - 1) * 640], src.yAt(h - 1, 0));
        if (layout.plane == mediacodec::BufferLayout::Plane::NV12) {
            const size_t lastUvRow = size_t(640) * 640 + size_t(h / 2 - 1) * 640;
            QCOMPARE(buf[lastUvRow], src.uAt(h / 2 - 1, 0));
            QCOMPARE(buf[lastUvRow + 1], src.vAt(h / 2 - 1, 0));
        }
    }

    // One byte less than needed is still refused. The relaxation must
    // be exactly "what is written", not "near enough".
    const auto layout = mediacodec::layoutFor(
        mediacodec::kFormatYUV420SemiPlanar, 640, 640, w, h);
    const SourceI420 src = makeSource(w, h);
    std::vector<uint8_t> buf(layout.sizeBytes(), kPad);
    QVERIFY(!mediacodec::packInput(layout, src.yp(), w, src.up(), w / 2,
                                   src.vp(), w / 2, buf.data(),
                                   layout.requiredBytes(w, h) - 1, w, h));
}

void TestMobileVideo::requiredBytesRefusesAPictureBiggerThanItsGeometry() {
    // A picture that does not fit is not a SMALLER requirement. If this
    // returned a plausible-looking number instead of 0, the one case
    // that must fail would be the one case that always passes — the
    // caller's test is `capacity < need`, and every capacity is >= 0.
    const auto layout = mediacodec::layoutFor(
        mediacodec::kFormatYUV420SemiPlanar, 640, 480, 640, 480);
    // Note the even-rounding happens FIRST, exactly as it does in
    // packInput: 641 becomes 640 and genuinely does fit, so the
    // oversize cases have to be oversize after the round-down.
    QCOMPARE(layout.requiredBytes(641, 480), layout.requiredBytes(640, 480));
    QCOMPARE(layout.requiredBytes(642, 480), size_t(0));
    QCOMPARE(layout.requiredBytes(640, 482), size_t(0));
    QCOMPARE(layout.requiredBytes(0, 0), size_t(0));
    QCOMPARE(mediacodec::BufferLayout().requiredBytes(640, 480), size_t(0));

    // And packInput must refuse those outright, however big the buffer.
    const SourceI420 src = makeSource(640, 480);
    std::vector<uint8_t> huge(4u * 1024 * 1024, kPad);
    QVERIFY(!mediacodec::packInput(layout, src.yp(), 640, src.up(), 320,
                                   src.vp(), 320, huge.data(), huge.size(),
                                   642, 480));
    QVERIFY(!mediacodec::unpackOutputToNV12(mediacodec::BufferLayout(),
                                            huge.data(), huge.size(),
                                            huge.data(), 640, huge.data(), 640,
                                            640, 480));
}

void TestMobileVideo::annexBFindsSliceTypesAndParameterSets() {
    // The shape MediaCodec delivers at the head of a stream: SPS+PPS in
    // a codec-config buffer, then bare IDRs. The encoder has to notice
    // the keyframe carries no parameter sets, or a receiver joining
    // mid-call never gets them and stares at a black tile.
    const QByteArray config = nal(7) + nal(8);
    const annexb::Summary c = annexb::scan(config);
    QVERIFY(c.hasSps);
    QVERIFY(c.hasPps);
    QVERIFY(!c.hasIdr);
    QVERIFY(!c.hasSlice);

    const annexb::Summary bareIdr = annexb::scan(nal(5));
    QVERIFY(bareIdr.hasIdr);
    QVERIFY(bareIdr.hasSlice);
    QVERIFY2(!bareIdr.hasSps,
             "a bare IDR must report NO parameter sets — this is the test "
             "the encoder uses to decide whether to prepend its stashed "
             "SPS/PPS, and a false positive here is a keyframe no "
             "mid-call joiner can decode");

    // The other device behaviour: parameter sets inlined before every
    // IDR. Prepending again would put two copies on the wire.
    const annexb::Summary inlined = annexb::scan(nal(7) + nal(8) + nal(5));
    QVERIFY(inlined.hasSps);
    QVERIFY(inlined.hasIdr);

    // A P-frame is a slice but not a keyframe.
    const annexb::Summary p = annexb::scan(nal(1));
    QVERIFY(p.hasSlice);
    QVERIFY(!p.hasIdr);

    // Nothing at all.
    const annexb::Summary empty = annexb::scan(QByteArray());
    QVERIFY(!empty.hasSlice && !empty.hasSps && !empty.hasIdr);
}

void TestMobileVideo::annexBHandlesThreeByteStartCodes() {
    // Annex-B allows both 3- and 4-byte start codes and encoders mix
    // them. A parser that only knows the 4-byte form silently sees one
    // giant NAL and reports whatever the first byte happened to be.
    const annexb::Summary mixed =
        annexb::scan(nal(7, 3, /*shortStartCode=*/true) + nal(8) + nal(5, 3, true));
    QVERIFY(mixed.hasSps);
    QVERIFY(mixed.hasPps);
    QVERIFY(mixed.hasIdr);

    QByteArray sps, pps;
    QVERIFY(annexb::parameterSets(nal(7, 3, true) + nal(8, 3, true), sps, pps));
    // Normalised to the 4-byte form csd-0/csd-1 take, whatever came in.
    QCOMPARE(sps.left(4), QByteArray::fromHex("00000001"));
    QCOMPARE(pps.left(4), QByteArray::fromHex("00000001"));
    QCOMPARE(uint8_t(sps[4]) & 0x1F, 7u);
    QCOMPARE(uint8_t(pps[4]) & 0x1F, 8u);
}

void TestMobileVideo::annexBTakesTheLastParameterSetPair() {
    QByteArray sps, pps;

    // A half pair configures nothing: the decoder needs both, and
    // starting a codec on one is worse than waiting for the keyframe
    // that carries the other.
    QVERIFY(!annexb::parameterSets(nal(7), sps, pps));
    QVERIFY(!annexb::parameterSets(nal(8), sps, pps));
    QVERIFY(!annexb::parameterSets(nal(5), sps, pps));

    // When a resolution change puts two SPSs in one access unit, the
    // LAST one describes the picture that follows it. Taking the first
    // configures the decoder for the size the stream just left.
    const QByteArray oldSps = nal(7, /*payloadBytes=*/2);
    const QByteArray newSps = nal(7, /*payloadBytes=*/6);
    QVERIFY(annexb::parameterSets(oldSps + nal(8) + newSps + nal(8) + nal(5),
                                  sps, pps));
    QCOMPARE(sps, newSps);
    QVERIFY(sps != oldSps);
}

QTEST_GUILESS_MAIN(TestMobileVideo)
#include "test_mobile_video.moc"
