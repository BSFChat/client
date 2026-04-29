#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
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

    // Remember the server+room of the most recently shown notification so
    // messageClicked can navigate there. QSystemTrayIcon fires a single
    // messageClicked with no payload, so the only way to route is "last one
    // wins" — good enough for the usual click-as-it-pops-up case.
    QString m_pendingServerUrl;
    QString m_pendingRoomId;
};
