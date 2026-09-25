#include "voice/video/MediaCodecDecoder.h"

#include "voice/video/MediaCodecAnnexB.h"

#include <QLoggingCategory>
#include <QVideoFrameFormat>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaError.h>
#include <media/NdkMediaFormat.h>

#include <cstring>

Q_LOGGING_CATEGORY(logMCDec, "bsfchat.video.mediacodec", QtWarningMsg)

namespace {

constexpr const char* kMimeH264 = "video/avc";

// Size handed to configure() before the first stream has been seen.
//
// It is a HINT and nothing more. The authoritative geometry is in the
// SPS, which goes to the codec as csd-0 in the same format, and the
// codec reports what it actually decoded through
// INFO_OUTPUT_FORMAT_CHANGED — which is where every dimension used to
// build a QVideoFrame below comes from. A decoder configured with the
// wrong hint and a correct csd-0 is the normal case for adaptive
// streaming and is handled by the platform, not by us.
constexpr int kHintWidth = 1280;
constexpr int kHintHeight = 720;

constexpr int64_t kInputWaitUs = 4000;
constexpr int64_t kOutputWaitUs = 8000;

bool isHardError(ssize_t ret) {
    return ret < 0 && ret != AMEDIACODEC_INFO_TRY_AGAIN_LATER
        && ret != AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED
        && ret != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED;
}

} // namespace

bool MediaCodecDecoder::h264DecodeSupported() {
    static const bool supported = []() -> bool {
        AMediaCodec* probe = AMediaCodec_createDecoderByType(kMimeH264);
        const bool ok = probe != nullptr;
        if (probe) AMediaCodec_delete(probe);
        qCInfo(logMCDec, "H.264 decode %s on this device",
              ok ? "available" : "UNAVAILABLE");
        return ok;
    }();
    return supported;
}

MediaCodecDecoder::~MediaCodecDecoder() {
    destroy();
}

void MediaCodecDecoder::destroy() {
    if (m_codec) {
        auto* codec = static_cast<AMediaCodec*>(m_codec);
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
        m_codec = nullptr;
    }
    m_ready.clear();
    m_sps.clear();
    m_pps.clear();
    m_outputLayout = {};
    // m_outWidth/m_outHeight deliberately survive: they are the size
    // hint for the next ensureSession(), and after a loss-driven reset
    // the stream almost always comes back at the size it left at.
}

bool MediaCodecDecoder::init(VideoCodecKind kind) {
    destroy();
    // H.264 only. This build never advertises h265 from Android — see
    // VideoDecoder::h265DecodeSupported() in VideoCodecFactory.cpp —
    // so the receive pipeline will not ask for one, and refusing here
    // is what makes that true rather than merely likely.
    if (kind != VideoCodecKind::H264) return false;
    // Session is built lazily from the first parameter sets, exactly as
    // MacVTDecoder does: until an SPS arrives there is nothing to
    // configure a codec with.
    return true;
}

bool MediaCodecDecoder::ensureSession(const QByteArray& sps,
                                      const QByteArray& pps) {
    if (m_codec && sps == m_sps && pps == m_pps) return true;

    if (m_codec) {
        auto* old = static_cast<AMediaCodec*>(m_codec);
        AMediaCodec_stop(old);
        AMediaCodec_delete(old);
        m_codec = nullptr;
        m_ready.clear();
        m_outputLayout = {};
    }

    AMediaCodec* codec = AMediaCodec_createDecoderByType(kMimeH264);
    if (!codec) {
        qCWarning(logMCDec, "createDecoderByType(%s) failed", kMimeH264);
        return false;
    }

    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, kMimeH264);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH,
                          m_outWidth > 0 ? m_outWidth : kHintWidth);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT,
                          m_outHeight > 0 ? m_outHeight : kHintHeight);
    // Ask for a byte-buffer output in a layout we can read. Passing no
    // surface is what makes this a buffer decoder at all; the color
    // format request is advisory and the real answer comes back through
    // the output format, same as on the encode side.
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                          mediacodec::kFormatYUV420Flexible);
    // csd-0 = SPS, csd-1 = PPS, each Annex-B with its start code. This
    // is the form MediaExtractor produces and the form every AVC decoder
    // expects. Supplying them here rather than relying on the in-band
    // copies means the codec knows the real geometry before it starts,
    // which is what lets the width/height above be a mere hint.
    AMediaFormat_setBuffer(fmt, AMEDIAFORMAT_KEY_CSD_0,
                           sps.constData(), size_t(sps.size()));
    AMediaFormat_setBuffer(fmt, AMEDIAFORMAT_KEY_CSD_1,
                           pps.constData(), size_t(pps.size()));

    const media_status_t st = AMediaCodec_configure(
        codec, fmt, /*surface=*/nullptr, /*crypto=*/nullptr, /*flags=*/0);
    AMediaFormat_delete(fmt);
    if (st != AMEDIA_OK) {
        qCWarning(logMCDec, "configure failed: %d", int(st));
        AMediaCodec_delete(codec);
        return false;
    }
    if (AMediaCodec_start(codec) != AMEDIA_OK) {
        qCWarning(logMCDec, "start failed");
        AMediaCodec_delete(codec);
        return false;
    }

    m_codec = codec;
    m_sps = sps;
    m_pps = pps;
    qCInfo(logMCDec, "MediaCodec H.264 decoder up (SPS %d B, PPS %d B)",
          int(sps.size()), int(pps.size()));
    return true;
}

