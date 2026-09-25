#pragma once

#include "voice/video/MediaCodecLayout.h"
#include "voice/video/VideoEncoder.h"

#include <QByteArray>

#include <deque>

// Android hardware H.264 encoder, on the NDK's AMediaCodec.
//
// =====================================================================
// NDK AMediaCodec, not JNI to android.media.MediaCodec
// =====================================================================
//
// MediaCodec has two bindings and this backend uses the C one. The
// trade-off, stated because it is a real one and the other side of it
// is not worthless:
//
//   FOR the NDK. No JNI on the encode path at all. VideoEncoder's
//   contract is that an instance is owned and driven by ONE
//   VideoSendPipeline worker thread, and that thread is a QThread this
//   app created — so a JNI binding would have to AttachCurrentThread it
//   to the VM, keep a global ref to the codec object and its
//   ByteBuffers, manage a local-ref frame per call at 30 Hz, and detach
//   on the way out. Every one of those is a way to leak or crash that
//   has nothing to do with video. The NDK binding is an opaque pointer
//   and a handful of C calls. It also keeps the codec out of the Qt
//   package source dir (no new Java class, no proguard rule, no
//   androiddeployqt surprise) and out of the class loader the app is
//   already fighting on this platform — see the QtNative commit.
//
//   AGAINST the NDK, and the reason to write it down: it has no
//   getInputImage()/getOutputImage(). In Java, COLOR_FormatYUV420Flexible
//   hands back an android.media.Image whose Plane objects carry their
//   own row and pixel strides, and the layout question answers itself.
//   The NDK gives a flat uint8_t* and nothing else, so the layout has to
//   be RECONSTRUCTED from the MediaFormat's color-format, stride and
//   slice-height keys. That is MediaCodecLayout.h, and it is the part
//   of this backend that carries real risk on a device nobody has
//   tested — which is exactly why it is a separate, pure, host-tested
//   file rather than inline here.
//
//   Nothing else in the API surface is JNI-only for this use. minSdk is
//   28 and the one function above API 21 that is needed —
//   AMediaCodec_getInputFormat, the layout readback — is API 28 on the
//   nose. AMediaCodec_setParameters (live bitrate, forced IDR) is 26.
//
// =====================================================================
// Pipeline delay, and why encode() can return false early
// =====================================================================
//
// VideoToolbox is made one-in-one-out by calling
// VTCompressionSessionCompleteFrames after every frame. MediaCodec has
// no cheap equivalent: it is a queue in and a queue out, and a hardware
// encoder legitimately holds one to three frames before the first
// access unit appears. Draining it per frame would mean flush(), which
// discards state and forces an IDR — a slideshow with extra steps.
//
// So this backend keeps the frames in flight instead. encode() queues
// its input, drains whatever output is ready into m_ready, and returns
// the OLDEST access unit. Steady state is one-in-one-out with a
// constant delay of a frame or two; only the first frames of a session
// return false, which VideoSendPipeline already handles (it re-arms a
// pending keyframe request and moves on). No frame is dropped and none
// is reordered — AllowFrameReordering has no analogue here because
// B-frames are never requested.
//
// The consequence worth knowing: the access unit encode() returns is
// generally NOT the frame it was just handed, so `out.captureTimeUs`
// comes from the codec's presentation timestamp rather than from `in`,
// and a forceKeyframe request surfaces as a keyframe a frame or two
// later than the caller asked. Both are correct; neither is what the
// Apple backend does.
class MediaCodecEncoder : public VideoEncoder {
public:
    MediaCodecEncoder() = default;
    ~MediaCodecEncoder() override;

    bool init(const EncoderConfig& config) override;
    bool encode(const PlanarFrame& in, bool forceKeyframe,
                EncodedFrame& out) override;
    void setBitrate(int targetKbps, int maxKbps) override;
    bool reconfigure(const EncoderConfig& config) override;
    // Hardware, no lossless, and NOT High profile — see
    // h264EncodeProfiles() in VideoCodecFactory.cpp for why this build
    // advertises Constrained Baseline only on the encode side.
    Caps caps() const override { return {true, false, false}; }

    // Is there an H.264 encoder on this device at all? Asked once and
    // cached. The Android CDD has required one of every device with a
    // camera since API 21, so a false here means something is very
    // wrong — but "advertise a codec we then cannot create" is the
    // failure this whole port exists to remove, so it is asked rather
    // than assumed.
    static bool h264EncodeSupported();

private:
    void destroy();
    bool openSession(const EncoderConfig& config);
    // Dequeue one input buffer from a freshly started codec purely to
    // read its real capacity, then hand it back. Returns 0 if no buffer
    // came back in time, which means "unmeasured", not "zero".
    static size_t probeInputCapacity(void* codecPtr);
    // Queue one frame; false means the input could not be handed over.
    bool queueInput(const PlanarFrame& in, bool forceKeyframe, int* outStatus);
    // Move every output buffer that is ready into m_ready. `blockUs`
    // applies only to the first dequeue attempt.
    void drainOutput(int64_t blockUs, int* outStatus);

    struct Pending {
        QByteArray data;
        bool keyframe = false;
        qint64 ptsUs = 0;
    };

    void* m_codec = nullptr;            // opaque AMediaCodec*
    EncoderConfig m_config;
    mediacodec::BufferLayout m_inputLayout;
    // What an input buffer actually measured at session open. NOT
    // derivable from m_inputLayout: a codec's reported geometry and its
    // allocation are different facts and can disagree — see
    // BufferLayout::requiredBytes().
    size_t m_inputCapacity = 0;
    // The per-frame pack refusal is logged once per session; at frame
    // rate it buries the one line that explains it.
    bool m_packRefusalLogged = false;
    // SPS+PPS as Annex-B, captured from the codec-config output buffer
    // and prepended to keyframes that do not already carry them. A
    // receiver joining mid-stream has no other source for them.
    QByteArray m_csd;
    std::deque<Pending> m_ready;
    // True once a session has produced at least one access unit, so a
    // failure is distinguishable from the normal priming window.
    bool m_primed = false;
};
