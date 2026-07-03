#include "voice/video/VideoSendPipeline.h"

#include "voice/video/FrameConverter.h"
#include "voice/video/VideoEncoder.h"

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

void VideoSendPipeline::submitFrame(const QVideoFrame& frame, qint64 captureTimeUs) {
    {
        QMutexLocker lock(&m_mutex);
        // Latest-wins: an unprocessed older frame is simply replaced.
        m_pendingFrame = frame;
        m_pendingTimeUs = captureTimeUs;
    }
    // Coalesce wakeups — one queued call drains one pending slot, so a
    // second invoke while one is in flight would only encode the same
    // frame twice.
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
    }
    m_processQueued.store(false);
    if (!frame.isValid()) return;

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
        if (!m_encoder) m_encoder = VideoEncoder::create(want.codec);
        if (!m_encoder) return; // no backend on this platform
        const bool ok = m_sessionValid ? m_encoder->reconfigure(want)
                                       : m_encoder->init(want);
        if (!ok) {
            // Hardware backend refused — retry software once.
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
    emit encodedFrameReady(int(m_streamId), out);
}