bool MediaCodecDecoder::drainOutput(int64_t blockUs) {
    auto* codec = static_cast<AMediaCodec*>(m_codec);
    int64_t wait = blockUs;
    for (;;) {
        AMediaCodecBufferInfo info{};
        const ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec, &info, wait);
        wait = 0;
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return true;
        if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* out = AMediaCodec_getOutputFormat(codec);
            if (!out) continue;
            int32_t cf = 0, w = 0, h = 0, stride = 0, slice = 0;
            AMediaFormat_getInt32(out, AMEDIAFORMAT_KEY_COLOR_FORMAT, &cf);
            AMediaFormat_getInt32(out, AMEDIAFORMAT_KEY_WIDTH, &w);
            AMediaFormat_getInt32(out, AMEDIAFORMAT_KEY_HEIGHT, &h);
            AMediaFormat_getInt32(out, AMEDIAFORMAT_KEY_STRIDE, &stride);
            AMediaFormat_getInt32(out, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &slice);
            // The crop rectangle is the difference between the buffer the
            // hardware allocated and the picture inside it. Decoders
            // routinely round 1080 up to 1088, and taking `height`
            // literally is the single most common way to get a band of
            // garbage across the bottom of a 1080p stream. The rect is
            // INCLUSIVE at both ends, hence the +1.
            int32_t cl = 0, ct = 0, cr = 0, cb = 0;
            if (AMediaFormat_getRect(out, AMEDIAFORMAT_KEY_DISPLAY_CROP,
                                     &cl, &ct, &cr, &cb)
                && cr >= cl && cb >= ct) {
                // A non-zero origin would mean the picture does not start
                // at the buffer's first pixel. No decoder in practice
                // reports one, and honouring it means an offset into
                // every plane at a different rate for luma and chroma —
                // so it is logged and ignored rather than half-handled.
                if (cl != 0 || ct != 0) {
                    qCWarning(logMCDec,
                             "crop origin (%d,%d) is not 0,0 — picture may be "
                             "shifted; report this with the device model",
                             int(cl), int(ct));
                }
                w = cr - cl + 1;
                h = cb - ct + 1;
            }
            m_outWidth = w;
            m_outHeight = h;
            m_outputLayout = mediacodec::layoutFor(cf, stride, slice, w, h);
            qCInfo(logMCDec,
                  "output format: 0x%x %dx%d stride=%d slice=%d -> %s",
                  unsigned(cf), w, h, stride, slice,
                  m_outputLayout.isValid()
                      ? (m_outputLayout.plane
                             == mediacodec::BufferLayout::Plane::NV12
                             ? "NV12" : "I420")
                      : "UNUSABLE");
            AMediaFormat_delete(out);
            continue;
        }
        if (isHardError(idx)) {
            qCWarning(logMCDec, "dequeueOutputBuffer failed: %d", int(idx));
            return false;
        }

        // A real output buffer. Anything before the first
        // INFO_OUTPUT_FORMAT_CHANGED has no known layout — that
        // cannot happen per the API's ordering, but reading a buffer
        // whose shape we are guessing at is not a risk worth taking.
        if (info.size > 0 && m_outputLayout.isValid()) {
            size_t capacity = 0;
            const uint8_t* src =
                AMediaCodec_getOutputBuffer(codec, size_t(idx), &capacity);
            if (src && size_t(info.offset) + size_t(info.size) <= capacity) {
                QVideoFrameFormat vfmt(QSize(m_outWidth, m_outHeight),
                                       QVideoFrameFormat::Format_NV12);
                QVideoFrame frame(vfmt);
                if (frame.map(QVideoFrame::WriteOnly)) {
                    const bool ok = mediacodec::unpackOutputToNV12(
                        m_outputLayout, src + info.offset, size_t(info.size),
                        frame.bits(0), frame.bytesPerLine(0),
                        frame.bits(1), frame.bytesPerLine(1),
                        m_outWidth, m_outHeight);
                    frame.unmap();
                    if (ok) m_ready.push_back(frame);
                }
            }
        }
        AMediaCodec_releaseOutputBuffer(codec, size_t(idx), /*render=*/false);
    }
}

