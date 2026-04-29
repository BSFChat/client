#include "core/NotificationManager.h"

#include "core/AndroidNotifier.h"
#include "core/Settings.h"
#include "net/ServerConnection.h"
#include "net/ServerManager.h"
#include "voice/NotificationSounds.h"

#include <QUrl>

#include <QAction>
#include <QApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSystemTrayIcon>
#include <QTimer>
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

    // Android bridge — owns the JNI calls that talk to
    // NotificationManager.java + starts the SyncService. The object
    // itself is safe to construct on desktop; every method is a
    // no-op off Android.
    m_androidNotifier = new AndroidNotifier(this);

    // Spin up the Android SyncService once the first connection
    // appears so /sync keeps pumping when the app is backgrounded.
    // Stopped lazily — there's no sign-out hook here, but
    // application-exit hits aboutToQuit which the C++ side already
    // uses to tear the process down.
    connect(serverManager, &ServerManager::serverAdded, this,
        [this](int /*index*/) {
            if (m_androidNotifier) m_androidNotifier->startSyncService();
        });

    // QSystemTrayIcon is the portable Qt 6 path for OS notifications.
    // On macOS this routes through NSUserNotificationCenter, on Windows
    // through the Action Center / legacy balloon API, and on Linux through
    // org.freedesktop.Notifications. We keep the tray icon itself hidden
    // (transparent pixmap) because BSFChat doesn't want a tray presence —
    // the icon only exists so showMessage() has something to attach to.
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        m_tray = std::make_unique<QSystemTrayIcon>(this);

        // Tray icon — simple rounded accent square with "B" glyph.
        // Qt will template-tint it on macOS since we render pure
        // black on transparent and let the OS invert for dark menu
        // bars. Keep the resolution high so it looks crisp at 2x.
        QPixmap px(44, 44);
        px.fill(Qt::transparent);
        {
            QPainter p(&px);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::black);
            p.drawRoundedRect(0, 0, 44, 44, 9, 9);
            p.setPen(Qt::white);
            QFont f; f.setPixelSize(26); f.setBold(true);
            p.setFont(f);
            p.drawText(QRect(0, 0, 44, 44), Qt::AlignCenter, "B");
        }
        QIcon ic(px);
        ic.setIsMask(true);  // template mode on macOS
        m_tray->setIcon(ic);
        m_tray->setToolTip(QStringLiteral("BSFChat"));

        rebuildTrayMenu();
        m_tray->setVisible(true);

        connect(m_tray.get(), &QSystemTrayIcon::messageClicked,
                this, &NotificationManager::onNotificationClicked);
        // Single left-click raises the app, matching most chat apps.
        connect(m_tray.get(), &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason r) {
                if (r == QSystemTrayIcon::Trigger
                    || r == QSystemTrayIcon::DoubleClick) {
                    onNotificationClicked();
                }
            });
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
    // Any time a server-scope flag changes that could affect unread
    // totals or the current mute/deafen state, refresh the tray.
    connect(conn, &ServerConnection::hasUnreadChanged, this,
            &NotificationManager::refreshUnreadBadge, Qt::UniqueConnection);
    connect(conn, &ServerConnection::voiceMutedChanged, this,
            &NotificationManager::rebuildTrayMenu, Qt::UniqueConnection);
    connect(conn, &ServerConnection::voiceDeafenedChanged, this,
            &NotificationManager::rebuildTrayMenu, Qt::UniqueConnection);

    // When the user opens a room, clear any Android notifications
    // we'd posted for that room — they're already seeing the
    // messages, so leaving the shade entry would be redundant.
    connect(conn, &ServerConnection::activeRoomIdChanged, this,
        [this, conn]() {
            if (!m_androidNotifier) return;
            QString roomId = conn->activeRoomId();
            if (roomId.isEmpty()) return;
            m_androidNotifier->cancelByTag(conn->serverUrl()
                + QStringLiteral("|") + roomId);
        }, Qt::UniqueConnection);
}

