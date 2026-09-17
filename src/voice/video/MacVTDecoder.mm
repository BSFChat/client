#include "voice/video/MacVTDecoder.h"

#include <QLoggingCategory>
#include <QVideoFrameFormat>

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#include <vector>

Q_LOGGING_CATEGORY(logVTDec, "bsfchat.video.vt", QtWarningMsg)

struct MacVTDecoder::OutputSlot {
    QVideoFrame frame;
    bool valid = false;
};

namespace {

struct Nal { const uint8_t* data; int size; };

// Split an Annex-B AU into NAL units (3- and 4-byte start codes).
std::vector<Nal> splitAnnexB(const QByteArray& au) {
    const auto* p = reinterpret_cast<const uint8_t*>(au.constData());
    const int n = au.size();
    std::vector<int> nalStart, codeStart;   // parallel arrays
    for (int i = 0; i + 2 < n;) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            codeStart.push_back(i);
            nalStart.push_back(i + 3);
            i += 3;
        } else {
            ++i;
        }
    }
    std::vector<Nal> nals;
    for (size_t k = 0; k < nalStart.size(); ++k) {
        int end = (k + 1 < nalStart.size()) ? codeStart[k + 1] : n;
        // A 4-byte start code's leading zero sits at the tail of the
        // previous NAL's range — strip it.
        if (k + 1 < nalStart.size() && end > nalStart[k] && p[end - 1] == 0)
            --end;
        if (end > nalStart[k]) nals.push_back({p + nalStart[k], end - nalStart[k]});
    }
    return nals;
}

void decompressionCallback(void* refcon, void* /*sourceRefcon*/,
                           OSStatus status, VTDecodeInfoFlags,
                           CVImageBufferRef imageBuffer, CMTime, CMTime) {
    auto* slot = static_cast<MacVTDecoder::OutputSlot*>(refcon);
    slot->valid = false;
    if (status != noErr || !imageBuffer) return;

    CVPixelBufferRef pixel = (CVPixelBufferRef)imageBuffer;
    CVPixelBufferLockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
    const int w = int(CVPixelBufferGetWidth(pixel));
    const int h = int(CVPixelBufferGetHeight(pixel));

    QVideoFrameFormat fmt(QSize(w, h), QVideoFrameFormat::Format_NV12);
    QVideoFrame frame(fmt);
    if (frame.map(QVideoFrame::WriteOnly)) {
        for (int plane = 0; plane < 2; ++plane) {
            const auto* src = static_cast<const uint8_t*>(
                CVPixelBufferGetBaseAddressOfPlane(pixel, size_t(plane)));
            const size_t srcStride =
                CVPixelBufferGetBytesPerRowOfPlane(pixel, size_t(plane));
            uchar* dst = frame.bits(plane);
            const int dstStride = frame.bytesPerLine(plane);
            const int rows = plane == 0 ? h : h / 2;
            const int cols = plane == 0 ? w : w; // NV12 UV plane: w bytes/row
            for (int row = 0; row < rows; ++row)
                memcpy(dst + row * dstStride, src + size_t(row) * srcStride,
                       size_t(cols));
        }
        frame.unmap();
        slot->frame = frame;
        slot->valid = true;
    }
    CVPixelBufferUnlockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
}

} // namespace

bool MacVTDecoder::hevcDecodeSupported() {
    static const bool supported = []() -> bool {
        const bool ok = VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC);
        qCInfo(logVTDec, "HEVC decode %s on this Mac",
              ok ? "available" : "unavailable");
        return ok;
    }();
    return supported;
}

MacVTDecoder::~MacVTDecoder() {
    destroy();
    delete m_slot;
}

void MacVTDecoder::destroy() {
    if (m_session) {
        auto session = (VTDecompressionSessionRef)m_session;
        VTDecompressionSessionInvalidate(session);
        CFRelease(session);
        m_session = nullptr;
    }
    if (m_format) {
        CFRelease((CMVideoFormatDescriptionRef)m_format);
        m_format = nullptr;
    }
    m_vps.clear();
    m_sps.clear();
    m_pps.clear();
}

bool MacVTDecoder::init(VideoCodecKind kind) {
    destroy();
    if (kind != VideoCodecKind::H264 && kind != VideoCodecKind::H265)
        return false;
    m_kind = kind;
    if (!m_slot) m_slot = new OutputSlot;
    // Session is built lazily from the first parameter sets.
    return true;
}

