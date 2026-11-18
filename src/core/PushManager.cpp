#include "core/PushManager.h"

#include "core/PushBridge.h"
#include "core/PushPlatform.h"
#include "core/Settings.h"
#include "net/MatrixClient.h"
#include "net/ServerConnection.h"
#include "net/ServerManager.h"

#include <QDebug>
#include <QJsonDocument>
#include <QLocale>
#include <QTimer>
#include <QUrl>

#include <QStringList>

#include <memory>

namespace push = bsfchat::client::push;

namespace {

constexpr bool kIsIos =
#ifdef Q_OS_IOS
    true;
#else
    false;
#endif

// How long a pushed event may take to arrive over this process's own /sync
// before the FCM service posts a generic notification instead. Below
// PushBridge.java's NATIVE_WAIT_MS, which is below Firebase's ~10 s.
constexpr int kPushedEventWaitMs = 7000;
// Event ids recently seen over /sync, so a push that loses the race to the
// live sync is answered at once instead of after the timeout.
constexpr int kRecentEvents = 256;

} // namespace

PushManager::PushManager(ServerManager* serverManager, Settings* settings, QObject* parent)
    : QObject(parent)
    , m_serverManager(serverManager)
    , m_settings(settings)
    , m_bridge(PushBridge::instance())
{
    push::PusherIdentity id;
    id.platform = kIsIos ? push::Platform::Ios : push::Platform::Android;
    id.appId = QString::fromLatin1(bsfchat::push::kAppId);
    id.deviceDisplayName = m_bridge->deviceName();
    id.lang = QLocale::system().bcp47Name();
    m_registrar = new PushRegistrar(this, id, this);

    connect(m_registrar, &PushRegistrar::registeredPushkeyChanged, this,
            [this](const QString& account, const QString& pushkey) {
                m_settings->setRegisteredPusher(account, pushkey);
            });
    connect(m_registrar, &PushRegistrar::stateChanged, this, &PushManager::statusChanged);

    connect(m_bridge, &PushBridge::tokenChanged, this, [this]() {
        m_registrar->setToken(m_bridge->token());
        emit statusChanged();
    });
    connect(m_bridge, &PushBridge::unavailableChanged, this, [this]() {
        applySettings();
        emit statusChanged();
    });
    connect(m_bridge, &PushBridge::messageReceived, this, &PushManager::onPushedEvent);
    connect(m_bridge, &PushBridge::notificationTapped, this, &PushManager::onTapped);

    connect(m_settings, &Settings::notificationsEnabledChanged, this, &PushManager::applySettings);
    connect(m_settings, &Settings::pushShowNamesChanged, this, &PushManager::applySettings);
    connect(m_settings, &Settings::pushGatewayUrlChanged, this, &PushManager::applySettings);
    connect(m_settings, &Settings::backgroundConnectionChanged, this, &PushManager::applySettings);

    for (int i = 0; i < m_serverManager->connectionCount(); ++i)
        watch(m_serverManager->connectionAt(i));
    connect(m_serverManager, &ServerManager::serverAdded, this, [this](int index) {
        watch(m_serverManager->connectionAt(index));
    });
    // iOS asks for notification permission after the first server sign-in,
    // the moment Android asks for POST_NOTIFICATIONS (main.cpp). The system
    // only ever prompts once; later calls just re-register.
    if (kIsIos) {
        connect(m_serverManager, &ServerManager::loginSuccess, this, [this]() {
            if (pushWanted()) m_bridge->start(true);
        }, Qt::QueuedConnection);
    }
    m_serverManager->setPusherDeletionHook([this](ServerConnection* conn) {
        return takeDeletions(conn);
    });

    m_lastBackgroundSyncWanted = backgroundSyncWanted();
    m_registrar->setToken(m_bridge->token());
    applySettings();
    if (!kIsIos) m_bridge->start(pushWanted());
    else if (pushWanted() && m_serverManager->connectionCount() > 0) m_bridge->start(true);

    QString room, event;
    if (m_bridge->takePendingTap(&room, &event)) {
        QMetaObject::invokeMethod(this, [this, room, event]() { onTapped(room, event); },
                                  Qt::QueuedConnection);
    }
}

