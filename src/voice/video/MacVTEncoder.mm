#include "voice/video/MacVTEncoder.h"

#include <QLoggingCategory>

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

Q_LOGGING_CATEGORY(logVTEnc, "bsfchat.video.vt", QtWarningMsg)

// Per-encode output slot. VTCompressionSessionEncodeFrame +
// VTCompressionSessionCompleteFrames gives strictly synchronous
// one-in-one-out behavior, so the callback fills this before
// encode() returns — no cross-thread hand-off needed.
struct MacVTEncoder::CallbackSlot {
    QByteArray annexB;
    bool keyframe = false;
    bool valid = false;
};

namespace {

// AVCC sample buffer → Annex-B access unit. Keyframes get SPS/PPS
// prepended from the format description so any AU a receiver joins
// on is self-contained (matches openh264's output shape).
bool sampleToAnnexB(CMSampleBufferRef sample, QByteArray& out, bool keyframe) {
    static const char kStartCode[4] = {0, 0, 0, 1};
    out.clear();

    if (keyframe) {
        CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sample);
        if (!fmt) return false;
        size_t paramCount = 0;
        CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            fmt, 0, nullptr, nullptr, &paramCount, nullptr);
        for (size_t i = 0; i < paramCount; ++i) {
            const uint8_t* ps = nullptr;
            size_t psSize = 0;
            if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    fmt, i, &ps, &psSize, nullptr, nullptr) != noErr)
                return false;
            out.append(kStartCode, 4);
            out.append(reinterpret_cast<const char*>(ps), int(psSize));
        }
    }

    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    if (!block) return false;
    size_t totalLen = 0;
    char* dataPtr = nullptr;
    if (CMBlockBufferGetDataPointer(block, 0, nullptr, &totalLen, &dataPtr) != noErr)
        return false;

    // Walk 4-byte-length-prefixed NAL units, emitting start codes.
    size_t offset = 0;
    while (offset + 4 <= totalLen) {
        uint32_t nalLen = 0;
        memcpy(&nalLen, dataPtr + offset, 4);
        nalLen = CFSwapInt32BigToHost(nalLen);
        offset += 4;
        if (nalLen == 0 || offset + nalLen > totalLen) return false;
        out.append(kStartCode, 4);
        out.append(dataPtr + offset, int(nalLen));
        offset += nalLen;
    }
    return !out.isEmpty();
}

void compressionCallback(void* refcon, void* /*frameRefcon*/, OSStatus status,
                         VTEncodeInfoFlags flags, CMSampleBufferRef sample) {
    auto* slot = static_cast<MacVTEncoder::CallbackSlot*>(refcon);
    slot->valid = false;
    if (status != noErr || !sample) {
        qCWarning(logVTEnc, "compression callback status=%d", int(status));
        return;
    }
    if (flags & kVTEncodeInfo_FrameDropped) return;

    bool keyframe = true;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        auto dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        // Absent NotSync key ⇒ sync sample (keyframe).
        keyframe = !CFDictionaryContainsKey(dict, kCMSampleAttachmentKey_NotSync);
    }
    slot->keyframe = keyframe;
    slot->valid = sampleToAnnexB(sample, slot->annexB, keyframe);
}

} // namespace

MacVTEncoder::~MacVTEncoder() {
    destroy();
    delete m_slot;
}

void MacVTEncoder::destroy() {
    if (m_session) {
        auto session = (VTCompressionSessionRef)m_session;
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        m_session = nullptr;
    }
}

