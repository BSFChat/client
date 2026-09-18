#include "voice/video/MFDecoder.h"

#include <QLoggingCategory>
#include <QVideoFrameFormat>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

Q_LOGGING_CATEGORY(logMFDec, "bsfchat.video.mf", QtWarningMsg)

namespace {

// Size we describe the stream as while setting up. It is a HINT, not a
// promise: decoder MFTs want an input type that carries a frame size
// before they will describe their output, but the size that ends up in
// m_width/m_height always comes back off the output type the MFT
// accepts (and again on every MF_E_TRANSFORM_STREAM_CHANGE), never from
// these constants.
constexpr int kHintWidth = 1920;
constexpr int kHintHeight = 1080;
constexpr UINT32 kHintFps = 30;

// How many access units we will feed a decoder that has not described
// its output yet before declaring it a lost cause, and how many times a
// single access unit may send us round the renegotiation loop.
constexpr int kMaxDeferredNegotiations = 30;
constexpr int kMaxRenegotiations = 4;

bool ensureMFStartup() {
    static const bool ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    return ok;
}

// HRESULT is LONG; log it unsigned so the 0x8007.../0xC00D... shape a
// reader recognises survives the trip.
unsigned long hrToUL(HRESULT hr) { return static_cast<unsigned long>(hr); }

const GUID& mfSubtypeFor(VideoCodecKind kind) {
    return kind == VideoCodecKind::H265 ? MFVideoFormat_HEVC : MFVideoFormat_H264;
}

// Outcome of configuring a decoder MFT. `failedStep` is the name of the
// step that went wrong (nullptr ⇒ everything succeeded) and travels with
// the HRESULT so every refusal can be logged with both.
struct MftConfig {
    const char* failedStep = nullptr;
    HRESULT hr = S_OK;
    bool inputTypeSet = false;
    bool outputTypeSet = false;
    int width = 0;
    int height = 0;
    int stride = 0;
    bool ok() const { return failedStep == nullptr; }
    void fail(const char* step, HRESULT result) {
        failedStep = step;
        hr = result;
    }
};

// Build the input media type we hand a decoder.
//
// `described` false yields the bare major-type + subtype pair this class
// used to send. HEVC decoder MFTs — the Store "HEVC Video Extensions"
// one and the vendor ones — will accept that and then refuse to
// enumerate a single output type, because they have not been told a
// frame size; CMSH264DecoderMFT never needed one, which is exactly why
// H.264 worked and H.265 silently did not.
HRESULT buildInputType(VideoCodecKind kind, int width, int height,
                       bool described, ComPtr<IMFMediaType>& out) {
    ComPtr<IMFMediaType> type;
    HRESULT hr = MFCreateMediaType(&type);
    if (FAILED(hr)) return hr;
    hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, mfSubtypeFor(kind));
    if (SUCCEEDED(hr) && described) {
        hr = MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE,
                                UINT32(width), UINT32(height));
        if (SUCCEEDED(hr))
            hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (SUCCEEDED(hr))
            hr = MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, kHintFps, 1);
        if (SUCCEEDED(hr))
            hr = MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    }
    if (FAILED(hr)) return hr;
    out = type;
    return S_OK;
}

// Walk the decoder's output types, select the first NV12 one, and read
// the coded size and stride back off the type the MFT actually accepted.
void selectNv12Output(IMFTransform* mft, MftConfig& cfg) {
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        HRESULT hr = mft->GetOutputAvailableType(0, i, &type);
        if (FAILED(hr)) {
            // MF_E_NO_MORE_TYPES just ends the list; anything else
            // (MF_E_TRANSFORM_TYPE_NOT_SET above all) is the real
            // reason and worth keeping.
            cfg.fail(hr == MF_E_NO_MORE_TYPES ? "no NV12 output type offered"
                                              : "GetOutputAvailableType",
                     hr);
            return;
        }
        GUID subtype = GUID_NULL;
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) continue;
        if (subtype != MFVideoFormat_NV12) continue;

        hr = mft->SetOutputType(0, type.Get(), 0);
        if (FAILED(hr)) {
            cfg.fail("SetOutputType(NV12)", hr);
            return;
        }
        // The accepted type is the authority on the coded size: a
        // decoder routinely snaps 1080 up to 1088, and after a stream
        // change this is where the NEW size comes from.
        ComPtr<IMFMediaType> accepted;
        if (FAILED(mft->GetOutputCurrentType(0, &accepted)) || !accepted)
            accepted = type;
        UINT32 w = 0, h = 0;
        if (FAILED(MFGetAttributeSize(accepted.Get(), MF_MT_FRAME_SIZE, &w, &h))
            || w == 0 || h == 0) {
            cfg.fail("output type carries no frame size", MF_E_INVALIDMEDIATYPE);
            return;
        }
        // MF_MT_DEFAULT_STRIDE is a UINT32 holding a signed stride; a
        // negative (bottom-up) or absurd one means "assume packed".
        UINT32 rawStride = 0;
        INT32 stride = 0;
        if (SUCCEEDED(accepted->GetUINT32(MF_MT_DEFAULT_STRIDE, &rawStride)))
            stride = INT32(rawStride);
        if (stride < INT32(w) || stride > 4 * INT32(w)) stride = INT32(w);

        cfg.width = int(w);
        cfg.height = int(h);
        cfg.stride = int(stride);
        cfg.outputTypeSet = true;
        cfg.failedStep = nullptr;
        cfg.hr = S_OK;
        return;
    }
}