PushManager::~PushManager()
{
    if (m_serverManager) m_serverManager->setPusherDeletionHook({});
}

bool PushManager::available() const
{
    return bsfchat::push::kCompiledIn;
}

bool PushManager::pushWanted() const
{
    if (!available() || !m_settings->notificationsEnabled()) return false;
    if (!kIsIos && m_settings->backgroundConnection()) return false;
    return true;
}

bool PushManager::active() const
{
    return pushWanted() && !m_bridge->token().isEmpty();
}

bool PushManager::backgroundSyncWanted() const
{
    if (kIsIos) return false;   // no such thing on iOS
    return m_settings->backgroundConnection() || !available() || m_bridge->unavailable();
}

QString PushManager::statusText() const
{
    if (!available()) return {};
    if (!m_settings->notificationsEnabled()) return {};
    if (!pushWanted()) return tr("Using a background connection instead of push.");
    if (m_bridge->unavailable()) {
        return kIsIos
            ? tr("Push notifications are not available: allow notifications for BSFChat "
                 "in iOS Settings.")
            : tr("Push is not available on this device (no Google Play services?). "
                 "Using a background connection instead.");
    }
    if (m_bridge->token().isEmpty()) return tr("Waiting for the push service…");

    int registered = 0;
    QStringList notAllowed, namesOff;
    for (const QString& account : m_registrar->accounts()) {
        const QString host = QUrl(account.section(QLatin1Char('|'), 1)).host();
        switch (m_registrar->state(account)) {
        case PushRegistrar::State::Registered: ++registered; break;
        case PushRegistrar::State::GatewayRefused: notAllowed << host; break;
        default: break;
        }
        if (m_registrar->namesRefused(account)) namesOff << host;
    }
    QStringList parts;
    parts << tr("Push is on for %n server(s).", nullptr, registered);
    if (!notAllowed.isEmpty())
        parts << tr("%1 does not allow this push gateway, so it can only notify you "
                    "while BSFChat is open.").arg(notAllowed.join(QStringLiteral(", ")));
    if (!namesOff.isEmpty() && m_settings->pushShowNames())
        parts << tr("%1 only sends \"New message\".").arg(namesOff.join(QStringLiteral(", ")));
    return parts.join(QLatin1Char(' '));
}

void PushManager::applySettings()
{
    m_registrar->setGatewayUrl(m_settings->pushGatewayUrl());
    m_registrar->setTier(m_settings->pushShowNames() ? push::PayloadTier::Names
                                                     : push::PayloadTier::EventIdOnly);
    const bool wanted = pushWanted();
    if (wanted != m_registrar->wanted()) {
        m_registrar->setWanted(wanted);
        // Android: starting or stopping Firebase itself (token minted or
        // deleted). iOS: only (re-)register; there is nothing to stop.
        if (!kIsIos || (wanted && m_serverManager->connectionCount() > 0))
            m_bridge->start(wanted);
    }
    m_bridge->setPreferences(m_settings->notificationsEnabled(), m_settings->pushShowNames());

    const bool bg = backgroundSyncWanted();
    if (bg != m_lastBackgroundSyncWanted) {
        m_lastBackgroundSyncWanted = bg;
        emit backgroundSyncWantedChanged();
    }
    emit statusChanged();
}

QString PushManager::accountKey(const ServerConnection* conn)
{
    if (!conn || conn->userId().isEmpty()) return {};
    return conn->userId() + QLatin1Char('|') + conn->serverUrl();
}