bool MacVTEncoder::init(const EncoderConfig& config) {
    destroy();
    if (config.codec != VideoCodecKind::H264) return false;
    if (!m_slot) m_slot = new CallbackSlot;

    VTCompressionSessionRef session = nullptr;
    NSDictionary* encoderSpec = @{
        (id)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @YES,
    };
    NSDictionary* sourceAttrs = @{
        (id)kCVPixelBufferPixelFormatTypeKey:
            @(kCVPixelFormatType_420YpCbCr8Planar),
        (id)kCVPixelBufferWidthKey: @(config.width),
        (id)kCVPixelBufferHeightKey: @(config.height),
    };
    OSStatus status = VTCompressionSessionCreate(
        kCFAllocatorDefault, config.width, config.height,
        kCMVideoCodecType_H264, (__bridge CFDictionaryRef)encoderSpec,
        (__bridge CFDictionaryRef)sourceAttrs, kCFAllocatorDefault,
        compressionCallback, m_slot, &session);
    if (status != noErr || !session) {
        qCWarning(logVTEnc, "VTCompressionSessionCreate failed: %d", int(status));
        return false;
    }
    m_session = session;

    auto setProp = [session](CFStringRef key, CFTypeRef value) {
        VTSessionSetProperty(session, key, value);
    };
    setProp(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    // No B-frames: flat latency, and keeps the bitstream inside what
    // openh264 receivers decode comfortably.
    setProp(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    setProp(kVTCompressionPropertyKey_ProfileLevel,
            config.profile == H264Profile::High
                ? kVTProfileLevel_H264_High_AutoLevel
                : kVTProfileLevel_H264_Baseline_AutoLevel);
    setProp(kVTCompressionPropertyKey_AverageBitRate,
            (__bridge CFTypeRef)@(config.targetBitrateKbps * 1000));
    setProp(kVTCompressionPropertyKey_DataRateLimits,
            (__bridge CFTypeRef)@[
                @(config.maxBitrateKbps * 1000 / 8), @(1.0) ]);
    setProp(kVTCompressionPropertyKey_MaxKeyFrameInterval,
            (__bridge CFTypeRef)@(config.fps * config.keyframeIntervalSec));
    setProp(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration,
            (__bridge CFTypeRef)@(config.keyframeIntervalSec));
    setProp(kVTCompressionPropertyKey_ExpectedFrameRate,
            (__bridge CFTypeRef)@(config.fps));
    VTCompressionSessionPrepareToEncodeFrames(session);

    m_config = config;
    qCInfo(logVTEnc, "VideoToolbox encoder up: %dx%d@%dfps %d kbps %s",
          config.width, config.height, config.fps, config.targetBitrateKbps,
          config.profile == H264Profile::High ? "high" : "baseline");
    return true;
}

bool MacVTEncoder::encode(const PlanarFrame& in, bool forceKeyframe,
                          EncodedFrame& out) {
    if (!m_session || !in.isValid() || in.layout != PlanarFrame::Layout::I420)
        return false;
    if (in.width != m_config.width || in.height != m_config.height)
        return false;
    auto session = (VTCompressionSessionRef)m_session;

    CVPixelBufferRef pixelBuffer = nullptr;
    if (CVPixelBufferCreate(kCFAllocatorDefault, size_t(in.width),
                            size_t(in.height),
                            kCVPixelFormatType_420YpCbCr8Planar, nullptr,
                            &pixelBuffer) != kCVReturnSuccess)
        return false;
    CVPixelBufferLockBaseAddress(pixelBuffer, 0);
    const uint8_t* srcPlanes[3] = {
        reinterpret_cast<const uint8_t*>(in.y.constData()),
        reinterpret_cast<const uint8_t*>(in.u.constData()),
        reinterpret_cast<const uint8_t*>(in.v.constData()),
    };
    const int srcStrides[3] = {in.strideY, in.strideU, in.strideV};
    for (int plane = 0; plane < 3; ++plane) {
        auto* dst = static_cast<uint8_t*>(
            CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, size_t(plane)));
        const size_t dstStride =
            CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, size_t(plane));
        const int planeH = plane == 0 ? in.height : in.height / 2;
        const int planeW = plane == 0 ? in.width : in.width / 2;
        for (int row = 0; row < planeH; ++row) {
            memcpy(dst + size_t(row) * dstStride,
                   srcPlanes[plane] + size_t(row) * size_t(srcStrides[plane]),
                   size_t(planeW));
        }
    }
    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);

    NSDictionary* frameProps = forceKeyframe
        ? @{(id)kVTEncodeFrameOptionKey_ForceKeyFrame: @YES}
        : nil;
    const CMTime pts = CMTimeMake(in.captureTimeUs, 1000000);
    m_slot->valid = false;
    OSStatus status = VTCompressionSessionEncodeFrame(
        session, pixelBuffer, pts, kCMTimeInvalid,
        (__bridge CFDictionaryRef)frameProps, nullptr, nullptr);
    CVPixelBufferRelease(pixelBuffer);
    if (status != noErr) {
        qCWarning(logVTEnc, "EncodeFrame failed: %d", int(status));
        return false;
    }
    // Force synchronous completion so m_slot is filled before we read.
    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);

    if (!m_slot->valid) return false;
    out.data = m_slot->annexB;
    out.keyframe = m_slot->keyframe;
    out.captureTimeUs = in.captureTimeUs;
    out.width = in.width;
    out.height = in.height;
    return true;
}

void MacVTEncoder::setBitrate(int targetKbps, int maxKbps) {
    if (!m_session) return;
    auto session = (VTCompressionSessionRef)m_session;
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AverageBitRate,
                         (__bridge CFTypeRef)@(targetKbps * 1000));
    VTSessionSetProperty(session, kVTCompressionPropertyKey_DataRateLimits,
                         (__bridge CFTypeRef)@[ @(maxKbps * 1000 / 8), @(1.0) ]);
    m_config.targetBitrateKbps = targetKbps;
    m_config.maxBitrateKbps = maxKbps;
}

bool MacVTEncoder::reconfigure(const EncoderConfig& config) {
    // Resolution changes need a fresh session; VT session creation is
    // fast (<10 ms) and the next frame is forced IDR by the pipeline.
    return init(config);
}
