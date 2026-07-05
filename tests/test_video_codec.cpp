// Round-trip tests for the video codec layer: FrameConverter →
// VideoEncoder (openh264) → VideoDecoder. Catches backend API misuse
// (bad strides, wrong Annex-B framing, profile mismatches) without
// needing two live clients.
#include <QtTest/QtTest>
#include <QImage>
#include <QPainter>
#include <QVideoFrame>

#include "voice/video/FrameConverter.h"
#include "voice/video/VideoDecoder.h"
#include "voice/video/VideoEncoder.h"
#ifdef BSFCHAT_HAVE_VIDEOTOOLBOX
#include "voice/video/MacVTEncoder.h"
#include "voice/video/MacVTDecoder.h"
#include "voice/video/OpenH264Encoder.h"
#include "voice/video/OpenH264Decoder.h"
#endif

namespace {

// Synthetic screen-ish content: colored blocks + text-like stripes,
// varied per frame index so P-frames have real motion to code.
QVideoFrame makeTestFrame(int w, int h, int index) {
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(QColor(30, 32, 38));
    QPainter p(&img);
    p.fillRect(20 + index * 7, 40, 200, 120, QColor(200, 60, 60));
    p.fillRect(300, 200 + index * 5, 240, 100, QColor(60, 200, 120));
    for (int y = 0; y < h; y += 14)
        p.fillRect(0, y, w, 2, QColor(220, 220, 225));
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
    return frame;
}

} // namespace

class TestVideoCodec : public QObject {
    Q_OBJECT
private slots:
    void frameConverterProducesI420();
    void frameConverterScalesToLongEdge();
    void h264RoundTrip();
    void h264DecoderRecoversAtKeyframe();
#ifdef BSFCHAT_HAVE_VIDEOTOOLBOX
    // Cross-backend interop: the real mac ↔ linux stream matrix.
    void vtEncodesOpenh264Decodes();
    void openh264EncodesVtDecodes();
#endif
#ifdef BSFCHAT_HAVE_AOM
    // The lossless tier's entire contract: RGB in == RGB out, bit for
    // bit, across encode → decode.
    void av1LosslessIsBitExact();
#endif
};

void TestVideoCodec::frameConverterProducesI420() {
    QVideoFrame in = makeTestFrame(640, 360, 0);
    QVERIFY(in.isValid());
    PlanarFrame f = FrameConverter::toI420(in, 0, 1000);
    QVERIFY(f.isValid());
    QCOMPARE(f.width, 640);
    QCOMPARE(f.height, 360);
    QCOMPARE(f.layout, PlanarFrame::Layout::I420);
    QCOMPARE(f.y.size(), 640 * 360);
    QCOMPARE(f.u.size(), 320 * 180);
    QCOMPARE(f.captureTimeUs, qint64(1000));
}

void TestVideoCodec::frameConverterScalesToLongEdge() {
    QVideoFrame in = makeTestFrame(1920, 1080, 0);
    PlanarFrame f = FrameConverter::toI420(in, 960, 0);
    QVERIFY(f.isValid());
    QCOMPARE(f.width, 960);
    QCOMPARE(f.height, 540);
}