// Everything a decoder MFT needs before it will talk: the low-latency
// hint, a fully described input type, and an NV12 output type. Used by
// BOTH hevcDecodeSupported() and init() — the probe's whole job is to
// answer "would init() work?", so it has to walk the same steps or it
// will drift back into lying.
void configureMft(IMFTransform* mft, VideoCodecKind kind, int width,
                  int height, MftConfig& cfg) {
    // Low-latency: emit frames in decode order without buffering a
    // reorder window (our streams carry no B-frames anyway).
    // Best-effort — not every MFT implements the attribute.
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(mft->GetAttributes(&attrs)) && attrs)
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);

    ComPtr<IMFMediaType> inType;
    HRESULT hr = buildInputType(kind, width, height, /*described=*/true, inType);
    if (FAILED(hr)) {
        cfg.fail("build input type", hr);
        return;
    }
    hr = mft->SetInputType(0, inType.Get(), 0);
    if (FAILED(hr)) {
        // A decoder that dislikes being described still gets offered the
        // bare type we have always sent: H.264 is the fallback the whole
        // product leans on and must not regress because of a hint.
        const HRESULT describedHr = hr;
        ComPtr<IMFMediaType> bare;
        if (SUCCEEDED(buildInputType(kind, width, height, /*described=*/false,
                                     bare))) {
            hr = mft->SetInputType(0, bare.Get(), 0);
            if (SUCCEEDED(hr)) {
                qCWarning(logMFDec,
                          "%s MFT refused a described input type (0x%08lX); "
                          "fell back to subtype only",
                          videoCodecName(kind), hrToUL(describedHr));
            }
        }
        if (FAILED(hr)) {
            cfg.fail("SetInputType", describedHr);
            return;
        }
    }
    cfg.inputTypeSet = true;
    selectNv12Output(mft, cfg);
}

