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
bool ensureMFStartup() {
    static const bool ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    return ok;
}
} // namespace

MFDecoder::~MFDecoder() {
    destroy();
}

void MFDecoder::destroy() {
    if (m_mft) { m_mft->Release(); m_mft = nullptr; }
    m_width = m_height = 0;
}

bool MFDecoder::init(VideoCodecKind kind) {
    destroy();
    if (kind != VideoCodecKind::H264) return false;
    if (!ensureMFStartup()) return false;

    if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_mft)))) {
        qCWarning(logMFDec, "H.264 decoder MFT unavailable");
        return false;
    }

    // Low-latency: emit frames in decode order without buffering a
    // reorder window (our streams carry no B-frames anyway).
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(m_mft->GetAttributes(&attrs)) && attrs)
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);

    ComPtr<IMFMediaType> inType;
    if (FAILED(MFCreateMediaType(&inType))) return false;
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (FAILED(m_mft->SetInputType(0, inType.Get(), 0))) {
        qCWarning(logMFDec, "SetInputType(H264) failed");
        destroy();
        return false;
    }
    if (!negotiateOutputType()) {
        destroy();
        return false;
    }
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool MFDecoder::negotiateOutputType() {
    // Pick the NV12 output type the decoder offers.
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        HRESULT hr = m_mft->GetOutputAvailableType(0, i, &type);
        if (FAILED(hr)) break;
        GUID subtype = GUID_NULL;
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype == MFVideoFormat_NV12) {
            if (FAILED(m_mft->SetOutputType(0, type.Get(), 0))) return false;
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
            m_width = int(w);
            m_height = int(h);
            return true;
        }
    }
    return false;
}

VideoDecoder::Result MFDecoder::decode(const QByteArray& au, QVideoFrame& out) {
    if (!m_mft || au.isEmpty()) return Result::Error;

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
        qCWarning(logMFDec, "ProcessInput failed: 0x%lx", hr);
        return Result::Error;
    }

    for (;;) {
        MFT_OUTPUT_STREAM_INFO streamInfo{};
        if (FAILED(m_mft->GetOutputStreamInfo(0, &streamInfo)))
            return Result::Error;

        ComPtr<IMFSample> outSample;
        ComPtr<IMFMediaBuffer> outBuffer;
        if (FAILED(MFCreateSample(&outSample))) return Result::Error;
        if (FAILED(MFCreateMemoryBuffer(
                streamInfo.cbSize ? streamInfo.cbSize
                                  : DWORD(m_width * m_height * 3 / 2 + 4096),
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
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // Mid-stream resolution change: renegotiate and retry.
            if (!negotiateOutputType()) return Result::Error;
            continue;
        }
        if (FAILED(hr)) {
            qCWarning(logMFDec, "ProcessOutput failed: 0x%lx", hr);
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
        bool ok = false;
        if (int(len) >= w * h * 3 / 2 && w > 0 && h > 0) {
            QVideoFrameFormat fmt(QSize(w, h), QVideoFrameFormat::Format_NV12);
            QVideoFrame frame(fmt);
            if (frame.map(QVideoFrame::WriteOnly)) {
                // Decoder output is tightly packed NV12 (stride == w
                // for the MS software MFT at even widths).
                const uint8_t* srcY = data;
                const uint8_t* srcUV = data + w * h;
                for (int row = 0; row < h; ++row)
                    memcpy(frame.bits(0) + row * frame.bytesPerLine(0),
                           srcY + row * w, size_t(w));
                for (int row = 0; row < h / 2; ++row)
                    memcpy(frame.bits(1) + row * frame.bytesPerLine(1),
                           srcUV + row * w, size_t(w));
                frame.unmap();
                out = frame;
                ok = true;
            }
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
