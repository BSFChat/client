#include "voice/QtAudioBackend.h"

#include <QAudioSink>
#include <QAudioSource>
#include <QIODevice>

namespace bsfchat::voice {

QtAudioBackend::QtAudioBackend(QObject* parent) : QObject(parent) {}

QtAudioBackend::~QtAudioBackend()
{
    closeCapture();
    closeRender();
}

bool QtAudioBackend::openCapture(const QAudioDevice& device,
                                 const QAudioFormat& format)
{
    closeCapture();
    if (device.isNull()) return false;

    // Parented to `this`, which lives on the audio thread, and
    // constructed here — so the source and the QIODevice it hands back
    // are both affine to the thread that will drive them.
    m_source = new QAudioSource(device, format, this);
    m_captureDevice = m_source->start();
    if (m_captureDevice) {
        // Both ends are audio-thread objects, so this is a direct
        // connection and the encode happens inline on the device
        // callback's thread — never a hop through the GUI.
        connect(m_captureDevice, &QIODevice::readyRead,
                this, &QtAudioBackend::onReadyRead);
    } else {
        qWarning("[voice] QAudioSource::start() returned null — "
                 "macOS likely still denying microphone access");
    }
    qInfo("[voice] QAudioSource initial state=%d error=%d",
          int(m_source->state()), int(m_source->error()));
    return m_captureDevice != nullptr;
}

void QtAudioBackend::closeCapture()
{
    if (!m_source) return;
    m_source->stop();
    delete m_source;
    m_source = nullptr;
    m_captureDevice = nullptr;
}

qint64 QtAudioBackend::readCapture(char* dst, qint64 maxBytes)
{
    if (!m_captureDevice || maxBytes <= 0) return 0;
    const qint64 n = m_captureDevice->read(dst, maxBytes);
    return n > 0 ? n : 0;
}

void QtAudioBackend::onReadyRead()
{
    notifyCaptureReady();
}

bool QtAudioBackend::openRender(const QAudioDevice& device,
                                const QAudioFormat& format, int bufferBytes)
{
    closeRender();
    if (device.isNull()) return false;

    m_sink = new QAudioSink(device, format, this);
    // Must be set before start(). Bounds the amount of audio the device
    // holds, and therefore the floor on output latency.
    if (bufferBytes > 0) m_sink->setBufferSize(bufferBytes);
    m_playbackDevice = m_sink->start();
    if (!m_playbackDevice) {
        qWarning("[voice] QAudioSink::start() returned null — no playback");
    }
    return m_playbackDevice != nullptr;
}

void QtAudioBackend::closeRender()
{
    if (!m_sink) return;
    m_sink->stop();
    delete m_sink;
    m_sink = nullptr;
    m_playbackDevice = nullptr;
}

qint64 QtAudioBackend::bytesFree()
{
    if (!m_sink || !m_playbackDevice) return 0;
    // The state check the pump used to do inline. A stopped sink
    // reports free space it will not actually accept.
    if (m_sink->state() == QAudio::StoppedState) return 0;
    return m_sink->bytesFree();
}

qint64 QtAudioBackend::writeRender(const char* src, qint64 bytes)
{
    if (!m_playbackDevice) return -1;
    return m_playbackDevice->write(src, bytes);
}

QString QtAudioBackend::describe() const
{
    return QStringLiteral("Qt Multimedia (QAudioSource/QAudioSink)");
}

} // namespace bsfchat::voice
