#include "voice/video/AomLosslessEncoder.h"

#include <QLoggingCategory>

#include <aom/aom_encoder.h>
#include <aom/aomcx.h>

#include <cstring>

Q_LOGGING_CATEGORY(logAomEnc, "bsfchat.video.aom", QtWarningMsg)

struct AomEncoderState {
    aom_codec_ctx_t ctx{};
    bool initialized = false;
    qint64 frameIndex = 0;
};

AomLosslessEncoder::AomLosslessEncoder()
    : m_state(std::make_unique<AomEncoderState>()) {}

AomLosslessEncoder::~AomLosslessEncoder() {
    if (m_state->initialized) aom_codec_destroy(&m_state->ctx);
}

bool AomLosslessEncoder::init(const EncoderConfig& config) {
    if (m_state->initialized) {
        aom_codec_destroy(&m_state->ctx);
        m_state->initialized = false;
    }
    if (config.codec != VideoCodecKind::Av1Lossless) return false;

    aom_codec_enc_cfg_t cfg;
    if (aom_codec_enc_config_default(aom_codec_av1_cx(), &cfg,
                                     AOM_USAGE_REALTIME) != AOM_CODEC_OK)
        return false;
    cfg.g_w = unsigned(config.width);
    cfg.g_h = unsigned(config.height);
    cfg.g_timebase = {1, 1000000};   // µs
    // Profile 1 carries 4:4:4 8-bit — required for identity-matrix RGB.
    cfg.g_profile = 1;
    cfg.g_lag_in_frames = 0;         // zero-latency
    cfg.g_threads = 4;
    cfg.rc_end_usage = AOM_Q;        // quantizer mode; lossless pins q=0
    cfg.kf_max_dist = unsigned(qMax(1, config.fps * config.keyframeIntervalSec));
    cfg.kf_mode = AOM_KF_AUTO;

    if (aom_codec_enc_init(&m_state->ctx, aom_codec_av1_cx(), &cfg, 0)
        != AOM_CODEC_OK) {
        qCWarning(logAomEnc, "aom encoder init failed: %s",
                 aom_codec_error(&m_state->ctx));
        return false;
    }
    m_state->initialized = true;

    aom_codec_control(&m_state->ctx, AV1E_SET_LOSSLESS, 1);
    // Fastest realtime speed step — lossless coding tools barely
    // benefit from slower presets, and smoothness is the contract.
    aom_codec_control(&m_state->ctx, AOME_SET_CPUUSED, 9);
    aom_codec_control(&m_state->ctx, AV1E_SET_MATRIX_COEFFICIENTS,
                      AOM_CICP_MC_IDENTITY);
    aom_codec_control(&m_state->ctx, AV1E_SET_COLOR_RANGE, AOM_CR_FULL_RANGE);
    aom_codec_control(&m_state->ctx, AV1E_SET_ENABLE_CDEF, 0);

    m_state->frameIndex = 0;
    m_config = config;
    qCInfo(logAomEnc, "AV1 lossless encoder up: %dx%d@%dfps",
          config.width, config.height, config.fps);
    return true;
}

bool AomLosslessEncoder::encode(const PlanarFrame& in, bool forceKeyframe,
                                EncodedFrame& out) {
    if (!m_state->initialized || !in.isValid()
        || in.layout != PlanarFrame::Layout::I444Identity)
        return false;
    if (in.width != m_config.width || in.height != m_config.height)
        return false;

    aom_image_t img;
    if (!aom_img_wrap(&img, AOM_IMG_FMT_I444, unsigned(in.width),
                      unsigned(in.height), 1,
                      // aom_img_wrap wants one buffer; we pass planes
                      // explicitly below instead.
                      const_cast<unsigned char*>(
                          reinterpret_cast<const unsigned char*>(in.y.constData()))))
        return false;
    img.planes[AOM_PLANE_Y] = const_cast<unsigned char*>(
        reinterpret_cast<const unsigned char*>(in.y.constData()));
    img.planes[AOM_PLANE_U] = const_cast<unsigned char*>(
        reinterpret_cast<const unsigned char*>(in.u.constData()));
    img.planes[AOM_PLANE_V] = const_cast<unsigned char*>(
        reinterpret_cast<const unsigned char*>(in.v.constData()));
    img.stride[AOM_PLANE_Y] = in.strideY;
    img.stride[AOM_PLANE_U] = in.strideU;
    img.stride[AOM_PLANE_V] = in.strideV;
    img.range = AOM_CR_FULL_RANGE;
    img.mc = AOM_CICP_MC_IDENTITY;

    const aom_enc_frame_flags_t flags = forceKeyframe ? AOM_EFLAG_FORCE_KF : 0;
    if (aom_codec_encode(&m_state->ctx, &img, m_state->frameIndex++, 1, flags)
        != AOM_CODEC_OK) {
        qCWarning(logAomEnc, "aom encode failed: %s",
                 aom_codec_error(&m_state->ctx));
        return false;
    }

    out.data.clear();
    aom_codec_iter_t iter = nullptr;
    const aom_codec_cx_pkt_t* pkt = nullptr;
    bool keyframe = false;
    while ((pkt = aom_codec_get_cx_data(&m_state->ctx, &iter)) != nullptr) {
        if (pkt->kind != AOM_CODEC_CX_FRAME_PKT) continue;
        out.data.append(static_cast<const char*>(pkt->data.frame.buf),
                        int(pkt->data.frame.sz));
        keyframe = keyframe || (pkt->data.frame.flags & AOM_FRAME_IS_KEY);
    }
    if (out.data.isEmpty()) return false;
    out.keyframe = keyframe;
    out.captureTimeUs = in.captureTimeUs;
    out.width = in.width;
    out.height = in.height;
    return true;
}

void AomLosslessEncoder::setBitrate(int, int) {
    // Lossless: bitrate is content-determined; admission control at
    // the transport handles congestion.
}

bool AomLosslessEncoder::reconfigure(const EncoderConfig& config) {
    return init(config);
}
