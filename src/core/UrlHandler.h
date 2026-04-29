#pragma once

#include <QObject>
#include <QString>

class QLocalServer;
class QCoreApplication;

// Wires up OS-level `bsfchat://` URL handling.
//
// Split across three responsibilities that all funnel into the same signal:
//
//   1. **macOS**: Launch Services sends a `QFileOpenEvent` to the running
//      app when a `bsfchat://…` URL is activated. An event filter installed
//      on QCoreApplication catches it. The scheme itself is registered via
//      CFBundleURLTypes in Info.plist (see CMakeLists.txt POST_BUILD step).
//
//   2. **Windows / Linux**: the OS runs `bsfchat-app.exe <url>` — the URL
//      arrives as argv[1]. forwardToRunningInstance() speaks to an existing
//      instance via QLocalSocket; if there's a running app, the URL is
//      handed to it and the freshly-launched process exits. Otherwise this
//      process becomes the single instance and listens on a QLocalServer.
//      Registration is done at startup:
//         - Windows: HKCU\Software\Classes\bsfchat via QSettings
//         - Linux:   a .desktop file in ~/.local/share/applications,
//                    registered with `xdg-mime default …`
//
// All three paths converge on the urlReceived() signal. Main.cpp connects
// that to ServerManager::openMessageLink and raises the window.
class UrlHandler : public QObject {
    Q_OBJECT
public:
    // Attempt to hand `url` to an already-running instance. Returns true if
    // a running instance accepted the URL (caller should then exit). Returns
    // false if no running instance exists or the handoff failed.
    // Safe to call before QCoreApplication is constructed? No — needs one.
    static bool forwardToRunningInstance(const QString& url);

    // Extract a bsfchat:// URL from argv, if any. Returns "" when absent.
    static QString urlFromArgv(int argc, char** argv);

    explicit UrlHandler(QObject* parent = nullptr);
    ~UrlHandler() override;

    // Install the macOS event filter AND start the single-instance local
    // server on all platforms. Safe to call once the QGuiApplication exists.
    void install(QCoreApplication* app);

    // Register this binary as the OS-level handler for `bsfchat://`.
    // macOS is a build-time Info.plist affair (no-op here). Windows writes
    // HKCU registry entries. Linux writes a .desktop file. All are idempotent.
    void registerSchemeHandler();

    // Android: read the Activity's launch intent and, if it's an
    // ACTION_SEND, emit sharedPayloadReceived. Called once after
    // engine.load() so QML listeners are wired. No-op off Android.
    void checkAndroidShareIntent();

signals:
    void urlReceived(const QString& url);
    // Android ACTION_SEND share. `payloadUri` is either a content://
    // URI (for EXTRA_STREAM, i.e. shared file) or a plain text blob
    // (for EXTRA_TEXT). `mimeType` is whatever the source app declared.
    // MobileMain wires this into the active room's composer /
    // sendMediaMessage path.
    void sharedPayloadReceived(const QString& payloadOrUri,
                               const QString& mimeType,
                               bool isFile);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void onNewConnection();
    static QString socketName();

    QLocalServer* m_server = nullptr;
};
