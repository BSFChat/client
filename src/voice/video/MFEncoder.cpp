#include "voice/video/MFEncoder.h"

#include <QLoggingCategory>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <wrl/client.h>

#include <libyuv.h>

using Microsoft::WRL::ComPtr;

Q_LOGGING_CATEGORY(logMFEnc, "bsfchat.video.mf", QtWarningMsg)

namespace {

// Process-wide one-shot MF init (MFShutdown intentionally skipped —
// the runtime lives as long as the app once video has been used).
bool ensureMFStartup() {
    static const bool ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    return ok;
}

LONGLONG usToMfTime(qint64 us) { return LONGLONG(us) * 10; } // 100 ns units

} // namespace

MFEncoder::MFEncoder(bool preferHardware)
    : m_preferHardware(preferHardware) {}

MFEncoder::~MFEncoder() {
    destroy();
}

void MFEncoder::destroy() {
    if (m_codecApi) { m_codecApi->Release(); m_codecApi = nullptr; }
    if (m_events)   { m_events->Release();   m_events = nullptr; }
    if (m_mft)      { m_mft->Release();      m_mft = nullptr; }
    m_isHardware = false;
    m_isAsync = false;
}

bool MFEncoder::createTransform(bool hardware, const EncoderConfig& config) {
    MFT_REGISTER_TYPE_INFO inInfo{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, MFVideoFormat_H264};
    UINT32 flags = MFT_ENUM_FLAG_SORTANDFILTER
        | (hardware ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT)
                    : MFT_ENUM_FLAG_SYNCMFT);
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &inInfo, &outInfo,
                         &activates, &count)) || count == 0) {
        return false;
    }
    HRESULT hr = activates[0]->ActivateObject(IID_PPV_ARGS(&m_mft));
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    if (FAILED(hr) || !m_mft) return false;

    m_isHardware = hardware;
    m_isAsync = false;
    if (hardware) {
        // Hardware MFTs are async: unlock and grab the event generator.
        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(m_mft->GetAttributes(&attrs)) && attrs) {
            UINT32 isAsync = 0;
            attrs->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
            if (isAsync) {
                attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                if (FAILED(m_mft->QueryInterface(IID_PPV_ARGS(&m_events))))
                    return false;
                m_isAsync = true;
            }
        }
    }
    m_mft->QueryInterface(IID_PPV_ARGS(&m_codecApi)); // optional

    return configureTypes(config);
}

bool MFEncoder::configureTypes(const EncoderConfig& config) {
    // Output type first (encoder MFT convention).
    ComPtr<IMFMediaType> outType;
    if (FAILED(MFCreateMediaType(&outType))) return false;
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, UINT32(config.targetBitrateKbps) * 1000);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outType->SetUINT32(MF_MT_MPEG2_PROFILE,
                       config.profile == H264Profile::High
                           ? eAVEncH264VProfile_High
                           : eAVEncH264VProfile_ConstrainedBase);
    MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE,
                       UINT32(config.width), UINT32(config.height));
    MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE,
                        UINT32(config.fps), 1);
    MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(m_mft->SetOutputType(0, outType.Get(), 0))) return false;

    ComPtr<IMFMediaType> inType;
    if (FAILED(MFCreateMediaType(&inType))) return false;
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE,
                       UINT32(config.width), UINT32(config.height));
    MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE,
                        UINT32(config.fps), 1);
    if (FAILED(m_mft->SetInputType(0, inType.Get(), 0))) return false;

    // GOP length + low-latency knobs (best-effort — not all HW MFTs
    // implement every property).
    if (m_codecApi) {
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = UINT32(config.fps * config.keyframeIntervalSec);
        m_codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &var);
        var.vt = VT_BOOL;
        var.boolVal = VARIANT_TRUE;
        m_codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &var);
        var.vt = VT_UI4;
        var.ulVal = 0;
        m_codecApi->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &var);

        // Explicit rate-control mode. Without one, several MFTs
        // (vendor hardware ones especially) default to a quality mode
        // and silently ignore both MF_MT_AVG_BITRATE and later
        // AVEncCommonMeanBitRate updates — the adaptive controller
        // then "cuts" bitrate with no effect, over-corrects, and
        // ladders resolution down for nothing. LowDelayVBR suits
        // realtime screen content; fall back to CBR (universally
        // supported, incl. the MS software MFT).
        var.vt = VT_UI4;
        var.ulVal = eAVEncCommonRateControlMode_LowDelayVBR;
        if (FAILED(m_codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode,
                                        &var))) {
            var.ulVal = eAVEncCommonRateControlMode_CBR;
            if (FAILED(m_codecApi->SetValue(
                    &CODECAPI_AVEncCommonRateControlMode, &var))) {
                qCInfo(logMFEnc, "MFT accepts no rate-control mode; "
                       "dynamic bitrate may be ignored");
            }
        }
        // Seed the mean bitrate through ICodecAPI too — the path
        // setBitrate() uses for live updates — so mode + rate are
        // consistent from the first frame.
        var.vt = VT_UI4;
        var.ulVal = UINT32(config.targetBitrateKbps) * 1000;
        m_codecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);
        var.vt = VT_BOOL;
        var.boolVal = VARIANT_TRUE;
        m_codecApi->SetValue(&CODECAPI_AVEncCommonRealTime, &var);
    }

    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool MFEncoder::init(const EncoderConfig& config) {
    destroy();
    if (config.codec != VideoCodecKind::H264) return false;
    if (!ensureMFStartup()) return false;

    bool ok = m_preferHardware && createTransform(true, config);
    if (!ok) {
        destroy();
        ok = createTransform(false, config);
    }
    if (!ok) {
        destroy();
        qCWarning(logMFEnc, "no usable H.264 encoder MFT");
        return false;
    }
    m_config = config;
    qCInfo(logMFEnc, "MF encoder up: %dx%d@%dfps %d kbps (%s, %s)",
          config.width, config.height, config.fps, config.targetBitrateKbps,
          m_isHardware ? "hardware" : "software",
          m_isAsync ? "async" : "sync");
    return true;
}

