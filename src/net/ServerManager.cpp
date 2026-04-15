#include "net/ServerManager.h"
#include "net/ServerConnection.h"
#include "net/MatrixClient.h"
#include "model/ServerListModel.h"
#include "core/Settings.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

ServerManager::ServerManager(Settings* settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_serverListModel(new ServerListModel(this))
{
    // Restore saved servers
    auto saved = m_settings->savedServers();
    for (const auto& entry : saved) {
        auto* conn = new ServerConnection(entry.url, this);
        conn->setCredentials(entry.userId, entry.accessToken, entry.deviceId, entry.displayName);
        m_connections.append(conn);
        QUrl url(entry.url);
        QString serverName = url.host().isEmpty() ? entry.displayName : url.host();
        m_serverListModel->addServer(serverName, entry.url);
    }

    int savedIndex = m_settings->activeServerIndex();
    if (savedIndex >= 0 && savedIndex < m_connections.size()) {
        setActiveServer(savedIndex);
    }
}

ServerManager::~ServerManager() = default;

void ServerManager::addServer(const QString& url, const QString& username, const QString& password)
{
    auto* conn = new ServerConnection(url, this);
    m_connections.append(conn);
    m_serverListModel->addServer(url, url); // Temporary name until login

    int index = m_connections.size() - 1;

    connect(conn, &ServerConnection::loginSucceeded, this, [this, conn, index]() {
        onLoginSuccess(conn);
    });
    connect(conn, &ServerConnection::loginFailed, this, [this, conn](const QString& error) {
        onLoginFailed(conn, error);
    });

    conn->login(username, password);
    emit serverAdded(index);

    // Auto-select if first server
    if (m_connections.size() == 1) {
        setActiveServer(0);
    }
}

void ServerManager::registerServer(const QString& url, const QString& username, const QString& password)
{
    auto* conn = new ServerConnection(url, this);
    m_connections.append(conn);
    m_serverListModel->addServer(url, url);

    int index = m_connections.size() - 1;

    connect(conn, &ServerConnection::registerSucceeded, this, [this, conn, index]() {
        onLoginSuccess(conn);
    });
    connect(conn, &ServerConnection::registerFailed, this, [this, conn](const QString& error) {
        onLoginFailed(conn, error);
    });

    conn->registerUser(username, password);
    emit serverAdded(index);

    if (m_connections.size() == 1) {
        setActiveServer(0);
    }
}

void ServerManager::checkLoginFlows(const QString& url)
{
    auto* tempClient = new MatrixClient(this);
    tempClient->setHomeserver(url);
    tempClient->getLoginFlows();

    connect(tempClient, &MatrixClient::loginFlowsResult, this, [this, url, tempClient](const QJsonArray& flows) {
        bool oidcAvailable = false;
        bool passwordAvailable = false;
        QString providerUrl;

        for (const auto& flowVal : flows) {
            QJsonObject flow = flowVal.toObject();
            QString type = flow.value("type").toString();
            if (type == "m.login.token") {
                oidcAvailable = true;
                providerUrl = flow.value("identity_provider").toString();
            } else if (type == "m.login.password") {
                passwordAvailable = true;
            }
        }

        emit loginFlowsChecked(url, oidcAvailable, providerUrl, passwordAvailable);
        tempClient->deleteLater();
    });

    connect(tempClient, &MatrixClient::loginError, this, [this, url, tempClient](const QString& /*error*/) {
        // On error, assume password-only
        emit loginFlowsChecked(url, false, QString(), true);
        tempClient->deleteLater();
    });
}

void ServerManager::addServerWithOidc(const QString& url)
{
    auto* conn = new ServerConnection(url, this);
    m_connections.append(conn);
    m_serverListModel->addServer(url, url);

    int index = m_connections.size() - 1;

    connect(conn, &ServerConnection::loginSucceeded, this, [this, conn]() {
        onLoginSuccess(conn);
    });
    connect(conn, &ServerConnection::loginFailed, this, [this, conn](const QString& error) {
        onLoginFailed(conn, error);
    });

    // Query login flows to get the identity_provider URL, then start OIDC
    auto* tempClient = new MatrixClient(this);
    tempClient->setHomeserver(url);
    tempClient->getLoginFlows();

    connect(tempClient, &MatrixClient::loginFlowsResult, this, [this, conn, tempClient](const QJsonArray& flows) {
        QString providerUrl;
        for (const auto& flowVal : flows) {
            QJsonObject flow = flowVal.toObject();
            if (flow.value("type").toString() == "m.login.token") {
                providerUrl = flow.value("identity_provider").toString();
                break;
            }
        }
        tempClient->deleteLater();

        if (providerUrl.isEmpty()) {
            emit loginError(conn->serverUrl(), "Server does not provide an identity provider URL");
            return;
        }
        conn->loginWithOidc(providerUrl);
    });

    connect(tempClient, &MatrixClient::loginError, this, [this, conn, tempClient](const QString& error) {
        tempClient->deleteLater();
        onLoginFailed(conn, "Failed to query login flows: " + error);
    });

    emit serverAdded(index);

    if (m_connections.size() == 1) {
        setActiveServer(0);
    }
}

void ServerManager::removeServer(int index)
{
    if (index < 0 || index >= m_connections.size()) return;

    auto* conn = m_connections.takeAt(index);
    conn->disconnectFromServer();
    conn->deleteLater();

    m_serverListModel->removeServer(index);
    m_settings->removeServer(index);

    if (m_activeServerIndex == index) {
        int newIndex = m_connections.isEmpty() ? -1 : qMin(index, m_connections.size() - 1);
        setActiveServer(newIndex);
    } else if (m_activeServerIndex > index) {
        m_activeServerIndex--;
        m_settings->setActiveServerIndex(m_activeServerIndex);
    }

    emit serverRemoved(index);
}

void ServerManager::setActiveServer(int index)
{
    if (index < -1 || index >= m_connections.size()) return;
    if (m_activeServerIndex == index) return;

    m_activeServerIndex = index;
    m_activeServer = (index >= 0) ? m_connections[index] : nullptr;
    m_settings->setActiveServerIndex(index);
    emit activeServerChanged();
}

void ServerManager::onLoginSuccess(ServerConnection* conn)
{
    int index = m_connections.indexOf(conn);
    if (index < 0) return;

    // Use the server hostname as display name for the sidebar icon
    QUrl url(conn->serverUrl());
    QString serverDisplayName = url.host();
    if (serverDisplayName.isEmpty()) serverDisplayName = conn->serverUrl();
    m_serverListModel->updateServer(index, serverDisplayName, conn->serverUrl());
    emit loginSuccess(conn->serverUrl());

    Settings::ServerEntry entry;
    entry.url = conn->serverUrl();
    entry.userId = conn->userId();
    entry.accessToken = conn->accessToken();
    entry.deviceId = conn->deviceId();
    entry.displayName = conn->displayName();
    m_settings->addServer(entry);
}

void ServerManager::onLoginFailed(ServerConnection* conn, const QString& error)
{
    int index = m_connections.indexOf(conn);
    emit loginError(conn->serverUrl(), error);

    // Remove the failed connection
    if (index >= 0) {
        removeServer(index);
    }
}
