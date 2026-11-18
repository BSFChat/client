#pragma once

// Wrap a packed RGB QImage (what the macOS capturers deliver) in a
// QVideoFrame for the preview sink and the send pipeline.
//
// The copy MUST go row by row. A QVideoFrame built from a format rounds
// every line up to a multiple of 16 bytes, while a QImage copy is packed
// at width * 4. The two only agree when the width is a multiple of four
// pixels; for any other width (a freely resized window, say) a single
// whole-image memcpy lands each row a few bytes later than the last and
// the picture comes out sheared diagonally for every receiver.

#include <QImage>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include <algorithm>
#include <cstring>

inline QVideoFrame imageToVideoFrame(const QImage& img) {
    if (img.isNull()) return {};
    const QVideoFrameFormat::PixelFormat pf =
        QVideoFrameFormat::pixelFormatFromImageFormat(img.format());
    if (pf == QVideoFrameFormat::Format_Invalid) return {};
    QVideoFrame vf(QVideoFrameFormat(img.size(), pf));
    if (!vf.map(QVideoFrame::WriteOnly)) return {};
    const qsizetype srcBpl = img.bytesPerLine();
    const qsizetype dstBpl = vf.bytesPerLine(0);
    const qsizetype rowBytes = std::min(srcBpl, dstBpl);
    const uchar* src = img.constBits();
    uchar* dst = vf.bits(0);
    for (int y = 0; y < img.height(); ++y)
        std::memcpy(dst + y * dstBpl, src + y * srcBpl, size_t(rowBytes));
    vf.unmap();
    return vf;
}