bool MacVTDecoder::ensureSession(const QByteArray& vps, const QByteArray& sps,
                                 const QByteArray& pps) {
    if (m_session && vps == m_vps && sps == m_sps && pps == m_pps) return true;
    // Parameter sets changed (or first keyframe) — rebuild.
    if (m_session) {
        auto session = (VTDecompressionSessionRef)m_session;
        VTDecompressionSessionInvalidate(session);
        CFRelease(session);
        m_session = nullptr;
    }
    if (m_format) {
        CFRelease((CMVideoFormatDescriptionRef)m_format);
        m_format = nullptr;
    }

    // HEVC needs all three, in VPS, SPS, PPS order; H.264 takes two.
    const bool hevc = m_kind == VideoCodecKind::H265;
    const uint8_t* paramSets[3];
    size_t paramSizes[3];
    size_t paramCount = 0;
    if (hevc) {
        paramSets[paramCount] = reinterpret_cast<const uint8_t*>(vps.constData());
        paramSizes[paramCount++] = size_t(vps.size());
    }
    paramSets[paramCount] = reinterpret_cast<const uint8_t*>(sps.constData());
    paramSizes[paramCount++] = size_t(sps.size());
    paramSets[paramCount] = reinterpret_cast<const uint8_t*>(pps.constData());
    paramSizes[paramCount++] = size_t(pps.size());

    CMVideoFormatDescriptionRef format = nullptr;
    OSStatus status = hevc
        ? CMVideoFormatDescriptionCreateFromHEVCParameterSets(
              kCFAllocatorDefault, paramCount, paramSets, paramSizes, 4,
              nullptr, &format)
        : CMVideoFormatDescriptionCreateFromH264ParameterSets(
              kCFAllocatorDefault, paramCount, paramSets, paramSizes, 4, &format);
    if (status != noErr || !format) {
        qCWarning(logVTDec, "%s format description failed: %d",
                 hevc ? "HEVC" : "H.264", int(status));
        return false;
    }

    NSDictionary* outputAttrs = @{
        (id)kCVPixelBufferPixelFormatTypeKey:
            @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
    };
    VTDecompressionOutputCallbackRecord cb{decompressionCallback, m_slot};
    VTDecompressionSessionRef session = nullptr;
    status = VTDecompressionSessionCreate(
        kCFAllocatorDefault, format, nullptr,
        (__bridge CFDictionaryRef)outputAttrs, &cb, &session);
    if (status != noErr || !session) {
        qCWarning(logVTDec, "session create failed: %d", int(status));
        CFRelease(format);
        return false;
    }
    m_session = session;
    // CMVideoFormatDescriptionRef is a const CF type; the opaque void*
    // member sheds the qualifier, restored at every use/release site.
    m_format = const_cast<void*>(static_cast<const void*>(format));
    m_vps = vps;
    m_sps = sps;
    m_pps = pps;
    return true;
}

VideoDecoder::Result MacVTDecoder::decode(const QByteArray& au, QVideoFrame& out) {
    if (au.isEmpty()) return Result::Error;

    // Collect parameter sets + slice NALs from the AU. The two codecs
    // differ only in how a NAL announces its type: H.264 packs it into
    // the low 5 bits of a one-byte header, HEVC into bits 1..6 of a
    // two-byte one.
    const bool hevc = m_kind == VideoCodecKind::H265;
    QByteArray vps = m_vps, sps = m_sps, pps = m_pps;
    QByteArray avcc;   // 4-byte-length-prefixed slice data
    for (const Nal& nal : splitAnnexB(au)) {
        if (nal.size <= 0) continue;
        const uint8_t type = hevc ? uint8_t((nal.data[0] >> 1) & 0x3F)
                                  : uint8_t(nal.data[0] & 0x1F);
        auto keep = [&nal]() {
            return QByteArray(reinterpret_cast<const char*>(nal.data), nal.size);
        };
        bool isVcl = false;
        if (hevc) {
            if (nal.size < 2) continue;         // truncated HEVC NAL header
            if (type == 32)      vps = keep();
            else if (type == 33) sps = keep();
            else if (type == 34) pps = keep();
            else isVcl = type <= 31;            // VCL NAL types are 0..31
        } else {
            if (type == 7)      sps = keep();
            else if (type == 8) pps = keep();
            else isVcl = (type == 5 || type == 1);
        }
        if (isVcl) {
            const uint32_t len = CFSwapInt32HostToBig(uint32_t(nal.size));
            avcc.append(reinterpret_cast<const char*>(&len), 4);
            avcc.append(reinterpret_cast<const char*>(nal.data), nal.size);
        }
    }
    // Missing parameter sets is not an error, it is a mid-GOP start:
    // consume and wait for the keyframe that carries them. (For HEVC
    // "missing" includes the VPS, which VideoToolbox requires.)
    if (sps.isEmpty() || pps.isEmpty() || (hevc && vps.isEmpty()))
        return Result::NeedMore;
    if (!ensureSession(vps, sps, pps)) return Result::Error;
    if (avcc.isEmpty()) return Result::NeedMore;   // parameter-set-only AU

    // Wrap the AVCC data in a sample buffer.
    CMBlockBufferRef block = nullptr;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault, nullptr, size_t(avcc.size()), kCFAllocatorDefault,
        nullptr, 0, size_t(avcc.size()), 0, &block);
    if (status != noErr) return Result::Error;
    CMBlockBufferReplaceDataBytes(avcc.constData(), block, 0, size_t(avcc.size()));

    CMSampleBufferRef sample = nullptr;
    const size_t sampleSize = size_t(avcc.size());
    status = CMSampleBufferCreateReady(
        kCFAllocatorDefault, block, (CMVideoFormatDescriptionRef)m_format,
        1, 0, nullptr, 1, &sampleSize, &sample);
    CFRelease(block);
    if (status != noErr || !sample) return Result::Error;

    m_slot->valid = false;
    status = VTDecompressionSessionDecodeFrame(
        (VTDecompressionSessionRef)m_session, sample,
        0 /* synchronous */, nullptr, nullptr);
    CFRelease(sample);
    if (status != noErr) {
        qCWarning(logVTDec, "decode failed: %d", int(status));
        return Result::Error;
    }
    if (!m_slot->valid) return Result::NeedMore;
    out = m_slot->frame;
    return Result::Ok;
}

void MacVTDecoder::reset() {
    destroy();
    // Session rebuilds from the next keyframe's parameter sets.
}