void TestVideoCodec::h264RoundTrip() {
    auto encoder = VideoEncoder::create(VideoCodecKind::H264);
    if (!encoder) QSKIP("no H.264 encoder backend on this platform");

    EncoderConfig cfg;
    cfg.width = 640;
    cfg.height = 360;
    cfg.fps = 30;
    cfg.targetBitrateKbps = 1500;
    cfg.maxBitrateKbps = 3000;
    QVERIFY(encoder->init(cfg));

    auto decoder = VideoDecoder::create(VideoCodecKind::H264);
    QVERIFY(decoder != nullptr);
    QVERIFY(decoder->init(VideoCodecKind::H264));

    int decoded = 0;
    bool firstWasKeyframe = false;
    for (int i = 0; i < 30; ++i) {
        PlanarFrame planar = FrameConverter::toI420(
            makeTestFrame(640, 360, i), 0, i * 33000);
        QVERIFY(planar.isValid());

        EncodedFrame enc;
        if (!encoder->encode(planar, /*forceKeyframe=*/i == 0, enc)) continue;
        QVERIFY(!enc.data.isEmpty());
        // Annex-B start code present?
        QVERIFY(enc.data.size() > 4);
        QVERIFY(enc.data[0] == '\0' && enc.data[1] == '\0');
        if (decoded == 0 && enc.keyframe) firstWasKeyframe = true;

        QVideoFrame out;
        const auto res = decoder->decode(enc.data, out);
        QVERIFY(res != VideoDecoder::Result::Error);
        if (res == VideoDecoder::Result::Ok) {
            QCOMPARE(out.width(), 640);
            QCOMPARE(out.height(), 360);
            ++decoded;
        }
    }
    QVERIFY2(decoded >= 25, qPrintable(QStringLiteral(
        "expected ≥25 decoded frames, got %1").arg(decoded)));
    QVERIFY(firstWasKeyframe);
}

void TestVideoCodec::h264DecoderRecoversAtKeyframe() {
    auto encoder = VideoEncoder::create(VideoCodecKind::H264);
    if (!encoder) QSKIP("no H.264 encoder backend on this platform");
    EncoderConfig cfg;
    cfg.width = 320;
    cfg.height = 240;
    cfg.fps = 30;
    QVERIFY(encoder->init(cfg));

    auto decoder = VideoDecoder::create(VideoCodecKind::H264);
    QVERIFY(decoder->init(VideoCodecKind::H264));

    // Encode a GOP, feed the decoder ONLY frames 5.. (missing the
    // IDR + early references) — it must never present garbage as a
    // displayable frame. Backends differ in HOW they refuse (openh264
    // errors with concealment disabled; VideoToolbox reports NeedMore
    // while it lacks parameter sets) — both are acceptable, Ok is not.
    QList<EncodedFrame> frames;
    for (int i = 0; i < 10; ++i) {
        PlanarFrame planar = FrameConverter::toI420(
            makeTestFrame(320, 240, i), 0, i * 33000);
        EncodedFrame enc;
        if (encoder->encode(planar, i == 0, enc)) frames.append(enc);
    }
    QVERIFY(frames.size() >= 8);

    for (int i = 5; i < frames.size(); ++i) {
        QVideoFrame out;
        QVERIFY2(decoder->decode(frames[i].data, out) != VideoDecoder::Result::Ok,
                 "mid-GOP data without references must not decode as Ok");
    }

    // Recovery: reset, then feed a fresh keyframe.
    decoder->reset();
    PlanarFrame planar = FrameConverter::toI420(
        makeTestFrame(320, 240, 99), 0, 0);
    EncodedFrame idr;
    QVERIFY(encoder->encode(planar, /*forceKeyframe=*/true, idr));
    QVERIFY(idr.keyframe);
    QVideoFrame out;
    QCOMPARE(int(decoder->decode(idr.data, out)), int(VideoDecoder::Result::Ok));
}

