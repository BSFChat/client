#include "voice/video/MacVTEncoder.h"

#include "voice/video/NV12Pack.h"

#include <QLoggingCategory>

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <TargetConditionals.h>
#import <VideoToolbox/VideoToolbox.h>

Q_LOGGING_CATEGORY(logVTEnc, "bsfchat.video.vt", QtWarningMsg)

// kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder is
// macOS-only in practice and iOS 17.4+ in the SDK, while this app's iOS
// deployment target is 17 — so referencing it at all on iOS is a
// weak-linking hazard for a key that would be meaningless anyway: on
// iOS the hardware encoder is the ONLY encoder. There is nothing to ask
// for and nothing to fall back to.
//
// Gated rather than availability-guarded on purpose. An @available check
// would compile, and would then ask a question whose only possible
// answers are "yes, obviously" and "the SDK is too old to ask" — a
// runtime branch that can never change the outcome.
#if TARGET_OS_OSX
#define BSFCHAT_VT_WANT_HW_SPEC 1
#else
#define BSFCHAT_VT_WANT_HW_SPEC 0
#endif

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
#if BSFCHAT_VT_WANT_HW_SPEC
        NSDictionary* spec = @{
            (id)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @YES,
        };
#else
        // iOS: no specification at all. The probe still runs — an
        // iPhone older than the A10 media engine genuinely cannot encode
        // HEVC, and this is the question that separates those from the
        // ones that can.
        NSDictionary* spec = nil;
#endif
        OSStatus status = VTCopySupportedPropertyDictionaryForEncoder(
            1280, 720, kCMVideoCodecType_HEVC,
            (__bridge CFDictionaryRef)spec, &encoderId, &props);
        const bool ok = (status == noErr);
        if (encoderId) CFRelease(encoderId);
        if (props) CFRelease(props);
        qCInfo(logVTEnc, "HEVC encode %s on this device (status %d)",
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
#if BSFCHAT_VT_WANT_HW_SPEC
    NSDictionary* encoderSpec = @{
        (id)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @YES,
    };
#else
    NSDictionary* encoderSpec = nil;
#endif
    // Bi-planar NV12, IOSurface-backed. Both halves matter and neither
    // is a micro-optimisation:
    //
    //   * NV12 is what every Apple video encoder actually consumes. The
    //     tri-planar kCVPixelFormatType_420YpCbCr8Planar this used to ask
    //     for makes VideoToolbox insert its own conversion ahead of the
    //     media engine on macOS, and on iOS the format may be refused
    //     outright — a session that creates and then fails every frame.
    //   * kCVPixelBufferIOSurfacePropertiesKey is what lets the buffer be
    //     handed to the hardware without a copy. Without it the encoder
    //     gets a malloc'd CPU buffer it has to stage into an IOSurface
    //     itself, once per frame, at full frame size.
    //
    // Declaring them here (rather than only on the buffers we allocate)
    // is also what makes VTCompressionSessionGetPixelBufferPool return a
    // pool of the right shape — see acquirePixelBuffer().
    NSDictionary* sourceAttrs = @{
        (id)kCVPixelBufferPixelFormatTypeKey:
            @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (id)kCVPixelBufferWidthKey: @(config.width),
        (id)kCVPixelBufferHeightKey: @(config.height),
        (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
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

void* MacVTEncoder::acquirePixelBuffer(const PlanarFrame& in) {
    auto session = (VTCompressionSessionRef)m_session;
    CVPixelBufferRef pixelBuffer = nullptr;

    // The session's own pool first. It is created from the source
    // attributes above, so its buffers are already NV12, already the
    // right size, and already IOSurface-backed — and recycled, which is
    // the point: the old code malloc'd and freed a full frame buffer
    // thirty times a second for the length of a call.
    CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(session);
    if (pool) {
        if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool,
                                               &pixelBuffer) != kCVReturnSuccess)
            pixelBuffer = nullptr;
    }
    if (!pixelBuffer) {
        // No pool (the session has not finished preparing, or the
        // platform declined to provide one). Allocate a compatible
        // buffer by hand rather than failing the frame — same format and
        // the same IOSurface backing, just without the recycling.
        NSDictionary* attrs = @{
            (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
        };
        if (CVPixelBufferCreate(kCFAllocatorDefault, size_t(in.width),
                                size_t(in.height),
                                kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                                (__bridge CFDictionaryRef)attrs,
                                &pixelBuffer) != kCVReturnSuccess)
            return nullptr;
    }

    if (CVPixelBufferLockBaseAddress(pixelBuffer, 0) != kCVReturnSuccess) {
        CVPixelBufferRelease(pixelBuffer);
        return nullptr;
    }
    const bool ok = nv12::packFromI420(
        reinterpret_cast<const uint8_t*>(in.y.constData()), in.strideY,
        reinterpret_cast<const uint8_t*>(in.u.constData()), in.strideU,
        reinterpret_cast<const uint8_t*>(in.v.constData()), in.strideV,
        static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0)),
        int(CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0)),
        static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1)),
        int(CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1)),
        in.width, in.height);
    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
    if (!ok) {
        CVPixelBufferRelease(pixelBuffer);
        return nullptr;
    }
    return pixelBuffer;
}