void NotificationManager::rebuildTrayMenu()
{
    if (!m_tray) return;
    if (!m_trayMenu) m_trayMenu = std::make_unique<QMenu>();
    m_trayMenu->clear();

    QAction* showAct = m_trayMenu->addAction(tr("Show BSFChat"));
    connect(showAct, &QAction::triggered, this,
            &NotificationManager::onNotificationClicked);

    m_trayMenu->addSeparator();

    // Best-effort: control the first server that's currently in a
    // voice channel. If none is, these are disabled.
    ServerConnection* voiceTarget = nullptr;
    for (int i = 0; i < m_serverManager->connectionCount(); ++i) {
        auto* c = m_serverManager->connectionAt(i);
        if (c && c->inVoiceChannel()) { voiceTarget = c; break; }
    }
    m_muteAction = m_trayMenu->addAction(
        voiceTarget && voiceTarget->voiceMuted() ? tr("Unmute") : tr("Mute"));
    m_muteAction->setEnabled(voiceTarget != nullptr);
    if (voiceTarget) {
        connect(m_muteAction, &QAction::triggered,
                voiceTarget, &ServerConnection::toggleMute);
    }
    m_deafenAction = m_trayMenu->addAction(
        voiceTarget && voiceTarget->voiceDeafened() ? tr("Undeafen") : tr("Deafen"));
    m_deafenAction->setEnabled(voiceTarget != nullptr);
    if (voiceTarget) {
        connect(m_deafenAction, &QAction::triggered,
                voiceTarget, &ServerConnection::toggleDeafen);
    }

    m_trayMenu->addSeparator();
    QAction* quitAct = m_trayMenu->addAction(tr("Quit BSFChat"));
    connect(quitAct, &QAction::triggered,
            qApp, &QCoreApplication::quit);

    m_tray->setContextMenu(m_trayMenu.get());
}

void NotificationManager::refreshUnreadBadge()
{
    int total = 0;
    for (int i = 0; i < m_serverManager->connectionCount(); ++i) {
        auto* c = m_serverManager->connectionAt(i);
        if (c && c->hasUnread()) ++total;
    }
    if (total == m_unreadTotal) return;
    m_unreadTotal = total;

    if (m_tray) {
        m_tray->setToolTip(total > 0
            ? QStringLiteral("BSFChat — %1 server%2 with unread")
                .arg(total).arg(total == 1 ? "" : "s")
            : QStringLiteral("BSFChat"));
    }
    // Dock / taskbar badge. Qt 6.5+ — setBadgeNumber is an instance
    // method on the application singleton.
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        app->setBadgeNumber(total);
}

void NotificationManager::onMessageReceived(const QString& roomId,
                                             const QString& senderDisplayName,
                                             const QString& body,
                                             const QString& eventId,
                                             bool mentionsMe)
{
    if (!m_settings || !m_settings->notificationsEnabled()) return;

    auto* sender = qobject_cast<ServerConnection*>(QObject::sender());
    if (!sender) return;

    // Per-room override: users can set a channel to @mentions-only or
    // none via the room context menu. Honour it before any further
    // routing — we also skip the chat chime, so a "none" room is
    // entirely silent (the unread dot still appears in the channel
    // list; that's a different signal).
    const QString roomMode = m_settings->roomNotificationMode(roomId);
    if (roomMode == QStringLiteral("none")) return;
    if (roomMode == QStringLiteral("mentions") && !mentionsMe) return;

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

    // Shared notification text for both desktop tray + Android.
    const QString title = mentionsMe
        ? QStringLiteral("@ %1").arg(senderDisplayName)
        : senderDisplayName;
    QString preview = body;
    if (preview.size() > 200) preview = preview.left(197) + QStringLiteral("...");

    if (m_tray) {
        m_tray->showMessage(title, preview, QSystemTrayIcon::Information, 5000);
    }

    // Android: post a real NotificationManager notification so the
    // user sees it in the shade while the app is backgrounded. The
    // tap action fires a bsfchat://message/<server>/<room>/<event>
    // URL that the existing deep-link handler resolves (switches
    // server, opens room, scrolls to event).
    if (m_androidNotifier) {
        QString deepLink = QStringLiteral("bsfchat://message/%1/%2/%3")
            .arg(QString::fromUtf8(
                QUrl::toPercentEncoding(sender->serverUrl())))
            .arg(QString::fromUtf8(
                QUrl::toPercentEncoding(roomId)))
            .arg(QString::fromUtf8(
                QUrl::toPercentEncoding(eventId)));
        // Tag = server|room so multiple new messages in the same
        // room collapse into the latest (classic chat-app
        // behaviour). Room becomes the group key so the shade can
        // summarise per-room.
        QString tag = sender->serverUrl() + QStringLiteral("|") + roomId;
        QString group = QStringLiteral("bsfchat:room:") + roomId;
        m_androidNotifier->postChatNotification(
            tag, title, preview, deepLink, group);
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
