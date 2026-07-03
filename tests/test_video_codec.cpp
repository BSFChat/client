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
    // IDR + early references) — it must not emit garbage as Ok
    // forever; after reset + a fresh keyframe it must recover.
    QList<EncodedFrame> frames;
    for (int i = 0; i < 10; ++i) {
        PlanarFrame planar = FrameConverter::toI420(
            makeTestFrame(320, 240, i), 0, i * 33000);
        EncodedFrame enc;
        if (encoder->encode(planar, i == 0, enc)) frames.append(enc);
    }
    QVERIFY(frames.size() >= 8);

    bool sawError = false;
    for (int i = 5; i < frames.size(); ++i) {
        QVideoFrame out;
        if (decoder->decode(frames[i].data, out) == VideoDecoder::Result::Error)
            sawError = true;
    }
    QVERIFY2(sawError, "decoding mid-GOP without references should error");

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

QTEST_GUILESS_MAIN(TestVideoCodec)
#include "test_video_codec.moc"
