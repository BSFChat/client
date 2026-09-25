#include "voice/video/MediaCodecEncoder.h"

#include "voice/video/MediaCodecAnnexB.h"

#include <QLoggingCategory>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaError.h>
#include <media/NdkMediaFormat.h>

#include <utility>

Q_LOGGING_CATEGORY(logMCEnc, "bsfchat.video.mediacodec", QtWarningMsg)

namespace {

constexpr const char* kMimeH264 = "video/avc";

// android.media.MediaCodec parameter keys. The NDK names them
// (AMEDIACODEC_KEY_VIDEO_BITRATE, AMEDIACODEC_KEY_REQUEST_SYNC_FRAME)
// only from API 31, and referencing those symbols against a minSdk of
// 28 is a weak-linking hazard for two string constants whose VALUES are
// part of the platform's stable parameter vocabulary — "video-bitrate"
// has been MediaCodec.PARAMETER_KEY_VIDEO_BITRATE since API 19 and
// "request-sync" has been PARAMETER_KEY_REQUEST_SYNC_FRAME since 23.
// The literals are the same strings the API-31 constants expand to.
constexpr const char* kParamVideoBitrate = "video-bitrate";
constexpr const char* kParamRequestSync = "request-sync";

// MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR. Constant bitrate
// for a realtime call: the pacer and the congestion controller upstream
// both assume the encoder spends roughly what it was told to spend, and
// a VBR encoder's bursts are what turn a WiFi hiccup into a stall.
constexpr int32_t kBitrateModeCbr = 2;

// MediaCodec's flag for "this output buffer is a sync frame". The value
// is 1 and has been since MediaCodec existed (BUFFER_FLAG_SYNC_FRAME,
// renamed BUFFER_FLAG_KEY_FRAME); the NDK enumerator carries an
// "introduced in API 34" note that describes when the NDK NAMED it, not
// when the platform started setting it. Used as a hint only — the
// authoritative answer is whether the access unit actually contains an
// IDR slice, which annexb::scan() reads out of the bitstream.
constexpr uint32_t kFlagKeyFrame = 1;
constexpr uint32_t kFlagCodecConfig = AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG;

// How long encode() will wait for the codec when it has nothing ready.
// Only ever paid while the hardware pipeline is priming (the first frame
// or two of a session) or when the encoder has genuinely fallen behind:
// in steady state the output buffer is already there and the dequeue
// returns at once. Capped well inside a 30 fps frame budget so a stalled
// encoder costs a dropped frame rather than a stalled capture thread.
constexpr int64_t kOutputWaitUs = 10000;
// The input side gets less. Failing to hand a frame over is a dropped
// frame; blocking here backs up the capture pipeline behind it.
constexpr int64_t kInputWaitUs = 4000;

// Color formats to ask for, in order.
//
// Flexible first because it is the portable ask — every codec since API
// 21 accepts it at configure() time, and a device whose encoder wants
// some vendor semiplanar variant will resolve to it and say so. The two
// concrete formats behind it are the safety net for a codec that
// accepts Flexible and then declines to report what it resolved to, in
// which case MediaCodecLayout refuses to guess and we ask again for
// something unambiguous.
constexpr int32_t kColorFormatLadder[] = {
    mediacodec::kFormatYUV420Flexible,
    mediacodec::kFormatYUV420SemiPlanar,
    mediacodec::kFormatYUV420Planar,
};

// A dequeue return that is negative but not one of the three INFO_*
// sentinels is a media_status_t: the codec is in an error state.
bool isHardError(ssize_t ret) {
    return ret < 0 && ret != AMEDIACODEC_INFO_TRY_AGAIN_LATER
        && ret != AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED
        && ret != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED;
}

} // namespace

bool MediaCodecEncoder::h264EncodeSupported() {
    static const bool supported = []() -> bool {
        AMediaCodec* probe = AMediaCodec_createEncoderByType(kMimeH264);
        const bool ok = probe != nullptr;
        if (probe) AMediaCodec_delete(probe);
        qCInfo(logMCEnc, "H.264 encode %s on this device",
              ok ? "available" : "UNAVAILABLE");
        return ok;
    }();
    return supported;
}

MediaCodecEncoder::~MediaCodecEncoder() {
    destroy();
}

void MediaCodecEncoder::destroy() {
    if (m_codec) {
        auto* codec = static_cast<AMediaCodec*>(m_codec);
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
        m_codec = nullptr;
    }
    m_ready.clear();
    m_csd.clear();
    m_inputLayout = {};
    m_primed = false;
}