bool MFEncoder::drainOutput(EncodedFrame& out) {
    MFT_OUTPUT_STREAM_INFO streamInfo{};
    if (FAILED(m_mft->GetOutputStreamInfo(0, &streamInfo))) return false;
    const bool mftProvidesSamples =
        (streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES
                               | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

    MFT_OUTPUT_DATA_BUFFER outBuf{};
    outBuf.dwStreamID = 0;
    ComPtr<IMFSample> ourSample;
    ComPtr<IMFMediaBuffer> ourBuffer;
    if (!mftProvidesSamples) {
        if (FAILED(MFCreateSample(&ourSample))) return false;
        const DWORD size = streamInfo.cbSize ? streamInfo.cbSize
                                             : DWORD(4 * 1024 * 1024);
        if (FAILED(MFCreateMemoryBuffer(size, &ourBuffer))) return false;
        ourSample->AddBuffer(ourBuffer.Get());
        outBuf.pSample = ourSample.Get();
    }

    DWORD status = 0;
    HRESULT hr = m_mft->ProcessOutput(0, 1, &outBuf, &status);
    if (outBuf.pEvents) outBuf.pEvents->Release();
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return false;
    if (FAILED(hr)) {
        qCWarning(logMFEnc, "ProcessOutput failed: 0x%lx", hr);
        return false;
    }

    ComPtr<IMFSample> sample;
    if (mftProvidesSamples) sample.Attach(outBuf.pSample);
    else sample = ourSample;
    if (!sample) return false;

    ComPtr<IMFMediaBuffer> contiguous;
    if (FAILED(sample->ConvertToContiguousBuffer(&contiguous))) return false;
    BYTE* data = nullptr;
    DWORD maxLen = 0, curLen = 0;
    if (FAILED(contiguous->Lock(&data, &maxLen, &curLen))) return false;
    out.data = QByteArray(reinterpret_cast<const char*>(data), int(curLen));
    contiguous->Unlock();

    UINT32 cleanPoint = 0;
    sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint);
    out.keyframe = cleanPoint != 0;
    return !out.data.isEmpty();
}

bool MFEncoder::encode(const PlanarFrame& in, bool forceKeyframe,
                       EncodedFrame& out) {
    if (!m_mft || !in.isValid() || in.layout != PlanarFrame::Layout::I420)
        return false;
    if (in.width != m_config.width || in.height != m_config.height)
        return false;

    if (forceKeyframe && m_codecApi) {
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = 1;
        m_codecApi->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &var);
    }

    // I420 → NV12 into the staging buffer, then wrap in an IMFSample.
    const int w = in.width, h = in.height;
    m_nv12.resize(w * h * 3 / 2);
    auto* nv12 = reinterpret_cast<uint8_t*>(m_nv12.data());
    libyuv::I420ToNV12(
        reinterpret_cast<const uint8_t*>(in.y.constData()), in.strideY,
        reinterpret_cast<const uint8_t*>(in.u.constData()), in.strideU,
        reinterpret_cast<const uint8_t*>(in.v.constData()), in.strideV,
        nv12, w, nv12 + w * h, w, w, h);

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(DWORD(m_nv12.size()), &buffer)))
        return false;
    BYTE* dst = nullptr;
    if (FAILED(buffer->Lock(&dst, nullptr, nullptr))) return false;
    memcpy(dst, m_nv12.constData(), size_t(m_nv12.size()));
    buffer->Unlock();
    buffer->SetCurrentLength(DWORD(m_nv12.size()));

    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return false;
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(usToMfTime(in.captureTimeUs));
    sample->SetSampleDuration(10000000LL / m_config.fps);

    if (!m_isAsync) {
        HRESULT hr = m_mft->ProcessInput(0, sample.Get(), 0);
        if (FAILED(hr)) {
            qCWarning(logMFEnc, "ProcessInput failed: 0x%lx", hr);
            return false;
        }
        if (!drainOutput(out)) return false;
    } else {
        // Async MFT: obey METransformNeedInput/HaveOutput events. The
        // encoder is one-in/one-out realtime, so a short event pump per
        // frame suffices; bail after a bounded number of events.
        bool sentInput = false;
        bool haveOutput = false;
        for (int spins = 0; spins < 16 && !haveOutput; ++spins) {
            ComPtr<IMFMediaEvent> event;
            if (FAILED(m_events->GetEvent(0, &event))) break;
            MediaEventType type = MEUnknown;
            event->GetType(&type);
            if (type == METransformNeedInput && !sentInput) {
                if (FAILED(m_mft->ProcessInput(0, sample.Get(), 0))) break;
                sentInput = true;
            } else if (type == METransformHaveOutput) {
                haveOutput = drainOutput(out);
                break;
            }
        }
        if (!haveOutput) return false;
    }

    out.captureTimeUs = in.captureTimeUs;
    out.width = w;
    out.height = h;
    return true;
}

void MFEncoder::setBitrate(int targetKbps, int maxKbps) {
    Q_UNUSED(maxKbps);
    if (!m_codecApi) return;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = UINT32(targetKbps) * 1000;
    if (FAILED(m_codecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var))) {
        qCDebug(logMFEnc, "dynamic bitrate unsupported by this MFT");
    }
    m_config.targetBitrateKbps = targetKbps;
}

bool MFEncoder::reconfigure(const EncoderConfig& config) {
    // Dynamic format change support is spotty across vendor MFTs —
    // recreate instead of trusting it.
    return init(config);
}
