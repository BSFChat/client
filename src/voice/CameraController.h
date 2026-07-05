#pragma once

#include <QObject>
#include <QPointer>
#include <QVideoSink>
#include <QVideoFrame>
#include <QTimer>
#include <QVariantList>
#ifdef Q_OS_MACOS
class MacCameraCapturer;
#else
#include <QCamera>
#include <QMediaCaptureSession>
#endif

class VoiceEngine;
class ServerManager;
class Settings;
class VideoSendPipeline;
class VideoRateController;

// Webcam broadcaster — companion to ScreenShareController. Captures
// from QCamera, feeds a local preview sink, and (every ~200 ms)
// pushes the latest frame as a JPEG over the voice data channel
// using the 0x03 type tag (screen share uses 0x02, audio uses 0x01).
//
// Unlike QScreenCapture, QCamera works on Homebrew's Qt Multimedia
// build — no native Objective-C++ wrapper needed.
class CameraController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    // True only while frames are actually landing on ≥1 open peer
    // data channel — same semantics as ScreenShareController.
    Q_PROPERTY(bool transmitting READ transmitting NOTIFY transmittingChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QVideoSink* previewSink READ previewSink CONSTANT)
    Q_PROPERTY(QVariantList availableCameras READ availableCameras NOTIFY camerasChanged)
    Q_PROPERTY(QString cameraDescription READ cameraDescription NOTIFY cameraDescriptionChanged)

public:
    explicit CameraController(QObject* parent = nullptr);

    bool active() const { return m_active; }
    bool transmitting() const { return m_transmitting; }
    QString lastError() const { return m_lastError; }
    QVideoSink* previewSink() const { return m_sink; }
    QVariantList availableCameras() const;
    QString cameraDescription() const { return m_cameraDescription; }

    void setServerManager(ServerManager* mgr);
    void setSettings(Settings* settings) { m_settings = settings; }

    Q_INVOKABLE void start();
    Q_INVOKABLE void startForCamera(int index);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void toggle();

    // Mirror frames from our internal sink into a caller-supplied
    // VideoOutput sink (which is read-only from QML).
    Q_INVOKABLE void forwardTo(QVideoSink* sink);

signals:
    void activeChanged();
    void transmittingChanged();
    void lastErrorChanged();
    void camerasChanged();
    void cameraDescriptionChanged();

private:
    void pushFrameToPeers();
    void setTransmitting(bool transmitting);
    // Rewires the per-server "voice room changed" subscriptions
    // whenever the server list or active server changes (and at
    // initial setup), so leaving a voice channel reliably stops the
    // camera instead of letting it silently re-broadcast on the
    // next join.
    void rewireVoiceLeaveWatch();

#ifdef Q_OS_MACOS
    MacCameraCapturer* m_mac = nullptr;
#else
    QCamera* m_camera = nullptr;
    QMediaCaptureSession* m_session = nullptr;
#endif
    QVideoSink* m_sink = nullptr;
    QTimer* m_throttle = nullptr;
    ServerManager* m_servers = nullptr;
    Settings* m_settings = nullptr;
    // H.264-over-RTP encode worker + adaptive governor (vcamera
    // track), mirroring ScreenShareController's screen pair. The JPEG
    // branch survives for legacy peers / the renegotiation gap.
    VideoSendPipeline* m_pipeline = nullptr;
    VideoRateController* m_rate = nullptr;
    QPointer<QObject> m_wiredEngine;
    // Capture ticks at RTP rate; the legacy JPEG branch subsamples
    // via this counter to keep old peers at their accustomed ~5 fps.
    int m_tick = 0;
    // Per-connection voice-room subscriptions (one per server, not
    // just the active one — voice can be live on a backgrounded
    // server).
    QList<QMetaObject::Connection> m_voiceRoomConns;
    QVideoFrame m_pendingFrame;
    bool m_active = false;
    bool m_transmitting = false;
    QString m_lastError;
    QString m_cameraDescription;
};
