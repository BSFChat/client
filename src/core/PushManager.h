// Glue between the platform push token (PushBridge), the per-account
// pusher bookkeeping (PushRegistrar), the settings and the live server
// connections. See docs/push-notifications.md.
//
//   * Every signed-in, connected account gets a pusher once a token is
//     known and push is wanted (notifications on, and on Android the user
//     has not chosen a background connection instead).
//   * Sign-out deletes the account's pusher on the session before it is
//     revoked (ServerManager's pusher-deletion hook).
//   * A pushed event arriving while this process runs (Android) kicks that
//     server's /sync; the FCM service waits for the answer and posts a
//     generic notification only if the event did not come in.
//   * A tapped push notification opens its room.
//
// Exposed to QML as `pushManager` for the settings rows.
#pragma once

#include "net/PushRegistrar.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

class ServerConnection;
class ServerManager;
class Settings;
class PushBridge;

class PushManager : public QObject, private bsfchat::client::push::PushTransport {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    // Push is wanted and the platform has handed us a token.
    Q_PROPERTY(bool active READ active NOTIFY statusChanged)
    // One line for the settings pane, e.g. "Push is on for 2 servers;
    // example.org does not allow the BSFChat push gateway."
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    // Android: whether the persistent SyncService should run — the user's
    // choice, or the fallback when push cannot work on this device.
    Q_PROPERTY(bool backgroundSyncWanted READ backgroundSyncWanted
               NOTIFY backgroundSyncWantedChanged)
public:
    PushManager(ServerManager* serverManager, Settings* settings, QObject* parent = nullptr);
    ~PushManager() override;

    bool available() const;
    bool active() const;
    QString statusText() const;
    bool backgroundSyncWanted() const;

    // ServerManager::removeServer's hook: the deletion bodies owed for this
    // account, serialised, and the account forgotten.
    QList<QByteArray> takeDeletions(ServerConnection* conn);

    PushRegistrar* registrar() const { return m_registrar; }

signals:
    void statusChanged();
    void backgroundSyncWantedChanged();

private:
    void setPusher(const QString& account, const QJsonObject& body, Done done) override;

    static QString accountKey(const ServerConnection* conn);
    void watch(ServerConnection* conn);
    void evaluate(ServerConnection* conn);
    ServerConnection* connectionFor(const QString& account) const;
    ServerConnection* connectionForRoom(const QString& roomId) const;
    void applySettings();
    void onPushedEvent(const QString& roomId, const QString& eventId, int unread);
    void onTapped(const QString& roomId, const QString& eventId);
    bool pushWanted() const;

    QPointer<ServerManager> m_serverManager;
    Settings* m_settings;
    PushBridge* m_bridge;
    PushRegistrar* m_registrar;
    // Live connection -> the account key it was registered under, so a
    // destroyed connection can still be dropped by key.
    QHash<ServerConnection*, QString> m_keys;
    QHash<ServerConnection*, QStringList> m_recent;
    bool m_lastBackgroundSyncWanted = false;
};
