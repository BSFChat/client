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
    // appears so /sync keeps pumping when the app is backgrounded —
    // when the background-connection policy says so. With push built
    // in and working that is off by default: FCM wakes the app instead
    // (docs/push-notifications.md), and a dataSync foreground service
    // is only kept for users who chose it or devices without Google
    // Play services. This only records the intent: AndroidNotifier
    // decides when the platform will actually permit the start (this
    // signal can fire during a restore, before the activity is resumed,
    // and starting a foreground service from the background is fatal on
    // Android 12+). Torn down in onServerRemoved once the last
    // connection goes.
    m_backgroundSyncPolicy = [settings]() { return settings->backgroundConnection(); };
    connect(serverManager, &ServerManager::serverAdded, this,
        [this](int /*index*/) { refreshBackgroundSync(); });

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
        // It only raises: the icon is not a notification, and jumping to
        // whichever room last notified — however long ago — took people
        // away from what they were reading (audit 3).
        connect(m_tray.get(), &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason r) {
                if (r == QSystemTrayIcon::Trigger
                    || r == QSystemTrayIcon::DoubleClick) {
                    raiseWindows();
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

void NotificationManager::setBackgroundSyncPolicy(std::function<bool()> policy)
{
    m_backgroundSyncPolicy = std::move(policy);
    refreshBackgroundSync();
}

void NotificationManager::refreshBackgroundSync()
{
    if (!m_androidNotifier || !m_serverManager) return;
    const bool wanted = m_serverManager->connectionCount() > 0
                        && (!m_backgroundSyncPolicy || m_backgroundSyncPolicy());
    if (wanted) m_androidNotifier->startSyncService();
    else m_androidNotifier->stopSyncService();
}

void NotificationManager::onServerAdded(int index)
{
    wireConnection(m_serverManager->connectionAt(index));
}

void NotificationManager::onServerRemoved(int /*index*/)
{
    // ServerConnection is deleteLater()'d by ServerManager, which
    // auto-disconnects our slot — nothing else to do for the wiring.
    //
    // The Android sync service is the exception. It was started when the
    // first connection appeared and nothing ever stopped it: the header
    // said "called at sign-in / sign-out" but stopSyncService() had no
    // caller anywhere in the tree, so a signed-out app went on holding a
    // dataSync foreground service, a wake-ish process anchor and a
    // permanent notification for nothing — and burned the Android 15
    // six-hour budget doing it. With no servers left there is nothing to
    // sync, so put it down.
    if (m_androidNotifier && m_serverManager
        && m_serverManager->connectionCount() == 0) {
        m_androidNotifier->stopSyncService();
    }
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

    // An invite is a notification of its own (audit 3, #6): the invitee is
    // not in the room, so no message event will ever announce it. The toast
    // covers a focused window; this covers everything else.
    connect(conn, &ServerConnection::inviteReceived, this,
        [this, conn](const QString&, const QString& roomName,
                     const QString& inviterName, bool isDirect) {
            if (!m_settings || !m_settings->notificationsEnabled()) return;
            const bool windowFocused = QGuiApplication::focusWindow() != nullptr;
            if (windowFocused) return;
            const QString title = isDirect ? tr("New message request")
                                           : tr("New channel invite");
            const QString body = isDirect
                ? tr("%1 wants to message you").arg(inviterName)
                : tr("%1 invited you to %2").arg(inviterName,
                      roomName.isEmpty() ? tr("a channel") : QStringLiteral("#") + roomName);
            // Clicking opens the server it is on; the invite is in its sidebar.
            setPendingTarget(conn->serverUrl(), QString());
            if (m_tray) m_tray->showMessage(title, body, QSystemTrayIcon::Information, 5000);
        });

    // When the user opens a room, clear any Android notifications
    // we'd posted for that room — they're already seeing the
    // messages, so leaving the shade entry would be redundant.
    // Lambdas can't use Qt::UniqueConnection (Qt requires a
    // pointer-to-member-function for the dedup) — the
    // wireConnection call site only fires once per server-add
    // anyway so duplicate connects aren't a real risk.
    connect(conn, &ServerConnection::activeRoomIdChanged, this,
        [this, conn]() {
            if (!m_androidNotifier) return;
            QString roomId = conn->activeRoomId();
            if (roomId.isEmpty()) return;
            // One tag per room, shared with the notifications PushBridge.java
            // posts while BSFChat is not running, so this clears either.
            m_androidNotifier->cancelByTag(AndroidNotifier::roomTag(roomId));
        });
}

void NotificationManager::rebuildTrayMenu()
{
    if (!m_tray) return;
    if (!m_trayMenu) m_trayMenu = std::make_unique<QMenu>();
    m_trayMenu->clear();

    QAction* showAct = m_trayMenu->addAction(tr("Show BSFChat"));
    connect(showAct, &QAction::triggered, this, &NotificationManager::raiseWindows);

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
    // Not on iOS: PushManager owns the icon badge there and sets it to the
    // unread MESSAGE total, the unit APNs pushes use (audit 3).
#ifndef Q_OS_IOS
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        app->setBadgeNumber(total);
#endif
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
    const QString roomMode = m_settings->roomNotificationMode(roomId, sender->isDirectRoom(roomId));
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
    setPendingTarget(sender->serverUrl(), roomId);

    // Shared notification text for both desktop tray + Android.
    const QString title = mentionsMe
        ? QStringLiteral("@ %1").arg(senderDisplayName)
        : senderDisplayName;
    QString preview = body;
    if (preview.size() > 200) preview = preview.left(197) + QStringLiteral("...");

    if (m_tray) {
        m_tray->showMessage(title, preview, QSystemTrayIcon::Information, 5000);
    }
#if defined(Q_OS_IOS)
    // No tray, no Android shade: in the foreground nothing at all said a
    // message had arrived elsewhere. Only for rooms not on screen.
    if (!isActiveRoom && QGuiApplication::applicationState() == Qt::ApplicationActive) {
        int idx = -1;
        for (int i = 0; i < m_serverManager->connectionCount(); ++i)
            if (m_serverManager->connectionAt(i) == sender) { idx = i; break; }
        if (idx >= 0) emit inAppMessage(idx, roomId, title, preview);
    }
#endif

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
        // One notification per room: a newer message replaces the older
        // one, and replaces a generic one a push posted for the same room.
        m_androidNotifier->postChatNotification(
            AndroidNotifier::roomTag(roomId), title, preview, deepLink);
    }

    if (m_settings->notificationSound() && m_sounds) {
        m_sounds->playMessage();
    }
}

void NotificationManager::setPendingTarget(const QString& serverUrl, const QString& roomId)
{
    m_pendingServerUrl = serverUrl;
    m_pendingRoomId = roomId;
    m_pendingSetAt.start();
}

void NotificationManager::raiseWindows()
{
    // Raise + activate the main window. We loop top-level windows because
    // m_window may not have been set yet on very early notifications.
    for (QWindow* w : QGuiApplication::topLevelWindows()) {
        w->raise();
        w->requestActivate();
    }
}

void NotificationManager::onNotificationClicked()
{
    raiseWindows();

    // One click per notification, and only while it can still be the one on
    // screen: a balloon clicked minutes later is not, and the old target
    // used to be followed forever.
    const bool fresh = m_pendingSetAt.isValid()
        && m_pendingSetAt.elapsed() < kPendingTargetMaxAgeMs;
    const QString serverUrl = m_pendingServerUrl;
    const QString roomId = m_pendingRoomId;
    m_pendingServerUrl.clear();
    m_pendingRoomId.clear();
    m_pendingSetAt.invalidate();
    if (!fresh || serverUrl.isEmpty()) return;

    // Locate the connection by URL and switch to it + the target room.
    for (int i = 0; i < m_serverManager->connectionCount(); ++i) {
        ServerConnection* conn = m_serverManager->connectionAt(i);
        if (!conn || conn->serverUrl() != serverUrl) continue;
        if (!roomId.isEmpty()) {
            if (conn->activeRoomId() != roomId || m_serverManager->activeServerIndex() != i)
                m_serverManager->openRoom(i, roomId);
        } else if (m_serverManager->activeServerIndex() != i) {
            m_serverManager->setViewingDms(false);
            m_serverManager->setActiveServer(i);
        }
        break;
    }
}
