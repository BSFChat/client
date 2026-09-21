#include "voice/video/VideoSendPipeline.h"

#include "voice/video/FrameConverter.h"
#include "voice/video/VideoEncoder.h"

#include <QElapsedTimer>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(logVideoSend, "bsfchat.video.send", QtWarningMsg)

VideoSendPipeline::VideoSendPipeline(VideoStreamId streamId, QObject* parent)
    : QObject(parent)
    , m_streamId(streamId)
{
    static const int s_meta = [] {
        qRegisterMetaType<EncodedFrame>("EncodedFrame");
        return 0;
    }();
    Q_UNUSED(s_meta);

    m_worker.moveToThread(&m_thread);
    m_thread.setObjectName(streamId == VideoStreamId::Screen
                           ? QStringLiteral("video-enc-screen")
                           : QStringLiteral("video-enc-camera"));
    m_thread.start();
}

VideoSendPipeline::~VideoSendPipeline() {
    // The encoder was created on the worker thread; destroy it there
    // before the thread winds down.
    QMetaObject::invokeMethod(&m_worker, [this]() {
        m_encoder.reset();
    }, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
}

void VideoSendPipeline::configure(const EncoderConfig& config) {
    QMutexLocker lock(&m_mutex);
    m_config = config;
    m_configDirty = true;
}

void VideoSendPipeline::setBitrate(int targetKbps, int maxKbps) {
    {
        QMutexLocker lock(&m_mutex);
        m_config.targetBitrateKbps = targetKbps;
        m_config.maxBitrateKbps = maxKbps;
    }
    QMetaObject::invokeMethod(&m_worker, [this, targetKbps, maxKbps]() {
        if (m_encoder) m_encoder->setBitrate(targetKbps, maxKbps);
    }, Qt::QueuedConnection);
}

videosend::Counters VideoSendPipeline::takeCounters() {
    videosend::Counters c;
    c.submitted = m_submitted.load(std::memory_order_relaxed);
    c.superseded = m_superseded.load(std::memory_order_relaxed);
    c.encoded = m_encoded.load(std::memory_order_relaxed);
    c.bytes = m_bytes.load(std::memory_order_relaxed);
    c.packets = m_packets.load(std::memory_order_relaxed);
    c.keyframes = m_keyframes.load(std::memory_order_relaxed);
    c.workUs = m_workUs.load(std::memory_order_relaxed);
    c.maxWorkUs = m_maxWorkUs.exchange(0, std::memory_order_relaxed);
    return c;
}

void VideoSendPipeline::submitFrame(const QVideoFrame& frame, qint64 captureTimeUs) {
    m_submitted.fetch_add(1, std::memory_order_relaxed);
    {
        QMutexLocker lock(&m_mutex);
        // Latest-wins: an unprocessed older frame is simply replaced —
        // and counted, because a replaced frame is a frame the encoder
        // could not keep up with.
        if (m_pendingFrame.isValid())
            m_superseded.fetch_add(1, std::memory_order_relaxed);
        m_pendingFrame = frame;
        m_pendingTimeUs = captureTimeUs;
    }
    // Coalesce wakeups — one queued call drains one pending slot, so a
    // second invoke while one is in flight would only encode the same
    // frame twice. The flag is cleared by the worker INSIDE the same
    // critical section that takes the frame (see processPending), which
    // is what makes this exchange safe: see the note there.
    if (!m_processQueued.exchange(true)) {
        QMetaObject::invokeMethod(&m_worker, [this]() { processPending(); },
                                  Qt::QueuedConnection);
    }
}

void VideoSendPipeline::processPending() {
    QVideoFrame frame;
    qint64 tsUs = 0;
    EncoderConfig config;
    bool configDirty = false;
    {
        QMutexLocker lock(&m_mutex);
        frame = std::move(m_pendingFrame);
        m_pendingFrame = QVideoFrame();
        tsUs = m_pendingTimeUs;
        config = m_config;
        configDirty = m_configDirty;
        m_configDirty = false;
        // S-16: clear the coalescing flag while still holding the lock
        // that guards the pending slot. Clearing it AFTER the unlock
        // opened a window in which submitFrame() could store a frame
        // (this worker had already taken the previous one) and then
        // find the flag still set, so it queued no wakeup and the frame
        // sat unencoded until the NEXT submit displaced it — a dropped
        // frame per race, and the last frame of a share never sent at
        // all. Ordering it inside the lock makes the two outcomes
        // either "the worker takes the new frame" or "submitFrame sees
        // a cleared flag and queues a wakeup"; the worst case is one
        // spurious wakeup that finds an invalid frame and returns.
        m_processQueued.store(false);
    }
    if (!frame.isValid()) return;
    // Convert + encode, the whole per-frame cost this worker carries.
    // Session (re)creation is included on purpose: it is paid in the
    // same thread, and a rebuild storm is exactly what the rate
    // controller's sender dwell exists to prevent.
    QElapsedTimer work;
    work.start();

    // Lossless takes the identity-I444 path at FULL capture size —
    // any scaling would be lossy, defeating the tier's whole point.
    const int maxLongEdge = qMax(config.width, config.height);
    PlanarFrame planar = config.lossless
        ? FrameConverter::toI444Identity(frame, tsUs)
        : FrameConverter::toI420(frame, maxLongEdge, tsUs);
    if (!planar.isValid()) return;

    // The session tracks the *converted* dimensions — sources change
    // size (window resize, display switch) and the encoder must follow.
    EncoderConfig want = config;
    want.width = planar.width;
    want.height = planar.height;

    if (m_sessionValid && m_sessionConfig.sameSessionAs(want)) {
        // Same session shape; a dirty config here can only mean a
        // bitrate change, which applies live.
        if (configDirty && m_encoder)
            m_encoder->setBitrate(want.targetBitrateKbps, want.maxBitrateKbps);
    } else {
        // Codec switch (AV1 lossless ↔ H.264) or a previously failed
        // session: recreate the backend so the platform (hardware)
        // encoder gets first refusal. Reusing the old instance here
        // meant reconfigure() failed and the fallback silently landed
        // on the software encoder for the rest of the share.
        if (m_encoder && (!m_sessionValid || m_sessionConfig.codec != want.codec))
            m_encoder.reset();
        if (!m_encoder) m_encoder = VideoEncoder::create(want.codec);
        if (!m_encoder) return; // no backend on this platform
        const bool ok = m_sessionValid ? m_encoder->reconfigure(want)
                                       : m_encoder->init(want);
        if (!ok) {
            // Hardware backend refused — retry software once.
            qCWarning(logVideoSend,
                     "stream %d: %s encoder rejected %dx%d@%d — retrying software",
                     int(m_streamId), m_sessionValid ? "reconfigure on" : "init of",
                     want.width, want.height, want.fps);
            m_encoder = VideoEncoder::create(want.codec, /*preferHardware=*/false);
            if (!m_encoder || !m_encoder->init(want)) {
                qCWarning(logVideoSend, "no usable encoder for stream %d",
                         int(m_streamId));
                m_encoder.reset();
                m_sessionValid = false;
                return;
            }
        }
        m_sessionConfig = want;
        m_sessionValid = true;
        m_forceKeyframe.store(true); // new session ⇒ start clean
    }

    EncodedFrame out;
    const bool kf = m_forceKeyframe.exchange(false);
    if (!m_encoder->encode(planar, kf, out)) {
        if (kf) m_forceKeyframe.store(true); // don't lose the request
        return;
    }
    out.codec = m_sessionConfig.codec;

    const quint64 us = quint64(work.nsecsElapsed() / 1000);
    m_encoded.fetch_add(1, std::memory_order_relaxed);
    m_bytes.fetch_add(quint64(out.data.size()), std::memory_order_relaxed);
    m_packets.fetch_add(videosend::packetsFor(out.data.size()),
                        std::memory_order_relaxed);
    if (out.keyframe) m_keyframes.fetch_add(1, std::memory_order_relaxed);
    m_workUs.fetch_add(us, std::memory_order_relaxed);
    const quint32 us32 = quint32(qMin<quint64>(us, 0xffffffffu));
    quint32 prevMax = m_maxWorkUs.load(std::memory_order_relaxed);
    while (us32 > prevMax
           && !m_maxWorkUs.compare_exchange_weak(prevMax, us32,
                                                 std::memory_order_relaxed)) {}

    emit encodedFrameReady(int(m_streamId), out);
}