// Activate the best registered decoder MFT for `subtype`, or return
// nullptr. Used for HEVC, which — unlike H.264 — has no CLSID we can
// count on: the decoder ships with the vendor driver or with the Store
// "HEVC Video Extensions" package, and on a machine with neither it
// simply is not there.
//
// "Best" means the first one that ACTIVATES AND CONFIGURES. Enumeration
// alone was the old test and it is what shipped the bug: a registered
// MFT that cannot be driven is not a decoder, it is a black tile.
// `cfg` comes back describing the returned MFT (already configured), or
// carrying the first failure when nothing worked.
IMFTransform* activateDecoderMft(const GUID& subtype, VideoCodecKind kind,
                                 int width, int height, MftConfig& cfg) {
    MFT_REGISTER_TYPE_INFO inInfo{MFMediaType_Video, subtype};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    // SYNC MFTs only. This class drives ProcessInput/ProcessOutput
    // directly and has no event pump, so an async (hardware) MFT would
    // activate happily and then never produce a frame. Hardware decode
    // is not lost by this: DXVA acceleration lives INSIDE the sync
    // decoder MFTs on Windows, which is also how the H.264 path has
    // always got it.
    const UINT32 flags = MFT_ENUM_FLAG_SORTANDFILTER | MFT_ENUM_FLAG_SYNCMFT;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &inInfo, nullptr,
                           &activates, &count);
    if (FAILED(hr) || count == 0) {
        if (activates) CoTaskMemFree(activates);
        cfg.fail("no decoder MFT registered", FAILED(hr) ? hr : E_FAIL);
        return nullptr;
    }

    IMFTransform* chosen = nullptr;   // fully configured
    MftConfig chosenCfg;
    IMFTransform* partial = nullptr;  // took our input type, described no output
    MftConfig partialCfg;
    MftConfig firstFailure;
    bool haveFailure = false;

    for (UINT32 i = 0; i < count; ++i) {
        if (!chosen) {
            IMFTransform* mft = nullptr;
            hr = activates[i]->ActivateObject(IID_PPV_ARGS(&mft));
            if (SUCCEEDED(hr) && mft) {
                MftConfig attempt;
                configureMft(mft, kind, width, height, attempt);
                if (attempt.ok()) {
                    chosen = mft;
                    chosenCfg = attempt;
                    mft = nullptr;
                } else if (attempt.inputTypeSet && !partial) {
                    partial = mft;
                    partialCfg = attempt;
                    mft = nullptr;
                } else if (!haveFailure) {
                    firstFailure = attempt;
                    haveFailure = true;
                }
                if (mft) mft->Release();
            } else if (!haveFailure) {
                firstFailure.fail("ActivateObject", hr);
                haveFailure = true;
            }
        }
        activates[i]->Release();
    }
    CoTaskMemFree(activates);

    if (chosen) {
        if (partial) partial->Release();
        cfg = chosenCfg;
        return chosen;
    }
    if (partial) {
        cfg = partialCfg;
        return partial;
    }
    if (haveFailure) cfg = firstFailure;
    else cfg.fail("no decoder MFT could be activated", E_FAIL);
    return nullptr;
}

} // namespace

bool MFDecoder::hevcDecodeSupported() {
    static const bool supported = []() -> bool {
        if (!ensureMFStartup()) {
            qCInfo(logMFDec, "HEVC decode %s (%s)", "unavailable",
                   "Media Foundation would not start");
            return false;
        }
        // The honest question is not "is an HEVC MFT registered?" but
        // "can one be driven?". A Windows client that answered the first
        // question advertised h265, flipped two Mac senders onto it, and
        // then logged `no decoder available` 17,000 times at a pair of
        // black tiles. So: activate, set a described input type,
        // negotiate NV12 out — the same steps init() takes — and
        // release. Only then is the answer yes.
        MftConfig cfg;
        IMFTransform* mft = activateDecoderMft(MFVideoFormat_HEVC,
                                               VideoCodecKind::H265, kHintWidth,
                                               kHintHeight, cfg);
        if (mft) mft->Release();
        const bool ok = mft != nullptr && cfg.ok();
        if (ok) {
            qCInfo(logMFDec, "HEVC decode %s (decoder MFT configured, %dx%d NV12)",
                   "available", cfg.width, cfg.height);
        } else {
            qCInfo(logMFDec, "HEVC decode %s (%s: 0x%08lX)", "unavailable",
                   cfg.failedStep ? cfg.failedStep : "MFT setup", hrToUL(cfg.hr));
        }
        return ok;
    }();
    return supported;
}

MFDecoder::~MFDecoder() {
    destroy();
}

void MFDecoder::destroy() {
    if (m_mft) { m_mft->Release(); m_mft = nullptr; }
    m_width = m_height = m_stride = 0;
    m_outputTypeSet = false;
    m_unusable = false;
    m_deferredNegotiations = 0;
    m_lastNegotiateHr = 0;
    m_lastNegotiateStep = nullptr;
}

