#pragma once

#include <QObject>
#include <QVideoSink>
#include <QVideoFrame>
#include <QImage>
#include <QTimer>
#include <QPointer>
#include <QVariantList>

#ifdef Q_OS_MACOS
class MacScreenCapturer;
#else
#include <QScreenCapture>
#include <QMediaCaptureSession>
#endif

class VoiceEngine;
class ServerManager;
class Settings;

// Controller around Qt6's QScreenCapture. Exposes the capture to QML
// (local preview via a QVideoSink that QML can attach to a
// VideoOutput) and pumps downsampled JPEG frames to the voice
// subsystem for transport over the existing WebRTC data channel.
//
// Real WebRTC video tracks (H.264/VP8 over RTP) would be preferable,
// but adding a codec pipeline + RTP packetization to libdatachannel
// is a much larger change. JPEG-over-data-channel is scrappy but
// gets end-to-end screen sharing working today at ~5 fps.
class ScreenShareController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    // True only while frames are actually landing on ≥1 open peer
    // data channel. `active && !transmitting` = capturing but nobody
    // can see it — the UI shows a "Not visible to others" badge.
    Q_PROPERTY(bool transmitting READ transmitting NOTIFY transmittingChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QVideoSink* previewSink READ previewSink CONSTANT)
    Q_PROPERTY(QVariantList availableScreens READ availableScreens NOTIFY screensChanged)

public:
    explicit ScreenShareController(QObject* parent = nullptr);

    bool active() const { return m_active; }
    bool transmitting() const { return m_transmitting; }
    QString lastError() const { return m_lastError; }
    QVideoSink* previewSink() const { return m_sink; }
    QVariantList availableScreens() const;

    // Quality-preset application. The controller resolves the user's
    // Settings pref and the active server's max on every start(), so
    // admins can tighten a limit live and new shares respect it.
    // Preset map: 0 Low (2 fps, 960 px, Q40)
    //             1 Medium (5 fps, 1280 px, Q60, the prior default)
    //             2 High (10 fps, 1600 px, Q75)
    //             3 Ultra (15 fps, 1920 px, Q85)
    struct QualityPreset { int fps; int maxWidth; int jpegQuality; };
    static QualityPreset presetFor(int level);

    // Hand the controller a pointer to ServerManager so it can
    // look up the active server's VoiceEngine on each frame push.
    // (Voice engines come and go — holding a pointer would race.)
    // Also wires up the auto-stop-on-voice-leave subscription so
    // share state can't outlive the call.
    void setServerManager(ServerManager* mgr);
    // Lets the controller resolve the user's quality pref each start
    // AND re-apply mid-share when the user moves a slider.
    void setSettings(Settings* settings);

    // Start capture of a specific screen by index into availableScreens.
    // Negative value ⇒ pick primary. Idempotent if already running.
    Q_INVOKABLE void startForScreen(int screenIndex);
    Q_INVOKABLE void start();   // convenience → startForScreen(-1)
    Q_INVOKABLE void stop();
    Q_INVOKABLE void toggle();
    // Open the native macOS window/display/app picker. On non-macOS
    // this falls through to start() with the primary display.
    Q_INVOKABLE void showPicker();

    // QML VideoOutput.videoSink is read-only; callers redirect the
    // capture session's output to their own sink via this method.
    Q_INVOKABLE void forwardTo(QVideoSink* sink);

    // Opens the Screen Recording TCC page in System Settings. macOS
    // adhoc-signed dev builds often get TCC attributions confused
    // (the prompt gets credited to the launching process), so the
    // most reliable path is to have the user manually add the app.
    Q_INVOKABLE void openSystemSettings();

signals:
    void activeChanged();
    void transmittingChanged();
    void lastErrorChanged();
    void screensChanged();

private:
#ifdef Q_OS_MACOS
    MacScreenCapturer* m_mac = nullptr;
#else
    QScreenCapture* m_capture = nullptr;
    QMediaCaptureSession* m_session = nullptr;
#endif
    QVideoSink* m_sink = nullptr;   // internal sink we listen on for frames
    QTimer* m_throttle = nullptr;
    ServerManager* m_servers = nullptr;
    Settings* m_settings = nullptr;
    // Per-connection voice-room subscriptions (one per server, not
    // just the active one — voice can be live on a backgrounded
    // server).
    QList<QMetaObject::Connection> m_voiceRoomConns;
    QVideoFrame m_pendingFrame;
    bool m_active = false;
    bool m_transmitting = false;
    QString m_lastError;

    void pushFrameToPeers();
    void setTransmitting(bool transmitting);
    // Rewires the per-server "voice room changed" subscriptions
    // whenever the server list or active server changes. Stops the
    // screen share when no connection is in voice so share state
    // can't outlive the call.
    void rewireVoiceLeaveWatch();
};
