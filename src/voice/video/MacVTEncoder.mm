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
    // Which parameter-set API the callback must use. Set by init()
    // before any frame is submitted, read only on the encode thread.
    bool hevc = false;
};

namespace {

// AVCC/HVCC sample buffer → Annex-B access unit. Keyframes get the
// parameter sets prepended from the format description (SPS+PPS for
// H.264; VPS+SPS+PPS for HEVC) so any AU a receiver joins on is
// self-contained — a receiver that starts mid-GOP has no other source
// for them, and for HEVC the VPS is not optional.
bool sampleToAnnexB(CMSampleBufferRef sample, QByteArray& out, bool keyframe,
                    bool hevc) {
    static const char kStartCode[4] = {0, 0, 0, 1};
    out.clear();

    if (keyframe) {
        CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sample);
        if (!fmt) return false;
        size_t paramCount = 0;
        // MSVC forbids #if inside a macro argument list and this file is
        // ObjC++ anyway; a plain function pointer keeps the two API
        // families to one branch instead of two copies of the loop.
        auto getParamSet = hevc
            ? &CMVideoFormatDescriptionGetHEVCParameterSetAtIndex
            : &CMVideoFormatDescriptionGetH264ParameterSetAtIndex;
        getParamSet(fmt, 0, nullptr, nullptr, &paramCount, nullptr);
        for (size_t i = 0; i < paramCount; ++i) {
            const uint8_t* ps = nullptr;
            size_t psSize = 0;
            if (getParamSet(fmt, i, &ps, &psSize, nullptr, nullptr) != noErr)
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
    const bool hevc = slot->hevc;
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
    slot->valid = sampleToAnnexB(sample, slot->annexB, keyframe, hevc);
}

} // namespace

bool MacVTEncoder::hevcEncodeSupported() {
    // VTCopySupportedPropertyDictionaryForEncoder is the cheap, honest
    // question: it consults the registered encoder list and fails with
    // kVTCouldNotFindVideoEncoderErr when nothing can encode the codec
    // at that size. Creating a throwaway compression session would also
    // work but spins up the media engine on a cold path.
    static const bool supported = []() -> bool {
        CFStringRef encoderId = nullptr;
        CFDictionaryRef props = nullptr;
        NSDictionary* spec = @{
            (id)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @YES,
        };
        OSStatus status = VTCopySupportedPropertyDictionaryForEncoder(
            1280, 720, kCMVideoCodecType_HEVC,
            (__bridge CFDictionaryRef)spec, &encoderId, &props);
        const bool ok = (status == noErr);
        if (encoderId) CFRelease(encoderId);
        if (props) CFRelease(props);
        qCInfo(logVTEnc, "HEVC encode %s on this Mac (status %d)",
              ok ? "available" : "unavailable", int(status));
        return ok;
    }();
    return supported;
}

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
    if (config.codec != VideoCodecKind::H264 && config.codec != VideoCodecKind::H265)
        return false;
    const bool hevc = config.codec == VideoCodecKind::H265;
    if (!m_slot) m_slot = new CallbackSlot;
    m_slot->hevc = hevc;

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
        hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264,
        (__bridge CFDictionaryRef)encoderSpec,
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
    // HEVC has no baseline/high axis worth exposing here — Main is the
    // 8-bit 4:2:0 profile every hardware decoder implements, and the
    // H264Profile setting is meaningless for it.
    setProp(kVTCompressionPropertyKey_ProfileLevel,
            hevc ? kVTProfileLevel_HEVC_Main_AutoLevel
                 : (config.profile == H264Profile::High
                        ? kVTProfileLevel_H264_High_AutoLevel
                        : kVTProfileLevel_H264_Baseline_AutoLevel));
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
    qCInfo(logVTEnc, "VideoToolbox %s encoder up: %dx%d@%dfps %d kbps %s",
          hevc ? "HEVC" : "H.264",
          config.width, config.height, config.fps, config.targetBitrateKbps,
          hevc ? "main" : (config.profile == H264Profile::High ? "high" : "baseline"));
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
