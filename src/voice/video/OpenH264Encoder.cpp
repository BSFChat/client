#include "voice/video/OpenH264Encoder.h"

#include <QLoggingCategory>

#include <wels/codec_api.h>

#include <cstring>

Q_LOGGING_CATEGORY(logVideoEnc, "bsfchat.video.encode", QtWarningMsg)

OpenH264Encoder::~OpenH264Encoder() {
    destroy();
}

void OpenH264Encoder::destroy() {
    if (m_encoder) {
        m_encoder->Uninitialize();
        WelsDestroySVCEncoder(m_encoder);
        m_encoder = nullptr;
    }
}

bool OpenH264Encoder::init(const EncoderConfig& config) {
    destroy();
    if (config.codec != VideoCodecKind::H264) return false;

    if (WelsCreateSVCEncoder(&m_encoder) != 0 || !m_encoder) {
        qCWarning(logVideoEnc, "openh264: WelsCreateSVCEncoder failed");
        m_encoder = nullptr;
        return false;
    }

    SEncParamExt param;
    m_encoder->GetDefaultParams(&param);
    param.iUsageType = config.screenContent ? SCREEN_CONTENT_REAL_TIME
                                            : CAMERA_VIDEO_REAL_TIME;
    param.iPicWidth = config.width;
    param.iPicHeight = config.height;
    param.fMaxFrameRate = float(config.fps);
    param.iTargetBitrate = config.targetBitrateKbps * 1000;
    param.iMaxBitrate = config.maxBitrateKbps * 1000;
    param.iRCMode = RC_BITRATE_MODE;
    // The rate controller upstream owns the smoothness/quality
    // trade-off — encoder-side frame skipping would fight it by
    // silently dropping frames the pipeline believes it sent.
    param.bEnableFrameSkip = false;
    param.uiIntraPeriod = unsigned(config.fps * config.keyframeIntervalSec);
    param.iTemporalLayerNum = 1;
    param.iSpatialLayerNum = 1;
    param.iMultipleThreadIdc = 0;   // auto
    param.sSpatialLayers[0].iVideoWidth = config.width;
    param.sSpatialLayers[0].iVideoHeight = config.height;
    param.sSpatialLayers[0].fFrameRate = float(config.fps);
    param.sSpatialLayers[0].iSpatialBitrate = param.iTargetBitrate;
    param.sSpatialLayers[0].iMaxSpatialBitrate = param.iMaxBitrate;
    // openh264 encodes Constrained Baseline regardless; pin it so the
    // bitstream matches what our caps advertised.
    param.sSpatialLayers[0].uiProfileIdc = PRO_BASELINE;

    if (m_encoder->InitializeExt(&param) != cmResultSuccess) {
        qCWarning(logVideoEnc, "openh264: InitializeExt failed (%dx%d@%d)",
                 config.width, config.height, config.fps);
        destroy();
        return false;
    }
    int videoFormat = videoFormatI420;
    m_encoder->SetOption(ENCODER_OPTION_DATAFORMAT, &videoFormat);

    m_config = config;
    qCInfo(logVideoEnc, "openh264 encoder up: %dx%d@%dfps %d kbps (%s)",
          config.width, config.height, config.fps, config.targetBitrateKbps,
          config.screenContent ? "screen" : "camera");
    return true;
}

bool OpenH264Encoder::encode(const PlanarFrame& in, bool forceKeyframe,
                             EncodedFrame& out) {
    if (!m_encoder || !in.isValid() || in.layout != PlanarFrame::Layout::I420)
        return false;
    if (in.width != m_config.width || in.height != m_config.height) {
        // Pipeline is expected to reconfigure() before feeding new dims.
        qCWarning(logVideoEnc, "openh264: frame %dx%d != session %dx%d",
                 in.width, in.height, m_config.width, m_config.height);
        return false;
    }

    if (forceKeyframe) m_encoder->ForceIntraFrame(true);

    SSourcePicture pic;
    std::memset(&pic, 0, sizeof(pic));
    pic.iColorFormat = videoFormatI420;
    pic.iPicWidth = in.width;
    pic.iPicHeight = in.height;
    pic.iStride[0] = in.strideY;
    pic.iStride[1] = in.strideU;
    pic.iStride[2] = in.strideV;
    pic.pData[0] = reinterpret_cast<unsigned char*>(const_cast<char*>(in.y.constData()));
    pic.pData[1] = reinterpret_cast<unsigned char*>(const_cast<char*>(in.u.constData()));
    pic.pData[2] = reinterpret_cast<unsigned char*>(const_cast<char*>(in.v.constData()));
    pic.uiTimeStamp = in.captureTimeUs / 1000;

    SFrameBSInfo info;
    std::memset(&info, 0, sizeof(info));
    if (m_encoder->EncodeFrame(&pic, &info) != cmResultSuccess) {
        qCWarning(logVideoEnc, "openh264: EncodeFrame failed");
        return false;
    }
    if (info.eFrameType == videoFrameTypeSkip) return false;

    // Concatenate all NAL units of all layers — openh264 writes them
    // back-to-back per layer with Annex-B start codes already in place.
    out.data.clear();
    for (int layer = 0; layer < info.iLayerNum; ++layer) {
        const SLayerBSInfo& li = info.sLayerInfo[layer];
        int layerBytes = 0;
        for (int n = 0; n < li.iNalCount; ++n) layerBytes += li.pNalLengthInByte[n];
        out.data.append(reinterpret_cast<const char*>(li.pBsBuf), layerBytes);
    }
    out.keyframe = (info.eFrameType == videoFrameTypeIDR);
    out.captureTimeUs = in.captureTimeUs;
    out.width = in.width;
    out.height = in.height;
    return !out.data.isEmpty();
}

void OpenH264Encoder::setBitrate(int targetKbps, int maxKbps) {
    if (!m_encoder) return;
    SBitrateInfo target;
    target.iLayer = SPATIAL_LAYER_ALL;
    target.iBitrate = targetKbps * 1000;
    m_encoder->SetOption(ENCODER_OPTION_BITRATE, &target);
    SBitrateInfo max;
    max.iLayer = SPATIAL_LAYER_ALL;
    max.iBitrate = maxKbps * 1000;
    m_encoder->SetOption(ENCODER_OPTION_MAX_BITRATE, &max);
    m_config.targetBitrateKbps = targetKbps;
    m_config.maxBitrateKbps = maxKbps;
}

bool OpenH264Encoder::reconfigure(const EncoderConfig& config) {
    // openh264 has no reliable in-place resolution change — rebuild.
    // Cheap for a software session, and the next frame is an IDR.
    return init(config);
}
