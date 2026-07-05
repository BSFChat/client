#include "voice/video/OpenH264Decoder.h"

#include <QLoggingCategory>
#include <QVideoFrameFormat>

#include <wels/codec_api.h>

#include <cstring>

Q_LOGGING_CATEGORY(logVideoDec, "bsfchat.video.decode", QtWarningMsg)

OpenH264Decoder::~OpenH264Decoder() {
    destroy();
}

void OpenH264Decoder::destroy() {
    if (m_decoder) {
        m_decoder->Uninitialize();
        WelsDestroyDecoder(m_decoder);
        m_decoder = nullptr;
    }
}

bool OpenH264Decoder::init(VideoCodecKind kind) {
    destroy();
    if (kind != VideoCodecKind::H264) return false;

    if (WelsCreateDecoder(&m_decoder) != 0 || !m_decoder) {
        qCWarning(logVideoDec, "openh264: WelsCreateDecoder failed");
        m_decoder = nullptr;
        return false;
    }
    SDecodingParam param;
    std::memset(&param, 0, sizeof(param));
    param.sVideoProperty.size = sizeof(param.sVideoProperty);
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    // No error concealment: a damaged frame must FAIL, not smear —
    // the pipeline reacts by flushing to the next keyframe and asking
    // the sender for one (PLI), which recovers faster and cleaner than
    // concealed artifacts.
    param.eEcActiveIdc = ERROR_CON_DISABLE;
    if (m_decoder->Initialize(&param) != 0) {
        qCWarning(logVideoDec, "openh264: decoder Initialize failed");
        destroy();
        return false;
    }
    return true;
}

VideoDecoder::Result OpenH264Decoder::decode(const QByteArray& au, QVideoFrame& out) {
    if (!m_decoder || au.isEmpty()) return Result::Error;

    unsigned char* dst[3] = {nullptr, nullptr, nullptr};
    SBufferInfo bufInfo;
    std::memset(&bufInfo, 0, sizeof(bufInfo));

    const DECODING_STATE state = m_decoder->DecodeFrameNoDelay(
        reinterpret_cast<const unsigned char*>(au.constData()),
        au.size(), dst, &bufInfo);
    if (state != dsErrorFree) {
        qCWarning(logVideoDec, "openh264: decode error 0x%x", int(state));
        return Result::Error;
    }
    if (bufInfo.iBufferStatus != 1 || !dst[0]) {
        // Consumed (SPS/PPS-only AU, or decoder-internal buffering).
        return Result::NeedMore;
    }

    const int w = bufInfo.UsrData.sSystemBuffer.iWidth;
    const int h = bufInfo.UsrData.sSystemBuffer.iHeight;
    const int strideY = bufInfo.UsrData.sSystemBuffer.iStride[0];
    const int strideUV = bufInfo.UsrData.sSystemBuffer.iStride[1];

    QVideoFrameFormat fmt(QSize(w, h), QVideoFrameFormat::Format_YUV420P);
    QVideoFrame frame(fmt);
    if (!frame.map(QVideoFrame::WriteOnly)) return Result::Error;
    // Row-wise copies — decoder strides rarely match QVideoFrame's.
    for (int plane = 0; plane < 3; ++plane) {
        const int planeH = plane == 0 ? h : h / 2;
        const int planeW = plane == 0 ? w : w / 2;
        const int srcStride = plane == 0 ? strideY : strideUV;
        uchar* dstBits = frame.bits(plane);
        const int dstStride = frame.bytesPerLine(plane);
        for (int row = 0; row < planeH; ++row) {
            std::memcpy(dstBits + row * dstStride,
                        dst[plane] + row * srcStride, size_t(planeW));
        }
    }
    frame.unmap();
    out = frame;
    return Result::Ok;
}

void OpenH264Decoder::reset() {
    // Full rebuild — cheapest reliable way to drop reference state.
    init(VideoCodecKind::H264);
}
