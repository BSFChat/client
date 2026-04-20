#include "identity/IdentityApiClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {

// Strip a single trailing slash so callers can pass either form.
QString normalizeBase(QString url)
{
    while (url.endsWith('/'))
        url.chop(1);
    return url;
}

} // namespace

IdentityApiClient::IdentityApiClient(const QString& baseUrl, const QString& accessToken,
                                     QObject* parent)
    : QObject(parent)
    , m_baseUrl(normalizeBase(baseUrl))
    , m_accessToken(accessToken)
{
}

void IdentityApiClient::fetchServers()
{
    QNetworkRequest request(QUrl(m_baseUrl + "/api/servers"));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + m_accessToken.toUtf8());
    request.setRawHeader("Accept", "application/json");

    auto* reply = m_nam.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();

        if (reply->error() != QNetworkReply::NoError) {
            emit fetchFailed(QStringLiteral("Fetch servers failed: %1 (%2)")
                                 .arg(reply->errorString(), QString::fromUtf8(data)));
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isArray()) {
            emit fetchFailed(QStringLiteral("Invalid server list response"));
            return;
        }
        emit serversFetched(doc.array());
    });
}

void IdentityApiClient::registerServer(const QString& serverUrl, const QString& serverName)
{
    QNetworkRequest request(QUrl(m_baseUrl + "/api/servers"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QByteArray("Bearer ") + m_accessToken.toUtf8());

    QJsonObject body;
    body["server_url"] = serverUrl;
    body["server_name"] = serverName;
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    auto* reply = m_nam.post(request, payload);
    connect(reply, &QNetworkReply::finished, this, [this, reply, serverUrl]() {
        reply->deleteLater();
        auto data = reply->readAll();

        if (reply->error() != QNetworkReply::NoError) {
            emit registerFailed(serverUrl,
                                QStringLiteral("%1 (%2)").arg(reply->errorString(),
                                                              QString::fromUtf8(data)));
            return;
        }
        emit serverRegistered(serverUrl);
    });
}

void IdentityApiClient::unregisterServer(const QString& serverUrl)
{
    // Matches the identity API: POST /api/servers/remove with {server_url}.
    QNetworkRequest request(QUrl(m_baseUrl + "/api/servers/remove"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QByteArray("Bearer ") + m_accessToken.toUtf8());

    QJsonObject body;
    body["server_url"] = serverUrl;
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    auto* reply = m_nam.post(request, payload);
    connect(reply, &QNetworkReply::finished, this, [this, reply, serverUrl]() {
        reply->deleteLater();
        auto data = reply->readAll();

        if (reply->error() != QNetworkReply::NoError) {
            emit unregisterFailed(serverUrl,
                                  QStringLiteral("%1 (%2)").arg(reply->errorString(),
                                                                QString::fromUtf8(data)));
            return;
        }
        emit serverUnregistered(serverUrl);
    });
}
