#include "voice/video/FrameConverter.h"

#include "voice/video/VideoFrameOrientation.h"

#include <QImage>
#include <QLoggingCategory>
#include <QVideoFrame>

#include <libyuv.h>

Q_LOGGING_CATEGORY(logVideoConv, "bsfchat.video.convert", QtWarningMsg)

namespace {

// Allocate the I420 plane buffers for even w×h.
PlanarFrame makeI420(int w, int h, qint64 tsUs) {
    PlanarFrame f;
    f.layout = PlanarFrame::Layout::I420;
    f.width = w;
    f.height = h;
    f.strideY = w;
    f.strideU = w / 2;
    f.strideV = w / 2;
    f.y.resize(w * h);
    f.u.resize((w / 2) * (h / 2));
    f.v.resize((w / 2) * (h / 2));
    f.captureTimeUs = tsUs;
    return f;
}

// libyuv's "ARGB" means B,G,R,A byte order in memory (little-endian
// packed 32-bit), which is what Qt's BGRA8888 and (LE) ARGB32 store.
// Qt's RGBA8888 (R,G,B,A in memory) is libyuv's "ABGR".
enum class Packed { LibyuvARGB, LibyuvABGR, Unsupported };

Packed packedKind(QVideoFrameFormat::PixelFormat fmt) {
    switch (fmt) {
    case QVideoFrameFormat::Format_BGRA8888:
    case QVideoFrameFormat::Format_BGRX8888:
    case QVideoFrameFormat::Format_ARGB8888_Premultiplied:
    case QVideoFrameFormat::Format_ARGB8888:
    case QVideoFrameFormat::Format_XRGB8888:
        return Packed::LibyuvARGB;
    case QVideoFrameFormat::Format_RGBA8888:
    case QVideoFrameFormat::Format_RGBX8888:
    case QVideoFrameFormat::Format_ABGR8888:
    case QVideoFrameFormat::Format_XBGR8888:
        return Packed::LibyuvABGR;
    default:
        return Packed::Unsupported;
    }
}

// Full-size I420 from whatever the capture backend produced. Returns
// invalid frame if the format isn't handled directly (caller falls
// back to the QImage path).
PlanarFrame directToI420(const QVideoFrame& mapped, qint64 tsUs) {
    const auto pixFmt = mapped.pixelFormat();
    // Even-dimension guard: chop a single row/column if needed.
    const int w = mapped.width() & ~1;
    const int h = mapped.height() & ~1;
    if (w < 16 || h < 16) return {};

    if (pixFmt == QVideoFrameFormat::Format_NV12) {
        PlanarFrame out = makeI420(w, h, tsUs);
        libyuv::NV12ToI420(
            mapped.bits(0), mapped.bytesPerLine(0),
            mapped.bits(1), mapped.bytesPerLine(1),
            reinterpret_cast<uint8_t*>(out.y.data()), out.strideY,
            reinterpret_cast<uint8_t*>(out.u.data()), out.strideU,
            reinterpret_cast<uint8_t*>(out.v.data()), out.strideV,
            w, h);
        return out;
    }
    if (pixFmt == QVideoFrameFormat::Format_YUV420P) {
        PlanarFrame out = makeI420(w, h, tsUs);
        libyuv::I420Copy(
            mapped.bits(0), mapped.bytesPerLine(0),
            mapped.bits(1), mapped.bytesPerLine(1),
            mapped.bits(2), mapped.bytesPerLine(2),
            reinterpret_cast<uint8_t*>(out.y.data()), out.strideY,
            reinterpret_cast<uint8_t*>(out.u.data()), out.strideU,
            reinterpret_cast<uint8_t*>(out.v.data()), out.strideV,
            w, h);
        return out;
    }

    switch (packedKind(pixFmt)) {
    case Packed::LibyuvARGB: {
        PlanarFrame out = makeI420(w, h, tsUs);
        libyuv::ARGBToI420(
            mapped.bits(0), mapped.bytesPerLine(0),
            reinterpret_cast<uint8_t*>(out.y.data()), out.strideY,
            reinterpret_cast<uint8_t*>(out.u.data()), out.strideU,
            reinterpret_cast<uint8_t*>(out.v.data()), out.strideV,
            w, h);
        return out;
    }
    case Packed::LibyuvABGR: {
        PlanarFrame out = makeI420(w, h, tsUs);
        libyuv::ABGRToI420(
            mapped.bits(0), mapped.bytesPerLine(0),
            reinterpret_cast<uint8_t*>(out.y.data()), out.strideY,
            reinterpret_cast<uint8_t*>(out.u.data()), out.strideU,
            reinterpret_cast<uint8_t*>(out.v.data()), out.strideV,
            w, h);
        return out;
    }
    case Packed::Unsupported:
        return {};
    }
    return {};
}

// Rotate an I420 frame clockwise by 90/180/270. 0 returns the input
// untouched (by value — the QByteArray planes are implicitly shared, so
// the no-op case costs a refcount, not a copy, which is what keeps this
// free on the desktop screen-share path where the angle is always 0).
PlanarFrame rotateI420(const PlanarFrame& in, int deg) {
    const int d = videoorient::normalise(deg);
    if (d == 0) return in;
    const bool swap = videoorient::swapsAxes(d);
    const int dstW = swap ? in.height : in.width;
    const int dstH = swap ? in.width : in.height;
    PlanarFrame out = makeI420(dstW, dstH, in.captureTimeUs);
    const libyuv::RotationMode mode = d == 90  ? libyuv::kRotate90
                                    : d == 180 ? libyuv::kRotate180
                                               : libyuv::kRotate270;
    // I420Rotate's w/h are the SOURCE dimensions; it writes the rotated
    // result into the destination planes, whose strides already describe
    // the swapped geometry.
    if (libyuv::I420Rotate(
            reinterpret_cast<const uint8_t*>(in.y.constData()), in.strideY,
            reinterpret_cast<const uint8_t*>(in.u.constData()), in.strideU,
            reinterpret_cast<const uint8_t*>(in.v.constData()), in.strideV,
            reinterpret_cast<uint8_t*>(out.y.data()), out.strideY,
            reinterpret_cast<uint8_t*>(out.u.data()), out.strideU,
            reinterpret_cast<uint8_t*>(out.v.data()), out.strideV,
            in.width, in.height, mode) != 0) {
        return in;   // refuse to lose the frame over a rotation failure
    }
    return out;
}

PlanarFrame scaleI420(const PlanarFrame& in, int dstW, int dstH) {
    PlanarFrame out = makeI420(dstW, dstH, in.captureTimeUs);
    libyuv::I420Scale(
        reinterpret_cast<const uint8_t*>(in.y.constData()), in.strideY,
        reinterpret_cast<const uint8_t*>(in.u.constData()), in.strideU,
        reinterpret_cast<const uint8_t*>(in.v.constData()), in.strideV,
        in.width, in.height,
        reinterpret_cast<uint8_t*>(out.y.data()), out.strideY,
        reinterpret_cast<uint8_t*>(out.u.data()), out.strideU,
        reinterpret_cast<uint8_t*>(out.v.data()), out.strideV,
        dstW, dstH, libyuv::kFilterBilinear);
    return out;
}

} // namespace