void PushManager::watch(ServerConnection* conn)
{
    if (!conn) return;
    connect(conn, &ServerConnection::connectedChanged, this, [this, conn]() { evaluate(conn); });
    connect(conn, &ServerConnection::userIdChanged, this, [this, conn]() { evaluate(conn); });
    // Remember what /sync delivered, for onPushedEvent's race.
    connect(conn, &ServerConnection::messageReceived, this,
            [this, conn](const QString&, const QString&, const QString&,
                         const QString& eventId, bool) {
                QStringList& recent = m_recent[conn];
                recent.append(eventId);
                if (recent.size() > kRecentEvents) recent.removeFirst();
            });
    connect(conn, &QObject::destroyed, this, [this, conn]() {
        m_recent.remove(conn);
        const QString key = m_keys.take(conn);
        // A rebuilt connection or app shutdown: the server-side pusher
        // stays. A sign-out has already taken the account (takeDeletions).
        if (!key.isEmpty()) m_registrar->dropAccount(key);
    });
    evaluate(conn);
}

void PushManager::evaluate(ServerConnection* conn)
{
    if (!conn) return;
    const QString key = accountKey(conn);
    const QString old = m_keys.value(conn);
    if (!old.isEmpty() && old != key) {
        m_registrar->dropAccount(old);
        m_keys.remove(conn);
    }
    if (key.isEmpty() || !conn->isConnected() || conn->accessToken().isEmpty()) return;
    m_keys.insert(conn, key);
    if (m_registrar->hasAccount(key)) {
        m_registrar->retryFailed();
        return;
    }
    m_registrar->addAccount(key, m_settings->registeredPushers().value(key).toString());
}

ServerConnection* PushManager::connectionFor(const QString& account) const
{
    for (auto it = m_keys.constBegin(); it != m_keys.constEnd(); ++it)
        if (it.value() == account) return it.key();
    return nullptr;
}

void PushManager::setPusher(const QString& account, const QJsonObject& body, Done done)
{
    ServerConnection* conn = connectionFor(account);
    if (!conn || !conn->client()) {
        done(Reply{0, {}, true});
        return;
    }
    conn->client()->setPusher(QJsonDocument(body).toJson(QJsonDocument::Compact),
        [done](int status, const QByteArray& replyBody, bool transportError) {
            done(Reply{status, replyBody, transportError});
        });
}

QList<QByteArray> PushManager::takeDeletions(ServerConnection* conn)
{
    QList<QByteArray> out;
    QString key = m_keys.take(conn);
    if (key.isEmpty()) key = accountKey(conn);
    if (key.isEmpty()) return out;
    for (const QJsonObject& body : m_registrar->takeAccount(key))
        out.append(QJsonDocument(body).toJson(QJsonDocument::Compact));
    return out;
}

ServerConnection* PushManager::connectionForRoom(const QString& roomId) const
{
    return m_serverManager->connectionForRoom(roomId);
}

void PushManager::onPushedEvent(const QString& roomId, const QString& eventId, int unread)
{
    Q_UNUSED(unread);
    ServerConnection* conn = connectionForRoom(roomId);
    if (!conn) {
        m_bridge->answerPushedEvent(eventId, false);
        return;
    }
    if (m_recent.value(conn).contains(eventId)) {
        m_bridge->answerPushedEvent(eventId, true);
        return;
    }
    // Fetch it on the device, over this server's own /sync. If it comes in,
    // NotificationManager raises the full notification from local data (and
    // applies the room's notification mode); Google only ever saw the ids.
    auto answered = std::make_shared<bool>(false);
    auto* waiter = new QObject(this);
    const auto answer = [this, eventId, answered, waiter](bool handled) {
        if (*answered) return;
        *answered = true;
        m_bridge->answerPushedEvent(eventId, handled);
        waiter->deleteLater();
    };
    connect(conn, &ServerConnection::messageReceived, waiter,
            [eventId, answer](const QString&, const QString&, const QString&,
                              const QString& got, bool) {
                if (got == eventId) answer(true);
            });
    connect(conn, &QObject::destroyed, waiter, [answer]() { answer(false); });
    QTimer::singleShot(kPushedEventWaitMs, waiter, [answer]() { answer(false); });
    conn->resyncNow();
}

void PushManager::onTapped(const QString& roomId, const QString& eventId)
{
    const QString link = QStringLiteral("bsfchat://push/%1/%2")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(roomId)),
             QString::fromUtf8(QUrl::toPercentEncoding(eventId)));
    m_serverManager->openMessageLink(link);
}
