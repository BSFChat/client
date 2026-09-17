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

#include <memory>
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
    // S-18. These skip cleanly wherever H.265 is unavailable — which is
    // every Linux build and any Windows machine without an HEVC MFT —
    // so the target stays green on all three platforms.
    void h265RoundTrip();
    void h265KeyframeCarriesVpsSpsPps();
    void h265DecoderRefusesAMidGopStart();
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

// ---- S-18: H.265 ----------------------------------------------------
//
// Same three questions as H.264, asked separately because the HEVC
// paths are genuinely different code: a two-byte NAL header, a third
// parameter set (the VPS) that VideoToolbox will not build a session
// without, and a different set of "you may start here" NAL types.

namespace {

// Encoder + decoder pair for H.265, or {nullptr, nullptr} when this
// machine has no H.265. The capability predicates are what production
// gates on, so the test gates on exactly them rather than on whether
// create() happened to succeed.
struct HevcPair {
    std::unique_ptr<VideoEncoder> enc;
    std::unique_ptr<VideoDecoder> dec;
};

HevcPair makeHevcPair() {
    if (!VideoEncoder::h265EncodeSupported()
        || !VideoDecoder::h265DecodeSupported())
        return {};
    HevcPair p;
    p.enc = VideoEncoder::create(VideoCodecKind::H265);
    p.dec = VideoDecoder::create(VideoCodecKind::H265);
    return p;
}

// Walk an Annex-B access unit and collect the HEVC NAL types in it.
// Type lives in bits 1..6 of the FIRST of the two header bytes — the
// single most likely thing to get wrong when porting H.264 code.
QList<int> hevcNalTypes(const QByteArray& au) {
    QList<int> types;
    const auto* p = reinterpret_cast<const uint8_t*>(au.constData());
    const int n = au.size();
    for (int i = 0; i + 3 < n; ++i) {
        if (p[i] != 0 || p[i + 1] != 0) continue;
        int start = -1;
        if (p[i + 2] == 1) start = i + 3;
        else if (p[i + 2] == 0 && i + 3 < n && p[i + 3] == 1) start = i + 4;
        if (start < 0 || start + 1 >= n) continue;
        types.append((p[start] >> 1) & 0x3F);
        i = start - 1;
    }
    return types;
}

} // namespace

void TestVideoCodec::h265RoundTrip() {
    HevcPair hevc = makeHevcPair();
    if (!hevc.enc || !hevc.dec)
        QSKIP("no H.265 encoder/decoder on this machine");

    EncoderConfig cfg;
    cfg.codec = VideoCodecKind::H265;
    cfg.width = 640;
    cfg.height = 360;
    cfg.fps = 30;
    cfg.targetBitrateKbps = 1500;
    cfg.maxBitrateKbps = 3000;
    QVERIFY(hevc.enc->init(cfg));
    QVERIFY(hevc.dec->init(VideoCodecKind::H265));

    int decoded = 0;
    bool firstWasKeyframe = false;
    qint64 totalBytes = 0;
    for (int i = 0; i < 30; ++i) {
        PlanarFrame planar = FrameConverter::toI420(
            makeTestFrame(640, 360, i), 0, i * 33000);
        QVERIFY(planar.isValid());

        EncodedFrame enc;
        if (!hevc.enc->encode(planar, /*forceKeyframe=*/i == 0, enc)) continue;
        QVERIFY(!enc.data.isEmpty());
        QVERIFY(enc.data.size() > 4);
        // Long start code, as the RTP packetizer is configured to expect.
        QVERIFY(enc.data[0] == '\0' && enc.data[1] == '\0'
                && enc.data[2] == '\0' && enc.data[3] == '\1');
        totalBytes += enc.data.size();
        if (decoded == 0 && enc.keyframe) firstWasKeyframe = true;

        QVideoFrame out;
        const auto res = hevc.dec->decode(enc.data, out);
        QVERIFY(res != VideoDecoder::Result::Error);
        if (res == VideoDecoder::Result::Ok) {
            QCOMPARE(out.width(), 640);
            QCOMPARE(out.height(), 360);
            ++decoded;
        }
    }
    QVERIFY2(decoded >= 25, qPrintable(QStringLiteral(
        "expected ≥25 decoded H.265 frames, got %1").arg(decoded)));
    QVERIFY(firstWasKeyframe);
    QVERIFY(totalBytes > 0);
}

