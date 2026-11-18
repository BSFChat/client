// Mobile mirror of ScreenShareController — the macOS-only screen-
// capture controller that lives behind `screenShare` in QML.
// Exposes the same property / method surface (active,
// lastError, start(), stop(), showPicker()) so VoiceDock's
// existing buttons work unchanged on both platforms.
//
// Internally delegates to Android's MediaProjection via the Java
// ScreenCaptureHelper. Frames arrive on a JNI callback and are
// forwarded to every voice peer via ServerManager -> active
// connection -> VoiceEngine::broadcastScreenFrame (same pipe the
// desktop controller uses).
//
// Compiled only when BSFCHAT_VOICE_ENABLED is on AND the platform
// is Android. Compiles to an empty translation unit otherwise.
#pragma once

#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>

class ServerConnection;
class ServerManager;
class Settings;

class AndroidScreenShareController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit AndroidScreenShareController(QObject* parent = nullptr);

    bool active() const { return m_active; }
    QString lastError() const { return m_lastError; }

    // Also wires the leave hook: the share stops the moment no
    // connection is in voice (or voice moves to another server), the
    // same rule the desktop ScreenShareController applies.
    void setServerManager(ServerManager* m);
    // Settings drive the same fps / maxWidth / jpegQuality knobs the
    // desktop controller honours; we forward the resolved values to
    // ScreenCaptureHelper.configure() over JNI before the projection
    // intent fires (and on every settings change while active).
    void setSettings(Settings* s);

    // Match the desktop controller's verbs so QML bindings in
    // VoiceDock don't need a platform branch.
    Q_INVOKABLE void showPicker();   // prompts consent then starts
    Q_INVOKABLE void stop();

    // The desktop controller's "Hide my IP while sharing" verbs, present here
    // only so a shared QML file cannot throw a TypeError on this platform.
    // They are deliberately inert: the Android share goes through
    // MediaProjection and the same mesh transport, but there is no picker
    // dialog on this path to offer the option in, so the user's STANDING
    // setting is what applies — which VoiceEngine already honours without
    // anything here. Answering "false" to canHideIpWhileSharing would be worse
    // than inert: it would render the option disabled with a reason that is
    // not the real one.
    Q_INVOKABLE void setHideIpForShare(bool) {}
    Q_INVOKABLE bool hideIpForShare() const { return false; }
    Q_INVOKABLE bool canHideIpWhileSharing() const { return true; }

    // Called from JNI. Public so the extern "C" shim can reach it
    // through a global pointer without making the class a friend
    // of free functions.
    void onStarted(int width, int height);
    void onStopped();
    void onPermissionDenied();
    // A projection that failed for a reason worth telling the user — the
    // foreground service being refused, a consent token the platform would
    // not convert, a VirtualDisplay that would not open. Lands in
    // lastError, which VoiceDock already toasts.
    void onError(const QString& message);
    void onFrame(const QByteArray& jpeg);

signals:
    void activeChanged();
    void lastErrorChanged();

private:
    void setActive(bool a);
    void setLastError(const QString& e);
    void broadcast(const QByteArray& jpeg);
    void rewireVoiceLeaveWatch();
    void checkVoiceStillActive();

    bool m_active = false;
    QString m_lastError;
    int m_width = 0;
    int m_height = 0;
    ServerManager* m_serverManager = nullptr;
    Settings* m_settings = nullptr;
    // The connection whose voice call the share is feeding, captured
    // when capture starts. Frames go only there; if voice ends or moves
    // to a different server, the share stops instead of following.
    QPointer<ServerConnection> m_shareServer;
    QList<QMetaObject::Connection> m_voiceRoomConns;
    void pushConfigToHelper();
};
