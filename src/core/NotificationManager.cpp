#include "core/NotificationManager.h"

#include "core/Settings.h"
#include "net/ServerConnection.h"
#include "net/ServerManager.h"
#include "voice/NotificationSounds.h"

#include <QGuiApplication>
#include <QIcon>
#include <QPixmap>
#include <QSystemTrayIcon>
#include <QWindow>

NotificationManager::NotificationManager(ServerManager* serverManager,
                                          Settings* settings,
                                          QObject* parent)
    : QObject(parent)
    , m_serverManager(serverManager)
    , m_settings(settings)
{
    // Own a dedicated NotificationSounds — voice's per-connection instance
    // is gated on BSFCHAT_VOICE_ENABLED, and chat chimes should work even
    // in no-voice builds.
    m_sounds = new NotificationSounds(this);

    // QSystemTrayIcon is the portable Qt 6 path for OS notifications.
    // On macOS this routes through NSUserNotificationCenter, on Windows
    // through the Action Center / legacy balloon API, and on Linux through
    // org.freedesktop.Notifications. We keep the tray icon itself hidden
    // (transparent pixmap) because BSFChat doesn't want a tray presence —
    // the icon only exists so showMessage() has something to attach to.
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        m_tray = std::make_unique<QSystemTrayIcon>(this);
        QPixmap transparent(16, 16);
        transparent.fill(Qt::transparent);
        m_tray->setIcon(QIcon(transparent));
        m_tray->setToolTip(QStringLiteral("BSFChat"));
        m_tray->setVisible(true);
        connect(m_tray.get(), &QSystemTrayIcon::messageClicked,
                this, &NotificationManager::onNotificationClicked);
    }

    // Wire any connections restored from settings before we were born.
    for (int i = 0; i < m_serverManager->connectionCount(); ++i) {
        wireConnection(m_serverManager->connectionAt(i));
    }
    // And future ones as they're added.
    connect(m_serverManager, &ServerManager::serverAdded,
            this, &NotificationManager::onServerAdded);
    connect(m_serverManager, &ServerManager::serverRemoved,
            this, &NotificationManager::onServerRemoved);
}

NotificationManager::~NotificationManager() = default;

void NotificationManager::setWindow(QWindow* window)
{
    m_window = window;
}

void NotificationManager::onServerAdded(int index)
{
    wireConnection(m_serverManager->connectionAt(index));
}

void NotificationManager::onServerRemoved(int /*index*/)
{
    // ServerConnection is deleteLater()'d by ServerManager, which
    // auto-disconnects our slot — nothing to do here.
}

void NotificationManager::wireConnection(ServerConnection* conn)
{
    if (!conn) return;
    connect(conn, &ServerConnection::messageReceived,
            this, &NotificationManager::onMessageReceived,
            Qt::UniqueConnection);
}

void NotificationManager::onMessageReceived(const QString& roomId,
                                             const QString& senderDisplayName,
                                             const QString& body,
                                             const QString& /*eventId*/,
                                             bool mentionsMe)
{
    if (!m_settings || !m_settings->notificationsEnabled()) return;

    auto* sender = qobject_cast<ServerConnection*>(QObject::sender());
    if (!sender) return;

    // Suppression: don't notify if the user is actively looking at this
    // exact room on the active server AND the app window is focused. Any
    // other scenario (different room, different server, window minimised,
    // window unfocused) warrants a notification. Explicit @-mentions
    // bypass this — if someone pinged you directly, you want to see it
    // even if the app is focused.
    const bool isActiveServer = (m_serverManager->activeServer() == sender);
    const bool isActiveRoom = isActiveServer && (sender->activeRoomId() == roomId);
    const bool windowFocused =
        (QGuiApplication::focusWindow() != nullptr)
        && (!m_window || QGuiApplication::focusWindow() == m_window.data());
    if (isActiveRoom && windowFocused && !mentionsMe) return;

    // Stash routing target for messageClicked (see onNotificationClicked).
    m_pendingServerUrl = sender->serverUrl();
    m_pendingRoomId = roomId;

    if (m_tray) {
        // Prefix mention notifications so they visually stand apart in the
        // notification center stack.
        const QString title = mentionsMe
            ? QStringLiteral("@ %1").arg(senderDisplayName)
            : senderDisplayName;
        // Truncate overly long bodies so platform notification widgets
        // don't clip mid-word.
        QString preview = body;
        if (preview.size() > 200) preview = preview.left(197) + QStringLiteral("...");
        m_tray->showMessage(title, preview, QSystemTrayIcon::Information, 5000);
    }

    if (m_settings->notificationSound() && m_sounds) {
        m_sounds->playMessage();
    }
}

void NotificationManager::onNotificationClicked()
{
    // Raise + activate the main window. We loop top-level windows because
    // m_window may not have been set yet on very early notifications.
    for (QWindow* w : QGuiApplication::topLevelWindows()) {
        w->raise();
        w->requestActivate();
    }

    if (m_pendingServerUrl.isEmpty() || m_pendingRoomId.isEmpty()) return;

    // Locate the connection by URL and switch to it + the target room.
    for (int i = 0; i < m_serverManager->connectionCount(); ++i) {
        ServerConnection* conn = m_serverManager->connectionAt(i);
        if (!conn || conn->serverUrl() != m_pendingServerUrl) continue;
        if (m_serverManager->activeServerIndex() != i)
            m_serverManager->setActiveServer(i);
        if (conn->activeRoomId() != m_pendingRoomId)
            conn->setActiveRoom(m_pendingRoomId);
        break;
    }
}