VideoDecoder::Result MediaCodecDecoder::decode(const QByteArray& au,
                                               QVideoFrame& out) {
    if (au.isEmpty()) return Result::Error;

    // Parameter sets first — they are what a session is built from, and
    // a change in them is a mid-stream resolution shift.
    QByteArray sps, pps;
    if (annexb::parameterSets(au, sps, pps)) {
        if (!ensureSession(sps, pps)) return Result::Error;
    } else if (!m_codec) {
        // No session and no parameter sets: this is a mid-GOP start, not
        // an error. Consume it and wait for the keyframe that carries
        // them — exactly the MacVTDecoder rule. Returning Error here
        // would make VideoReceivePipeline flush and beg for an IDR on
        // every single P-frame of the join window.
        return Result::NeedMore;
    }

    auto* codec = static_cast<AMediaCodec*>(m_codec);
    const annexb::Summary sum = annexb::scan(au);
    if (sum.hasSlice) {
        const ssize_t idx = AMediaCodec_dequeueInputBuffer(codec, kInputWaitUs);
        if (idx < 0) {
            if (isHardError(idx)) return Result::Error;
            // No input slot free. Draining below may open one for the
            // next access unit; this one is lost, and the pipeline's
            // loss handling is what that is for.
            qCWarning(logMCDec, "no input buffer — dropping access unit");
        } else {
            size_t capacity = 0;
            uint8_t* dst =
                AMediaCodec_getInputBuffer(codec, size_t(idx), &capacity);
            if (!dst || capacity < size_t(au.size())) {
                AMediaCodec_queueInputBuffer(codec, size_t(idx), 0, 0, 0, 0);
                return Result::Error;
            }
            memcpy(dst, au.constData(), size_t(au.size()));
            // MediaCodec wants a monotonically increasing presentation
            // timestamp and this layer has none to give — the RTP
            // timestamp never reaches VideoDecoder::decode(). The
            // playout buffer downstream schedules on its own clock
            // (VideoPlayoutBuffer), so the value here only has to not
            // confuse the codec's reordering, and with no B-frames in
            // this mesh there is no reordering to confuse.
            //
            // A PER-INSTANCE counter, not a function-local static: there
            // is one decoder per remote peer per stream and each runs on
            // its own VideoReceivePipeline worker thread, so a shared
            // static would be an unsynchronised read-modify-write across
            // threads for no benefit — the codec only cares that ITS
            // own input timestamps increase.
            m_ptsUs += 1000;
            if (AMediaCodec_queueInputBuffer(codec, size_t(idx), 0,
                                             size_t(au.size()), m_ptsUs, 0)
                != AMEDIA_OK) {
                return Result::Error;
            }
        }
    }

    if (!drainOutput(m_ready.empty() ? kOutputWaitUs : 0)) {
        // A hard codec error. Result::Error is already the right answer:
        // VideoReceivePipeline calls reset() (which tears the dead codec
        // down), arms its keyframe gate and asks the sender for an IDR,
        // and the next keyframe's parameter sets rebuild the session.
        //
        // It does NOT reach VoiceEngine::onDecoderUnavailable — that
        // edge is raised by a failed VideoDecoder::create(), not by a
        // failed decode — so an app whose codec was reclaimed while
        // backgrounded cannot latch a codec as broken for the rest of
        // the process. Same distinction as on iOS, load-bearing for the
        // same reason, and equally invisible from here.
        return Result::Error;
    }

    if (m_ready.empty()) return Result::NeedMore;
    out = m_ready.front();
    m_ready.pop_front();
    return Result::Ok;
}

void MediaCodecDecoder::reset() {
    destroy();
    // Session rebuilds from the next keyframe's parameter sets.
}