bool MediaCodecEncoder::openSession(const EncoderConfig& config) {
    for (int32_t wanted : kColorFormatLadder) {
        AMediaCodec* codec = AMediaCodec_createEncoderByType(kMimeH264);
        if (!codec) {
            qCWarning(logMCEnc, "createEncoderByType(%s) failed", kMimeH264);
            return false;
        }

        AMediaFormat* fmt = AMediaFormat_new();
        AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, kMimeH264);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, config.width);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, config.height);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, wanted);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_BIT_RATE,
                              config.targetBitrateKbps * 1000);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_BITRATE_MODE, kBitrateModeCbr);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, config.fps);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL,
                              config.keyframeIntervalSec);
        // "Encoder, do not buffer frames to look ahead." This is what
        // keeps the pipeline delay described in the header at one or two
        // frames instead of a GOP, and it is the closest MediaCodec has
        // to VideoToolbox's RealTime + AllowFrameReordering:false pair.
        // Advisory — a codec that ignores it is still correct, just
        // deeper, which is why encode() copes with depth rather than
        // assuming none.
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_LATENCY, 1);
        //
        // AMEDIAFORMAT_KEY_PROFILE is deliberately NOT set. Requesting a
        // profile on an Android encoder needs a matching LEVEL in the
        // same format, the pair is rejected outright by encoders that do
        // not implement it, and there is no NDK capability query below
        // API 36 to ask first — so asking for High is a configure()
        // failure on an unknown share of devices, and asking for
        // Baseline is the same risk for no gain. The default is Baseline
        // or Constrained Baseline on every Android AVC encoder in
        // practice, and a device that emits High anyway is harmless:
        // every receiver in this mesh advertises `high` on its DECODE
        // side. What matters is that we do not CLAIM High — see
        // h264EncodeProfiles() in VideoCodecFactory.cpp.
        //
        // Width/height are passed as the real picture size. Encoders
        // that need 16-aligned macroblocks pad internally and report the
        // padding back as stride/slice-height, which is exactly what
        // MediaCodecLayout consumes.

        media_status_t st = AMediaCodec_configure(
            codec, fmt, /*surface=*/nullptr, /*crypto=*/nullptr,
            AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        AMediaFormat_delete(fmt);
        if (st != AMEDIA_OK) {
            qCInfo(logMCEnc, "configure with color-format 0x%x failed: %d",
                  unsigned(wanted), int(st));
            AMediaCodec_delete(codec);
            continue;
        }

        // What did it actually choose? getInputFormat is the whole
        // reason minSdk 28 matters here: without it, Flexible is a
        // question with no answer and the input buffer is bytes of
        // unknown shape.
        mediacodec::BufferLayout layout;
        if (AMediaFormat* in = AMediaCodec_getInputFormat(codec)) {
            int32_t resolved = wanted, stride = 0, slice = 0;
            AMediaFormat_getInt32(in, AMEDIAFORMAT_KEY_COLOR_FORMAT, &resolved);
            AMediaFormat_getInt32(in, AMEDIAFORMAT_KEY_STRIDE, &stride);
            AMediaFormat_getInt32(in, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &slice);
            layout = mediacodec::layoutFor(resolved, stride, slice,
                                           config.width, config.height);
            qCInfo(logMCEnc,
                  "input format: asked 0x%x got 0x%x stride=%d slice=%d -> %s",
                  unsigned(wanted), unsigned(resolved), stride, slice,
                  layout.isValid()
                      ? (layout.plane == mediacodec::BufferLayout::Plane::NV12
                             ? "NV12" : "I420")
                      : "UNUSABLE");
            AMediaFormat_delete(in);
        }
        if (!layout.isValid()) {
            AMediaCodec_delete(codec);
            continue;
        }

        st = AMediaCodec_start(codec);
        if (st != AMEDIA_OK) {
            qCWarning(logMCEnc, "start failed: %d", int(st));
            AMediaCodec_delete(codec);
            continue;
        }

        m_codec = codec;
        m_inputLayout = layout;
        m_config = config;
        qCInfo(logMCEnc,
              "MediaCodec H.264 encoder up: %dx%d@%dfps %d kbps (stride %d, "
              "slice %d)",
              config.width, config.height, config.fps, config.targetBitrateKbps,
              layout.stride, layout.sliceHeight);
        return true;
    }
    qCWarning(logMCEnc,
             "no usable color format for %dx%d — every candidate was refused "
             "or reported a layout this build cannot pack",
             config.width, config.height);
    return false;
}

