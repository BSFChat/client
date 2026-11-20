#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>
#include <memory>

class QSystemTrayIcon;
class QMenu;
class QAction;
class QWindow;
class ServerManager;
class ServerConnection;
class Settings;
class NotificationSounds;
class AndroidNotifier;

// Routes inbound ServerConnection::messageReceived signals to OS
// notifications via QSystemTrayIcon (Qt 6's cross-platform notification
// surface on macOS, Windows, and most Linux DEs). Owns a hidden tray icon
// and a NotificationSounds instance used solely for chat chimes — voice
// events still use the per-connection NotificationSounds inside
// ServerConnection.
class NotificationManager : public QObject {
    Q_OBJECT
public:
    NotificationManager(ServerManager* serverManager, Settings* settings,
                        QObject* parent = nullptr);
    ~NotificationManager() override;

    // Called from main() once the QML engine has produced the top-level
    // window — we need it to tell "is the app currently focused?" apart from
    // "is any random popover focused?" and to raise/activate on click.
    void setWindow(QWindow* window);

    // Android: whether the persistent SyncService should run while a server
    // is connected. Default: Settings::backgroundConnection(). App points it
    // at PushManager, which also falls back to it when push cannot work on
    // the device. Call refreshBackgroundSync() when the answer changes.
    void setBackgroundSyncPolicy(std::function<bool()> policy);
    void refreshBackgroundSync();

signals:
    // A message for a room other than the one on screen, while the app is
    // in the foreground on a platform with no OS banner for that (iOS:
    // QSystemTrayIcon is unavailable, so nothing showed at all — audit 3).
    // The mobile shell shows it as a tappable in-app toast.
    void inAppMessage(int serverIndex, const QString& roomId,
                      const QString& title, const QString& preview);

private slots:
    void onServerAdded(int index);
    void onServerRemoved(int index);
    void onMessageReceived(const QString& roomId,
                           const QString& senderDisplayName,
                           const QString& body,
                           const QString& eventId,
                           bool mentionsMe);
    void onNotificationClicked();

private:
    void wireConnection(ServerConnection* conn);

    ServerManager* m_serverManager;
    Settings* m_settings;
    QPointer<QWindow> m_window;

    // Hidden tray icon — the icon itself is never shown (setVisible(true)
    // is still required for showMessage to display on some platforms, but
    // we use an empty/transparent icon so it stays unobtrusive).
    std::unique_ptr<QSystemTrayIcon> m_tray;
    std::unique_ptr<QMenu> m_trayMenu;
    QAction* m_muteAction = nullptr;
    QAction* m_deafenAction = nullptr;
    int m_unreadTotal = 0;
    void refreshUnreadBadge();
    void rebuildTrayMenu();

    // Dedicated sounds instance so we don't reach into the voice-gated
    // per-connection NotificationSounds (which doesn't exist when
    // BSFCHAT_VOICE_ENABLED is off).
    NotificationSounds* m_sounds = nullptr;

    // Android JNI bridge — starts the SyncService when the first
    // server connects, posts actual Android notifications via
    // NotificationManager API. No-op off Android.
    AndroidNotifier* m_androidNotifier = nullptr;
    std::function<bool()> m_backgroundSyncPolicy;

    // Remember the server+room of the most recently shown notification so
    // messageClicked can navigate there. QSystemTrayIcon fires a single
    // messageClicked with no payload, so the only way to route is "last one
    // wins" — good enough for the usual click-as-it-pops-up case.
    QString m_pendingServerUrl;
    QString m_pendingRoomId;
    // When the target was stashed; a click on an older notification only
    // raises the window.
    QElapsedTimer m_pendingSetAt;
    static constexpr qint64 kPendingTargetMaxAgeMs = 5 * 60 * 1000;
    void setPendingTarget(const QString& serverUrl, const QString& roomId);
    void raiseWindows();
};
