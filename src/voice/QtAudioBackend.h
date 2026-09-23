#pragma once

// IAudioBackend over QAudioSource + QAudioSink: the path this client has
// always used, moved behind the seam without changing what it does.
//
// This is the backend for Windows, Linux and Android, and the fallback
// on macOS and iOS. Nothing in it is new. openCapture() is the old
// AudioWorker::openSource(), openRender() is the old openSink(), and the
// readyRead connection is the same direct connection to the same audio
// thread, so capture is still delivered by push at exactly the same
// moments it was before. If this file ever behaves differently from the
// pre-seam code, that is a bug in this file and not a design decision.
//
// A QObject because the readyRead connection needs one, and because
// QAudioSource/QAudioSink want a parent that is affine to the thread
// that drives them.

#include <QObject>
#include <QByteArray>

#include "voice/AudioBackend.h"

class QAudioSource;
class QAudioSink;
class QIODevice;

namespace bsfchat::voice {

class QtAudioBackend : public QObject, public IAudioBackend {
    Q_OBJECT
public:
    explicit QtAudioBackend(QObject* parent = nullptr);
    ~QtAudioBackend() override;

    AudioBackendKind kind() const override { return AudioBackendKind::Qt; }

    // Android's QAudioSource opens under MODE_IN_COMMUNICATION (see
    // AndroidAudioRouting.h) and the platform runs its own gain control
    // and, usually, echo cancellation on the stream. Everywhere else Qt
    // gives us the raw capture path — including iOS, where QtMultimedia
    // builds on kAudioUnitSubType_RemoteIO and there is no processing at
    // all, which is the entire reason DarwinVpioBackend exists.
    bool platformVoiceProcessing() const override
    {
#ifdef Q_OS_ANDROID
        return true;
#else
        return false;
#endif
    }

    bool capturePolled() const override { return false; }

    bool openCapture(const QAudioDevice& device,
                     const QAudioFormat& format) override;
    void closeCapture() override;
    bool captureOpen() const override { return m_captureDevice != nullptr; }
    qint64 readCapture(char* dst, qint64 maxBytes) override;

    bool openRender(const QAudioDevice& device, const QAudioFormat& format,
                    int bufferBytes) override;
    void closeRender() override;
    bool renderOpen() const override { return m_playbackDevice != nullptr; }
    qint64 bytesFree() override;
    qint64 writeRender(const char* src, qint64 bytes) override;

    QString describe() const override;

private:
    void onReadyRead();

    QAudioSource* m_source = nullptr;
    QIODevice* m_captureDevice = nullptr;
    QAudioSink* m_sink = nullptr;
    QIODevice* m_playbackDevice = nullptr;
};

} // namespace bsfchat::voice
