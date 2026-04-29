#pragma once

#include <QObject>
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
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QVideoSink* previewSink READ previewSink CONSTANT)
    Q_PROPERTY(QVariantList availableCameras READ availableCameras NOTIFY camerasChanged)
    Q_PROPERTY(QString cameraDescription READ cameraDescription NOTIFY cameraDescriptionChanged)

public:
    explicit CameraController(QObject* parent = nullptr);

    bool active() const { return m_active; }
    QString lastError() const { return m_lastError; }
    QVideoSink* previewSink() const { return m_sink; }
    QVariantList availableCameras() const;
    QString cameraDescription() const { return m_cameraDescription; }

    void setServerManager(ServerManager* mgr) { m_servers = mgr; }
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
    void lastErrorChanged();
    void camerasChanged();
    void cameraDescriptionChanged();

private:
    void pushFrameToPeers();

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
    QVideoFrame m_pendingFrame;
    bool m_active = false;
    QString m_lastError;
    QString m_cameraDescription;
};