bool MFDecoder::init(VideoCodecKind kind) {
    destroy();
    if (kind != VideoCodecKind::H264 && kind != VideoCodecKind::H265)
        return false;
    if (!ensureMFStartup()) return false;
    m_kind = kind;
    const bool hevc = kind == VideoCodecKind::H265;

    MftConfig cfg;
    if (hevc) {
        // Enumerated, activated and configured in one go, so the MFT we
        // keep is one that answered every question the probe asks.
        m_mft = activateDecoderMft(MFVideoFormat_HEVC, kind, kHintWidth,
                                   kHintHeight, cfg);
    } else {
        const HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr,
                                            CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(&m_mft));
        if (FAILED(hr) || !m_mft) {
            m_mft = nullptr;
            cfg.fail("CoCreateInstance(CMSH264DecoderMFT)", hr);
        } else {
            configureMft(m_mft, kind, kHintWidth, kHintHeight, cfg);
        }
    }
    if (!m_mft) {
        qCWarning(logMFDec, "%s decoder MFT unavailable (%s: 0x%08lX)",
                  videoCodecName(kind),
                  cfg.failedStep ? cfg.failedStep : "activation",
                  hrToUL(cfg.hr));
        return false;
    }
    if (!cfg.inputTypeSet) {
        qCWarning(logMFDec, "%s decoder rejected its input type (%s: 0x%08lX)",
                  videoCodecName(kind),
                  cfg.failedStep ? cfg.failedStep : "SetInputType",
                  hrToUL(cfg.hr));
        destroy();
        return false;
    }

    if (cfg.outputTypeSet) {
        m_width = cfg.width;
        m_height = cfg.height;
        m_stride = cfg.stride;
        m_outputTypeSet = true;
    } else {
        // Not fatal by itself: a decoder is allowed to describe its
        // output only once it has seen the stream, and announces it
        // with MF_E_TRANSFORM_STREAM_CHANGE on the first ProcessOutput
        // — decode() picks it up there. Loud, though, because for HEVC
        // this is the exact shape of the field bug, and because the
        // caps probe will have declined to advertise h265 for the same
        // reason: whatever we do here, nobody should be sending us this
        // codec.
        m_lastNegotiateHr = cfg.hr;
        m_lastNegotiateStep = cfg.failedStep;
        qCWarning(logMFDec,
                  "%s decoder offered no NV12 output type yet (%s: 0x%08lX); "
                  "waiting for the stream-change handshake",
                  videoCodecName(kind),
                  cfg.failedStep ? cfg.failedStep : "negotiate output",
                  hrToUL(cfg.hr));
    }

    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    qCInfo(logMFDec, "MF %s decoder up: %dx%d NV12 (stride %d)%s",
           videoCodecName(kind), m_width, m_height, m_stride,
           m_outputTypeSet ? "" : " (output type still pending)");
    return true;
}

bool MFDecoder::negotiateOutputType(bool logFailure) {
    if (!m_mft) return false;
    MftConfig cfg;
    selectNv12Output(m_mft, cfg);
    m_lastNegotiateHr = cfg.hr;
    m_lastNegotiateStep = cfg.failedStep;
    if (!cfg.outputTypeSet) {
        m_outputTypeSet = false;
        if (logFailure) {
            qCWarning(logMFDec, "%s output negotiation failed (%s: 0x%08lX)",
                      videoCodecName(m_kind),
                      cfg.failedStep ? cfg.failedStep : "negotiate output",
                      hrToUL(cfg.hr));
        }
        return false;
    }
    if (m_outputTypeSet && (cfg.width != m_width || cfg.height != m_height)) {
        qCInfo(logMFDec, "%s output format change: %dx%d -> %dx%d",
               videoCodecName(m_kind), m_width, m_height, cfg.width, cfg.height);
    }
    m_width = cfg.width;
    m_height = cfg.height;
    m_stride = cfg.stride;
    m_outputTypeSet = true;
    return true;
}

void MFDecoder::markUnusable(const char* why, long hr) {
    if (m_unusable) return;
    m_unusable = true;
    qCWarning(logMFDec,
              "%s decoder is unusable (%s: 0x%08lX); dropping access units "
              "instead of repeating this line for every one of them",
              videoCodecName(m_kind), why, hrToUL(HRESULT(hr)));
}

