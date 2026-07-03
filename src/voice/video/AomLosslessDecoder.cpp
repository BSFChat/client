#include "voice/video/AomLosslessDecoder.h"

#include <QLoggingCategory>
#include <QVideoFrameFormat>

#include <aom/aom_decoder.h>
#include <aom/aomdx.h>

Q_LOGGING_CATEGORY(logAomDec, "bsfchat.video.aom", QtWarningMsg)

struct AomDecoderState {
    aom_codec_ctx_t ctx{};
    bool initialized = false;
};

AomLosslessDecoder::AomLosslessDecoder()
    : m_state(std::make_unique<AomDecoderState>()) {}

AomLosslessDecoder::~AomLosslessDecoder() {
    if (m_state->initialized) aom_codec_destroy(&m_state->ctx);
}

bool AomLosslessDecoder::init(VideoCodecKind kind) {
    if (m_state->initialized) {
        aom_codec_destroy(&m_state->ctx);
        m_state->initialized = false;
    }
    if (kind != VideoCodecKind::Av1Lossless) return false;
    aom_codec_dec_cfg_t cfg{};
    cfg.threads = 4;
    cfg.allow_lowbitdepth = 1;
    if (aom_codec_dec_init(&m_state->ctx, aom_codec_av1_dx(), &cfg, 0)
        != AOM_CODEC_OK)
        return false;
    m_state->initialized = true;
    return true;
}

VideoDecoder::Result AomLosslessDecoder::decode(const QByteArray& tu,
                                                QVideoFrame& out) {
    if (!m_state->initialized || tu.isEmpty()) return Result::Error;

    if (aom_codec_decode(&m_state->ctx,
                         reinterpret_cast<const uint8_t*>(tu.constData()),
                         size_t(tu.size()), nullptr) != AOM_CODEC_OK) {
        qCWarning(logAomDec, "aom decode failed: %s",
                 aom_codec_error(&m_state->ctx));
        return Result::Error;
    }

    aom_codec_iter_t iter = nullptr;
    aom_image_t* img = aom_codec_get_frame(&m_state->ctx, &iter);
    if (!img) return Result::NeedMore;
    if (img->fmt != AOM_IMG_FMT_I444) {
        qCWarning(logAomDec, "unexpected aom image format %d", int(img->fmt));
        return Result::Error;
    }

    const int w = int(img->d_w);
    const int h = int(img->d_h);
    // Identity mapping back to RGB: Y-plane carries G, U carries B,
    // V carries R (CICP identity-matrix convention). Emit BGRA.
    QVideoFrameFormat fmt(QSize(w, h), QVideoFrameFormat::Format_BGRA8888);
    QVideoFrame frame(fmt);
    if (!frame.map(QVideoFrame::WriteOnly)) return Result::Error;
    const uint8_t* gPlane = img->planes[AOM_PLANE_Y];
    const uint8_t* bPlane = img->planes[AOM_PLANE_U];
    const uint8_t* rPlane = img->planes[AOM_PLANE_V];
    const int gStride = img->stride[AOM_PLANE_Y];
    const int bStride = img->stride[AOM_PLANE_U];
    const int rStride = img->stride[AOM_PLANE_V];
    uchar* dst = frame.bits(0);
    const int dstStride = frame.bytesPerLine(0);
    for (int row = 0; row < h; ++row) {
        uchar* d = dst + row * dstStride;
        const uint8_t* g = gPlane + row * gStride;
        const uint8_t* b = bPlane + row * bStride;
        const uint8_t* r = rPlane + row * rStride;
        for (int x = 0; x < w; ++x) {
            d[4 * x + 0] = b[x];
            d[4 * x + 1] = g[x];
            d[4 * x + 2] = r[x];
            d[4 * x + 3] = 0xFF;
        }
    }
    frame.unmap();
    out = frame;
    return Result::Ok;
}

void AomLosslessDecoder::reset() {
    init(VideoCodecKind::Av1Lossless);
}
