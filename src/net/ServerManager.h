#pragma once

#include <QObject>
#include <QList>

class ServerConnection;
class ServerListModel;
class Settings;
class MatrixClient;

class ServerManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(ServerListModel* servers READ servers CONSTANT)
    Q_PROPERTY(ServerConnection* activeServer READ activeServer NOTIFY activeServerChanged)
    Q_PROPERTY(int activeServerIndex READ activeServerIndex WRITE setActiveServer NOTIFY activeServerChanged)

public:
    explicit ServerManager(Settings* settings, QObject* parent = nullptr);
    ~ServerManager() override;

    ServerListModel* servers() const { return m_serverListModel; }
    ServerConnection* activeServer() const { return m_activeServer; }
    int activeServerIndex() const { return m_activeServerIndex; }

    Q_INVOKABLE void addServer(const QString& url, const QString& username, const QString& password);
    Q_INVOKABLE void addServerWithOidc(const QString& url);
    Q_INVOKABLE void checkLoginFlows(const QString& url);
    Q_INVOKABLE void registerServer(const QString& url, const QString& username, const QString& password);

    // Identity-first login: authenticate to the identity provider directly,
    // then fetch the user's server-membership list and auto-connect each.
    Q_INVOKABLE void loginWithIdentity(const QString& identityUrl);
    // After successfully connecting to a server via OIDC, register the
    // membership with the identity provider so future logins restore it.
    Q_INVOKABLE void registerServerMembership(const QString& identityUrl,
                                               const QString& serverUrl,
                                               const QString& serverName);
    Q_INVOKABLE void removeServer(int index);
    Q_INVOKABLE void setActiveServer(int index);

signals:
    void activeServerChanged();
    void serverAdded(int index);
    void serverRemoved(int index);
    void loginError(const QString& serverUrl, const QString& error);
    void loginSuccess(const QString& serverUrl);
    void loginFlowsChecked(const QString& url, bool oidcAvailable, const QString& providerUrl, bool passwordAvailable);
    void identityLoginComplete(const QStringList& serverUrls);
    void identityLoginFailed(const QString& error);

private:
    void onLoginSuccess(ServerConnection* conn);
    void onLoginFailed(ServerConnection* conn, const QString& error);
    // Hooks up a ServerConnection so per-server UI state (unread dot, etc.)
    // in the sidebar tracks the connection's state.
    void wireConnection(ServerConnection* conn);

    Settings* m_settings;
    ServerListModel* m_serverListModel;
    QList<ServerConnection*> m_connections;
    ServerConnection* m_activeServer = nullptr;
    int m_activeServerIndex = -1;
};
