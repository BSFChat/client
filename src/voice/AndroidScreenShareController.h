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

#include <QObject>
#include <QString>

class ServerManager;

class AndroidScreenShareController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit AndroidScreenShareController(QObject* parent = nullptr);

    bool active() const { return m_active; }
    QString lastError() const { return m_lastError; }

    void setServerManager(ServerManager* m) { m_serverManager = m; }

    // Match the desktop controller's verbs so QML bindings in
    // VoiceDock don't need a platform branch.
    Q_INVOKABLE void showPicker();   // prompts consent then starts
    Q_INVOKABLE void stop();

    // Called from JNI. Public so the extern "C" shim can reach it
    // through a global pointer without making the class a friend
    // of free functions.
    void onStarted(int width, int height);
    void onStopped();
    void onPermissionDenied();
    void onFrame(const QByteArray& jpeg);

signals:
    void activeChanged();
    void lastErrorChanged();

private:
    void setActive(bool a);
    void setLastError(const QString& e);
    void broadcast(const QByteArray& jpeg);

    bool m_active = false;
    QString m_lastError;
    int m_width = 0;
    int m_height = 0;
    ServerManager* m_serverManager = nullptr;
};