bool MediaCodecEncoder::init(const EncoderConfig& config) {
    destroy();
    // H.265 is not wired up here. Android hardware HEVC encoders are
    // common but not universal, there is no NDK capability query below
    // API 36 to separate the devices that have one from the devices that
    // will fail at share time, and this codebase only advertises h265 on
    // the DECODE side anyway. Adding it means adding a probe that can be
    // trusted, which is its own piece of work.
    if (config.codec != VideoCodecKind::H264) return false;
    if (config.width <= 0 || config.height <= 0) return false;
    return openSession(config);
}

bool MediaCodecEncoder::queueInput(const PlanarFrame& in, bool forceKeyframe,
                                   int* outStatus) {
    auto* codec = static_cast<AMediaCodec*>(m_codec);

    if (forceKeyframe) {
        // The codec-level IDR request. Unlike VideoToolbox's per-frame
        // kVTEncodeFrameOptionKey_ForceKeyFrame this is not attached to
        // a particular input buffer — it asks the encoder to make the
        // next frame it starts a sync frame, so with the pipeline depth
        // described in the header the IDR may land a frame or two after
        // the caller asked. Correct either way: the receiver's keyframe
        // gate waits for it, and VideoSendPipeline only re-arms its
        // request when encode() returns nothing at all.
        AMediaFormat* params = AMediaFormat_new();
        AMediaFormat_setInt32(params, kParamRequestSync, 0);
        AMediaCodec_setParameters(codec, params);
        AMediaFormat_delete(params);
    }

    const ssize_t idx = AMediaCodec_dequeueInputBuffer(codec, kInputWaitUs);
    if (idx < 0) {
        if (isHardError(idx)) *outStatus = int(idx);
        return false;
    }

    size_t capacity = 0;
    uint8_t* dst = AMediaCodec_getInputBuffer(codec, size_t(idx), &capacity);
    if (!dst) {
        // Hand the buffer back empty rather than leaking the slot.
        AMediaCodec_queueInputBuffer(codec, size_t(idx), 0, 0, 0, 0);
        return false;
    }
    if (!mediacodec::packInput(
            m_inputLayout,
            reinterpret_cast<const uint8_t*>(in.y.constData()), in.strideY,
            reinterpret_cast<const uint8_t*>(in.u.constData()), in.strideU,
            reinterpret_cast<const uint8_t*>(in.v.constData()), in.strideV,
            dst, capacity, in.width, in.height)) {
        qCWarning(logMCEnc, "input pack refused (capacity %zu, need %zu)",
                 capacity, m_inputLayout.sizeBytes());
        AMediaCodec_queueInputBuffer(codec, size_t(idx), 0, 0, 0, 0);
        return false;
    }

    const media_status_t st = AMediaCodec_queueInputBuffer(
        codec, size_t(idx), /*offset=*/0, m_inputLayout.sizeBytes(),
        uint64_t(in.captureTimeUs), /*flags=*/0);
    if (st != AMEDIA_OK) {
        *outStatus = int(st);
        return false;
    }
    return true;
}

void MediaCodecEncoder::drainOutput(int64_t blockUs, int* outStatus) {
    auto* codec = static_cast<AMediaCodec*>(m_codec);
    int64_t wait = blockUs;
    for (;;) {
        AMediaCodecBufferInfo info{};
        const ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec, &info, wait);
        wait = 0;   // only the first attempt may block
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return;
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED
            || idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            continue;
        }
        if (isHardError(idx)) {
            qCWarning(logMCEnc, "dequeueOutputBuffer failed: %d", int(idx));
            *outStatus = int(idx);
            return;
        }

        size_t capacity = 0;
        const uint8_t* src =
            AMediaCodec_getOutputBuffer(codec, size_t(idx), &capacity);
        if (src && info.size > 0
            && size_t(info.offset) + size_t(info.size) <= capacity) {
            const char* begin =
                reinterpret_cast<const char*>(src) + info.offset;
            QByteArray au(begin, int(info.size));
            if (info.flags & kFlagCodecConfig) {
                // SPS+PPS, delivered once at the head of the stream.
                // Stashed, not emitted: on its own it is not a
                // displayable access unit, and the pipeline downstream
                // counts what it is given as frames.
                m_csd = au;
                qCInfo(logMCEnc, "codec config: %d bytes", int(au.size()));
            } else {
                const annexb::Summary sum = annexb::scan(au);
                Pending p;
                p.keyframe = (info.flags & kFlagKeyFrame) != 0 || sum.hasIdr;
                p.ptsUs = qint64(info.presentationTimeUs);
                // Make every keyframe self-contained. A receiver that
                // joins mid-call, or one that just flushed to recover
                // from loss, has no other source for the parameter sets
                // — the codec-config buffer went past before it was
                // listening. Skipped when the encoder already inlines
                // them (some do), so the AU never carries two copies.
                if (p.keyframe && !sum.hasSps && !m_csd.isEmpty()) {
                    p.data = m_csd;
                    p.data.append(au);
                } else {
                    p.data = std::move(au);
                }
                m_ready.push_back(std::move(p));
                m_primed = true;
            }
        }
        AMediaCodec_releaseOutputBuffer(codec, size_t(idx), /*render=*/false);
    }
}