bool MacVTEncoder::encodeAttempt(const PlanarFrame& in, bool forceKeyframe,
                                 EncodedFrame& out, int* outStatus) {
    *outStatus = noErr;
    auto session = (VTCompressionSessionRef)m_session;

    CVPixelBufferRef pixelBuffer = (CVPixelBufferRef)acquirePixelBuffer(in);
    if (!pixelBuffer) return false;

    NSDictionary* frameProps = forceKeyframe
        ? @{(id)kVTEncodeFrameOptionKey_ForceKeyFrame: @YES}
        : nil;
    const CMTime pts = CMTimeMake(in.captureTimeUs, 1000000);
    m_slot->valid = false;
    OSStatus status = VTCompressionSessionEncodeFrame(
        session, pixelBuffer, pts, kCMTimeInvalid,
        (__bridge CFDictionaryRef)frameProps, nullptr, nullptr);
    CVPixelBufferRelease(pixelBuffer);
    *outStatus = int(status);
    if (status != noErr) {
        qCWarning(logVTEnc, "EncodeFrame failed: %d", int(status));
        return false;
    }
    // Force synchronous completion so m_slot is filled before we read.
    //
    // This drains the hardware pipeline once per frame, which is
    // wasteful — the media engine could be working on frame N+1 while we
    // packetise N. Removing it means making VideoEncoder::encode()
    // asynchronous for every backend on every platform, which is a
    // different change from this one; left as measured work rather than
    // done blind. At 30 fps the drain costs a few milliseconds of a
    // 33 ms budget on an A18, so it is a throughput ceiling and not a
    // correctness problem.
    status = VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    if (status != noErr) {
        *outStatus = int(status);
        return false;
    }

    if (!m_slot->valid) return false;
    out.data = m_slot->annexB;
    out.keyframe = m_slot->keyframe;
    out.captureTimeUs = in.captureTimeUs;
    out.width = in.width;
    out.height = in.height;
    return true;
}

bool MacVTEncoder::encode(const PlanarFrame& in, bool forceKeyframe,
                          EncodedFrame& out) {
    if (!m_session || !in.isValid() || in.layout != PlanarFrame::Layout::I420)
        return false;
    if (in.width != m_config.width || in.height != m_config.height)
        return false;

    int status = noErr;
    if (encodeAttempt(in, forceKeyframe, out, &status)) return true;

    // iOS tears the compression session down when the app is
    // backgrounded — the media engine is a shared, foreground-only
    // resource — and from then on every call returns
    // kVTInvalidSessionErr. Nothing upstream can see that: the pipeline
    // only learns that encode() returned false, which it treats as a bad
    // frame and retries with the next one, forever. The user comes back
    // to the app and their camera light is on and nobody can see them.
    //
    // Rebuild and retry once, here, where the status code is still in
    // hand. The retry forces a keyframe whatever the caller asked for,
    // because the new session's first frame references nothing the
    // receiver holds.
    //
    // One retry, not a loop: if a fresh session also fails, the failure
    // is not the session, and the pipeline's own software-fallback path
    // is the right next move.
    if (status != kVTInvalidSessionErr) return false;
    qCWarning(logVTEnc, "session invalidated (backgrounded?) — rebuilding");
    // By value: init() assigns to m_config, and passing the member by
    // const reference would make that a self-assignment through an alias.
    const EncoderConfig cfg = m_config;
    if (!init(cfg)) return false;
    return encodeAttempt(in, /*forceKeyframe=*/true, out, &status);
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