VideoDecoder::Result MFDecoder::decode(const QByteArray& au, QVideoFrame& out) {
    if (!m_mft || m_unusable || au.isEmpty()) return Result::Error;

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(DWORD(au.size()), &buffer)))
        return Result::Error;
    BYTE* dst = nullptr;
    if (FAILED(buffer->Lock(&dst, nullptr, nullptr))) return Result::Error;
    memcpy(dst, au.constData(), size_t(au.size()));
    buffer->Unlock();
    buffer->SetCurrentLength(DWORD(au.size()));

    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return Result::Error;
    sample->AddBuffer(buffer.Get());

    HRESULT hr = m_mft->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) {
        qCWarning(logMFDec, "%s ProcessInput failed: 0x%08lX",
                  videoCodecName(m_kind), hrToUL(hr));
        // A decoder that never got an output type refuses every access
        // unit identically. Latch rather than log it 17,000 times.
        if (!m_outputTypeSet) markUnusable("ProcessInput without an output type", hr);
        return Result::Error;
    }

    // Deferred negotiation: the decoder had nothing to say about its
    // output at init() and may now, having seen data.
    if (!m_outputTypeSet && !negotiateOutputType(/*logFailure=*/false)) {
        if (++m_deferredNegotiations >= kMaxDeferredNegotiations) {
            markUnusable(m_lastNegotiateStep ? m_lastNegotiateStep
                                             : "no NV12 output type",
                         m_lastNegotiateHr);
            return Result::Error;
        }
        return Result::NeedMore;
    }

    int renegotiations = 0;
    for (;;) {
        MFT_OUTPUT_STREAM_INFO streamInfo{};
        if (FAILED(m_mft->GetOutputStreamInfo(0, &streamInfo)))
            return Result::Error;

        ComPtr<IMFSample> outSample;
        ComPtr<IMFMediaBuffer> outBuffer;
        if (FAILED(MFCreateSample(&outSample))) return Result::Error;
        if (FAILED(MFCreateMemoryBuffer(
                streamInfo.cbSize ? streamInfo.cbSize
                                  : DWORD(m_stride * m_height * 3 / 2 + 4096),
                &outBuffer)))
            return Result::Error;
        outSample->AddBuffer(outBuffer.Get());

        MFT_OUTPUT_DATA_BUFFER outData{};
        outData.dwStreamID = 0;
        outData.pSample = outSample.Get();
        DWORD status = 0;
        hr = m_mft->ProcessOutput(0, 1, &outData, &status);
        if (outData.pEvents) outData.pEvents->Release();

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
            return Result::NeedMore;   // parameter sets consumed, no frame yet
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE || hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
            // The documented handshake: the decoder has worked out the
            // real coded size (or is saying it never had an output type
            // at all) and wants the output type set again before it
            // will part with a frame. This is where m_width/m_height
            // get their true values — the init() hint is only ever a
            // hint.
            if (++renegotiations > kMaxRenegotiations) {
                markUnusable("decoder will not settle on an output type", hr);
                return Result::Error;
            }
            if (!negotiateOutputType()) {
                markUnusable(m_lastNegotiateStep ? m_lastNegotiateStep
                                                 : "renegotiate output",
                             m_lastNegotiateHr);
                return Result::Error;
            }
            continue;
        }
        if (FAILED(hr)) {
            qCWarning(logMFDec, "%s ProcessOutput failed: 0x%08lX",
                      videoCodecName(m_kind), hrToUL(hr));
            return Result::Error;
        }

        ComPtr<IMFMediaBuffer> contiguous;
        if (FAILED(outSample->ConvertToContiguousBuffer(&contiguous)))
            return Result::Error;
        BYTE* data = nullptr;
        DWORD len = 0;
        if (FAILED(contiguous->Lock(&data, nullptr, &len)))
            return Result::Error;

        const int w = m_width, h = m_height;
        const int srcStride = m_stride > 0 ? m_stride : w;
        bool ok = false;
        if (w > 0 && h > 0 && int(len) >= srcStride * h * 3 / 2) {
            QVideoFrameFormat fmt(QSize(w, h), QVideoFrameFormat::Format_NV12);
            QVideoFrame frame(fmt);
            if (frame.map(QVideoFrame::WriteOnly)) {
                // Decoder output is NV12 at the stride the output type
                // declared (packed, stride == width, for the MS
                // software MFT at even widths).
                const uint8_t* srcY = data;
                const uint8_t* srcUV = data + srcStride * h;
                for (int row = 0; row < h; ++row)
                    memcpy(frame.bits(0) + row * frame.bytesPerLine(0),
                           srcY + row * srcStride, size_t(w));
                for (int row = 0; row < h / 2; ++row)
                    memcpy(frame.bits(1) + row * frame.bytesPerLine(1),
                           srcUV + row * srcStride, size_t(w));
                frame.unmap();
                out = frame;
                ok = true;
            } else {
                qCWarning(logMFDec, "%s could not map a %dx%d NV12 frame",
                          videoCodecName(m_kind), w, h);
            }
        } else {
            qCWarning(logMFDec,
                      "%s decoder handed back %lu bytes for %dx%d (stride %d)",
                      videoCodecName(m_kind), static_cast<unsigned long>(len), w,
                      h, srcStride);
        }
        contiguous->Unlock();
        return ok ? Result::Ok : Result::Error;
    }
}

void MFDecoder::reset() {
    if (m_mft) {
        m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
}