namespace FrameConverter {

PlanarFrame toI420(const QVideoFrame& in, int maxLongEdge, qint64 captureTimeUs) {
    if (!in.isValid()) return {};

    PlanarFrame full;
    QVideoFrame frame(in);   // shallow copy so we can map const input
    if (frame.map(QVideoFrame::ReadOnly)) {
        full = directToI420(frame, captureTimeUs);
        frame.unmap();
    }
    if (!full.isValid()) {
        // GPU-backed or exotic-format frame: toImage() performs the
        // readback/conversion for us at the cost of an extra copy.
        QImage img = in.toImage();
        if (img.isNull()) {
            qCWarning(logVideoConv, "unconvertible frame (pixelFormat=%d)",
                     int(in.pixelFormat()));
            return {};
        }
        if (img.format() != QImage::Format_ARGB32
            && img.format() != QImage::Format_ARGB32_Premultiplied
            && img.format() != QImage::Format_RGB32) {
            img = img.convertToFormat(QImage::Format_ARGB32);
        }
        const int w = img.width() & ~1;
        const int h = img.height() & ~1;
        if (w < 16 || h < 16) return {};
        full = makeI420(w, h, captureTimeUs);
        libyuv::ARGBToI420(
            img.constBits(), int(img.bytesPerLine()),
            reinterpret_cast<uint8_t*>(full.y.data()), full.strideY,
            reinterpret_cast<uint8_t*>(full.u.data()), full.strideU,
            reinterpret_cast<uint8_t*>(full.v.data()), full.strideV,
            w, h);
    }

    // Scale BEFORE rotating, not after. A 90° turn leaves the long edge
    // unchanged, so both orders land on the same output size — but
    // rotating the already-downscaled frame touches a fraction of the
    // pixels. On a 4032-wide phone capture headed for a 640 encode that
    // is the difference between rotating 12 MB and rotating 0.3 MB,
    // every frame, on the encode worker.
    const int longEdge = qMax(full.width, full.height);
    if (maxLongEdge > 0 && longEdge > maxLongEdge) {
        const double scale = double(maxLongEdge) / double(longEdge);
        const int dstW = qMax(16, int(full.width * scale)) & ~1;
        const int dstH = qMax(16, int(full.height * scale)) & ~1;
        full = scaleI420(full, dstW, dstH);
    }

    // Bake the capture orientation into the pixels. See
    // VideoFrameOrientation.h for why this is the sender's job and not
    // the receiver's, and why the mirror flag is deliberately not
    // applied. Zero on every desktop source, so this is a refcount on
    // the screen-share path.
    return rotateI420(full,
        videoorient::wireRotation(videoorient::presentationRotation(in)));
}

PlanarFrame toI444Identity(const QVideoFrame& in, qint64 captureTimeUs) {
    if (!in.isValid()) return {};
    // Go through QImage/ARGB32 unconditionally: this path exists for
    // bit-exactness, so a single well-defined RGB source beats a fast
    // path per capture format. (For camera sources that arrive as
    // 4:2:0 YUV, "lossless" means lossless relative to this RGB
    // conversion — the subsampling already happened at capture.)
    QImage img = in.toImage();
    if (img.isNull()) return {};
    if (img.format() != QImage::Format_ARGB32
        && img.format() != QImage::Format_RGB32) {
        img = img.convertToFormat(QImage::Format_ARGB32);
    }
    const int w = img.width() & ~1;
    const int h = img.height() & ~1;
    if (w < 16 || h < 16) return {};

    PlanarFrame f;
    f.layout = PlanarFrame::Layout::I444Identity;
    f.width = w;
    f.height = h;
    f.strideY = f.strideU = f.strideV = w;
    f.y.resize(w * h);
    f.u.resize(w * h);
    f.v.resize(w * h);
    f.captureTimeUs = captureTimeUs;

    // Identity-matrix CICP plane mapping: Y←G, U←B, V←R.
    auto* g = reinterpret_cast<uint8_t*>(f.y.data());
    auto* b = reinterpret_cast<uint8_t*>(f.u.data());
    auto* r = reinterpret_cast<uint8_t*>(f.v.data());
    for (int row = 0; row < h; ++row) {
        const auto* src = img.constScanLine(row);   // B,G,R,A per pixel (LE)
        for (int x = 0; x < w; ++x) {
            b[row * w + x] = src[4 * x + 0];
            g[row * w + x] = src[4 * x + 1];
            r[row * w + x] = src[4 * x + 2];
        }
    }
    return f;
}

} // namespace FrameConverter