#ifdef BSFCHAT_HAVE_VIDEOTOOLBOX
namespace {

// Shared harness: encode 20 frames with `enc`, decode with `dec`,
// require ≥60% displayable output and matching dimensions.
void crossDecode(VideoEncoder& enc, VideoDecoder& dec, H264Profile profile) {
    EncoderConfig cfg;
    cfg.width = 640;
    cfg.height = 360;
    cfg.fps = 30;
    cfg.targetBitrateKbps = 2000;
    cfg.maxBitrateKbps = 4000;
    cfg.profile = profile;
    QVERIFY(enc.init(cfg));
    QVERIFY(dec.init(VideoCodecKind::H264));

    int decoded = 0, encoded = 0;
    for (int i = 0; i < 20; ++i) {
        PlanarFrame planar = FrameConverter::toI420(
            makeTestFrame(640, 360, i), 0, i * 33000);
        EncodedFrame out;
        if (!enc.encode(planar, i == 0, out)) continue;
        ++encoded;
        QVideoFrame frame;
        if (dec.decode(out.data, frame) == VideoDecoder::Result::Ok) {
            QCOMPARE(frame.width(), 640);
            QCOMPARE(frame.height(), 360);
            ++decoded;
        }
    }
    QVERIFY2(encoded >= 15, qPrintable(QStringLiteral("encoded %1").arg(encoded)));
    QVERIFY2(decoded >= encoded * 3 / 5,
             qPrintable(QStringLiteral("decoded %1 of %2").arg(decoded).arg(encoded)));
}

} // namespace

void TestVideoCodec::vtEncodesOpenh264Decodes() {
    MacVTEncoder enc;
    OpenH264Decoder dec;
    // High profile: exactly what a mac sender emits toward a Linux
    // receiver after the caps intersection.
    crossDecode(enc, dec, H264Profile::High);
}

void TestVideoCodec::openh264EncodesVtDecodes() {
    OpenH264Encoder enc;
    MacVTDecoder dec;
    crossDecode(enc, dec, H264Profile::ConstrainedBaseline);
}
#endif

#ifdef BSFCHAT_HAVE_AOM
void TestVideoCodec::av1LosslessIsBitExact() {
    auto encoder = VideoEncoder::create(VideoCodecKind::Av1Lossless);
    QVERIFY(encoder != nullptr);
    QVERIFY(encoder->caps().losslessSupported);
    auto decoder = VideoDecoder::create(VideoCodecKind::Av1Lossless);
    QVERIFY(decoder != nullptr);
    QVERIFY(decoder->init(VideoCodecKind::Av1Lossless));

    EncoderConfig cfg;
    cfg.codec = VideoCodecKind::Av1Lossless;
    cfg.lossless = true;
    cfg.width = 320;
    cfg.height = 240;
    cfg.fps = 10;
    QVERIFY(encoder->init(cfg));

    for (int i = 0; i < 5; ++i) {
        QVideoFrame src = makeTestFrame(320, 240, i);
        PlanarFrame planar = FrameConverter::toI444Identity(src, i * 100000);
        QVERIFY(planar.isValid());
        QCOMPARE(planar.layout, PlanarFrame::Layout::I444Identity);

        EncodedFrame enc;
        QVERIFY(encoder->encode(planar, i == 0, enc));
        QVideoFrame out;
        QCOMPARE(int(decoder->decode(enc.data, out)),
                 int(VideoDecoder::Result::Ok));
        QCOMPARE(out.width(), 320);
        QCOMPARE(out.height(), 240);

        // Bit-exactness: decoded BGRA must equal the source pixels.
        QImage decoded = out.toImage().convertToFormat(QImage::Format_ARGB32);
        QImage original = src.toImage().convertToFormat(QImage::Format_ARGB32);
        QVERIFY(!decoded.isNull() && !original.isNull());
        for (int y = 0; y < 240; ++y) {
            const auto* d = reinterpret_cast<const quint32*>(decoded.constScanLine(y));
            const auto* o = reinterpret_cast<const quint32*>(original.constScanLine(y));
            for (int x = 0; x < 320; ++x) {
                if ((d[x] | 0xFF000000u) != (o[x] | 0xFF000000u)) {
                    QFAIL(qPrintable(QStringLiteral(
                        "pixel mismatch frame %1 at (%2,%3): %4 != %5")
                        .arg(i).arg(x).arg(y)
                        .arg(d[x], 8, 16).arg(o[x], 8, 16)));
                }
            }
        }
    }
}
#endif

QTEST_GUILESS_MAIN(TestVideoCodec)
#include "test_video_codec.moc"
