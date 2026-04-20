#pragma once

#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

// Thin wrapper around the identity service's /api/servers endpoints.
// Authenticates via an OIDC access token (the identity service stores these
// as opaque session IDs, so its get_session_account() accepts any access
// token the service itself has issued — regardless of the token's intended
// audience. See identity/src/api/AccountHandler.cpp).
class IdentityApiClient : public QObject {
    Q_OBJECT
public:
    IdentityApiClient(const QString& baseUrl, const QString& accessToken,
                      QObject* parent = nullptr);

    void fetchServers();
    void registerServer(const QString& serverUrl, const QString& serverName);
    void unregisterServer(const QString& serverUrl);

    QString baseUrl() const { return m_baseUrl; }
    QString accessToken() const { return m_accessToken; }
    void setAccessToken(const QString& token) { m_accessToken = token; }

signals:
    // Each object in the array has keys: id, server_url, server_name, joined_at.
    void serversFetched(const QJsonArray& servers);
    void fetchFailed(const QString& error);
    void serverRegistered(const QString& serverUrl);
    void registerFailed(const QString& serverUrl, const QString& error);
    void serverUnregistered(const QString& serverUrl);
    void unregisterFailed(const QString& serverUrl, const QString& error);

private:
    QString m_baseUrl;       // e.g. "https://id.bsfchat.com"
    QString m_accessToken;
    QNetworkAccessManager m_nam;
};