bool MediaCodecEncoder::encode(const PlanarFrame& in, bool forceKeyframe,
                               EncodedFrame& out) {
    if (!m_codec || !in.isValid() || in.layout != PlanarFrame::Layout::I420)
        return false;
    if (in.width != m_config.width || in.height != m_config.height)
        return false;

    int status = AMEDIA_OK;
    // Free output slots first: a codec whose output queue is full can
    // refuse to give up an input buffer, and then nothing moves.
    drainOutput(0, &status);
    if (status == AMEDIA_OK) queueInput(in, forceKeyframe, &status);
    if (status == AMEDIA_OK)
        drainOutput(m_ready.empty() ? kOutputWaitUs : 0, &status);

    if (status != AMEDIA_OK) {
        // Android reclaims codecs. A MediaCodec instance is a handle on
        // a shared, foreground-biased hardware resource, and the
        // framework takes it back when the app is backgrounded or when a
        // higher-priority client (the camera app, a phone call) wants it
        // — after which every call on it fails for the life of the
        // instance. This is the same failure shape as iOS's
        // kVTInvalidSessionErr, and it needs the same answer, for the
        // same reason: nothing upstream can see it. VideoSendPipeline
        // only learns that encode() returned false, treats it as a bad
        // frame, and retries with the next one forever — the user comes
        // back to the app with the camera light on and nobody able to
        // see them.
        //
        // Rebuild and retry once, here, where the status is still in
        // hand. One retry and not a loop: if a fresh codec also fails,
        // the failure is not the codec, and the pipeline's own fallback
        // path is the right next move.
        qCWarning(logMCEnc, "codec error %d (reclaimed?) — rebuilding", status);
        const EncoderConfig cfg = m_config;
        destroy();
        if (!openSession(cfg)) return false;
        // The new codec's first frame references nothing the receiver
        // holds, so it must be a keyframe whatever the caller asked.
        status = AMEDIA_OK;
        if (!queueInput(in, /*forceKeyframe=*/true, &status)
            || status != AMEDIA_OK) {
            return false;
        }
        drainOutput(kOutputWaitUs, &status);
        if (status != AMEDIA_OK) return false;
    }

    if (m_ready.empty()) return false;   // pipeline still priming
    Pending p = std::move(m_ready.front());
    m_ready.pop_front();
    out.data = std::move(p.data);
    out.keyframe = p.keyframe;
    // The codec's presentation timestamp, not `in.captureTimeUs`: with a
    // frame or two in flight these are different frames, and the one
    // being emitted is the one whose capture time belongs on the wire.
    out.captureTimeUs = p.ptsUs;
    out.width = m_config.width;
    out.height = m_config.height;
    return true;
}

void MediaCodecEncoder::setBitrate(int targetKbps, int maxKbps) {
    if (!m_codec) return;
    // MediaCodec has one live knob, the target. There is no runtime
    // equivalent of VideoToolbox's DataRateLimits ceiling — the cap is
    // the bitrate mode, and this session is CBR, so the target IS the
    // ceiling in practice. maxKbps is recorded so sameSessionAs() and
    // the rate controller see a consistent config.
    AMediaFormat* params = AMediaFormat_new();
    AMediaFormat_setInt32(params, kParamVideoBitrate, targetKbps * 1000);
    AMediaCodec_setParameters(static_cast<AMediaCodec*>(m_codec), params);
    AMediaFormat_delete(params);
    m_config.targetBitrateKbps = targetKbps;
    m_config.maxBitrateKbps = maxKbps;
}

bool MediaCodecEncoder::reconfigure(const EncoderConfig& config) {
    // Resolution, frame rate and GOP are configure()-time on MediaCodec,
    // so a change means a new codec. Same answer as the VideoToolbox
    // backend, and the pipeline forces the next frame to be an IDR.
    return init(config);
}