void TestVideoCodec::h265KeyframeCarriesVpsSpsPps() {
    HevcPair hevc = makeHevcPair();
    if (!hevc.enc || !hevc.dec)
        QSKIP("no H.265 encoder/decoder on this machine");

    EncoderConfig cfg;
    cfg.codec = VideoCodecKind::H265;
    cfg.width = 320;
    cfg.height = 240;
    cfg.fps = 30;
    QVERIFY(hevc.enc->init(cfg));

    // A receiver joins mid-share, or re-enters after loss, with NO
    // prior state — every keyframe has to be self-contained. For HEVC
    // that means the VPS as well as the SPS and PPS; VideoToolbox
    // refuses to build a decompression session without all three, so
    // dropping the VPS produces a stream that no Mac can start on.
    EncodedFrame idr;
    PlanarFrame planar = FrameConverter::toI420(
        makeTestFrame(320, 240, 0), 0, 0);
    QVERIFY(hevc.enc->encode(planar, /*forceKeyframe=*/true, idr));
    QVERIFY(idr.keyframe);

    const QList<int> types = hevcNalTypes(idr.data);
    QVERIFY2(types.contains(32), "keyframe must carry a VPS (NAL 32)");
    QVERIFY2(types.contains(33), "keyframe must carry an SPS (NAL 33)");
    QVERIFY2(types.contains(34), "keyframe must carry a PPS (NAL 34)");
    // And an actual random-access picture, which is what the receive
    // pipeline's keyframe gate scans for (IRAP = 16..23).
    bool irap = false;
    for (int t : types) irap = irap || (t >= 16 && t <= 23);
    QVERIFY2(irap, "keyframe must carry an IRAP picture (NAL 16..23)");

    // Parameter sets must come BEFORE the picture, or a decoder that
    // stops at the first slice never sees them.
    int firstParamSet = -1, firstIrap = -1;
    for (int i = 0; i < types.size(); ++i) {
        if (firstParamSet < 0 && types[i] >= 32 && types[i] <= 34)
            firstParamSet = i;
        if (firstIrap < 0 && types[i] >= 16 && types[i] <= 23) firstIrap = i;
    }
    QVERIFY(firstParamSet >= 0 && firstIrap > firstParamSet);

    // A P-frame does NOT repeat them — that is the whole reason
    // keyframes have to.
    EncodedFrame delta;
    PlanarFrame next = FrameConverter::toI420(
        makeTestFrame(320, 240, 1), 0, 33000);
    if (hevc.enc->encode(next, /*forceKeyframe=*/false, delta)
        && !delta.keyframe) {
        QVERIFY(!hevcNalTypes(delta.data).contains(33));
    }
}

void TestVideoCodec::h265DecoderRefusesAMidGopStart() {
    HevcPair hevc = makeHevcPair();
    if (!hevc.enc || !hevc.dec)
        QSKIP("no H.265 encoder/decoder on this machine");

    EncoderConfig cfg;
    cfg.codec = VideoCodecKind::H265;
    cfg.width = 320;
    cfg.height = 240;
    cfg.fps = 30;
    QVERIFY(hevc.enc->init(cfg));
    QVERIFY(hevc.dec->init(VideoCodecKind::H265));

    QList<EncodedFrame> frames;
    for (int i = 0; i < 10; ++i) {
        PlanarFrame planar = FrameConverter::toI420(
            makeTestFrame(320, 240, i), 0, i * 33000);
        EncodedFrame enc;
        if (hevc.enc->encode(planar, i == 0, enc)) frames.append(enc);
    }
    QVERIFY(frames.size() >= 8);

    // Same contract as H.264: a decoder handed references it never saw
    // must refuse rather than present error-concealed garbage, because
    // the receive pipeline treats Ok as "display this".
    for (int i = 5; i < frames.size(); ++i) {
        QVideoFrame out;
        QVERIFY2(hevc.dec->decode(frames[i].data, out)
                     != VideoDecoder::Result::Ok,
                 "mid-GOP H.265 without parameter sets must not decode as Ok");
    }

    hevc.dec->reset();
    EncodedFrame idr;
    PlanarFrame planar = FrameConverter::toI420(
        makeTestFrame(320, 240, 99), 0, 0);
    QVERIFY(hevc.enc->encode(planar, /*forceKeyframe=*/true, idr));
    QVERIFY(idr.keyframe);
    QVideoFrame out;
    QCOMPARE(int(hevc.dec->decode(idr.data, out)),
             int(VideoDecoder::Result::Ok));
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
