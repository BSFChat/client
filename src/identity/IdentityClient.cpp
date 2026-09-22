#include "identity/IdentityClient.h"
#include "identity/OidcRequest.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

IdentityClient::IdentityClient(QObject* parent)
    : QObject(parent)
{
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this]() {
        cancel();
        emit loginFailed("Login timed out");
    });
}

IdentityClient::~IdentityClient()
{
    cancel();
}

bool IdentityClient::isActive() const
{
    return m_server && m_server->isListening();
}

void IdentityClient::startLogin(const QString& providerUrl, const QString& resource)
{
    // Clean up any previous attempt
    cancel();

    m_providerUrl = providerUrl;
    while (m_providerUrl.endsWith('/'))
        m_providerUrl.chop(1);
    m_resource = resource;

    // Generate PKCE parameters
    m_codeVerifier = generateCodeVerifier();
    QString codeChallenge = computeCodeChallenge(m_codeVerifier);
    m_state = generateState();
    m_nonce = oidc::generateNonce();

    // Start local HTTP server
    m_server = new QTcpServer(this);
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        emit loginFailed("Failed to start local callback server");
        delete m_server;
        m_server = nullptr;
        return;
    }
    m_port = m_server->serverPort();

    connect(m_server, &QTcpServer::newConnection, this, &IdentityClient::onNewConnection);

    // Build authorization URL
    QString redirectUri = QString("http://localhost:%1/oauth/callback").arg(m_port);
    // Names the chat server (resource) and this attempt (nonce) — C1; see
    // OidcRequest.h for why the resource must never come from a server.
    QUrl authUrl = oidc::authorizeUrl(m_providerUrl, redirectUri, codeChallenge, m_state,
                                      m_nonce, m_resource);

    // Open browser
    QDesktopServices::openUrl(authUrl);

    // Start 5-minute timeout
    m_timeout.start(5 * 60 * 1000);
}

void IdentityClient::cancel()
{
    m_timeout.stop();
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    m_port = 0;
}

void IdentityClient::onNewConnection()
{
    if (!m_server) return;

    auto* socket = m_server->nextPendingConnection();
    if (!socket) return;

    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
        QByteArray data = socket->readAll();
        QString request = QString::fromUtf8(data);

        // Parse the HTTP request line: GET /path?query HTTP/1.1
        QStringList lines = request.split("\r\n");
        if (lines.isEmpty()) {
            socket->close();
            socket->deleteLater();
            return;
        }

        QStringList parts = lines.first().split(' ');
        if (parts.size() < 2) {
            socket->close();
            socket->deleteLater();
            return;
        }

        QUrl requestUrl("http://localhost" + parts[1]);
        QString path = requestUrl.path();

        if (path != "/oauth/callback") {
            QByteArray response = "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n\r\n"
                                  "<html><body><h2>Not Found</h2></body></html>";
            socket->write(response);
            socket->flush();
            socket->close();
            socket->deleteLater();
            return;
        }

        QUrlQuery query(requestUrl.query());
        QString code = query.queryItemValue("code");
        QString state = query.queryItemValue("state");
        QString error = query.queryItemValue("error");

        // Check for error from provider
        if (!error.isEmpty()) {
            QString errorDesc = query.queryItemValue("error_description");
            QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
                                  "<html><body style=\"font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; "
                                  "display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; "
                                  "background-color: #313338; color: #f2f3f5;\">"
                                  "<div style=\"text-align: center;\">"
                                  "<h2 style=\"color: #ed4245;\">Login Failed</h2>"
                                  "<p>An error occurred during authentication.</p>"
                                  "<p>You can close this tab and try again in BSFChat.</p>"
                                  "</div></body></html>";
            socket->write(response);
            socket->flush();
            socket->close();
            socket->deleteLater();
            cancel();
            emit loginFailed(errorDesc.isEmpty() ? error : errorDesc);
            return;
        }

        // Verify state
        if (state != m_state) {
            QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
                                  "<html><body style=\"font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; "
                                  "display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; "
                                  "background-color: #313338; color: #f2f3f5;\">"
                                  "<div style=\"text-align: center;\">"
                                  "<h2 style=\"color: #ed4245;\">Login Failed</h2>"
                                  "<p>Security validation failed (state mismatch).</p>"
                                  "<p>You can close this tab and try again in BSFChat.</p>"
                                  "</div></body></html>";
            socket->write(response);
            socket->flush();
            socket->close();
            socket->deleteLater();
            cancel();
            emit loginFailed("State mismatch - possible CSRF attack");
            return;
        }

        if (code.isEmpty()) {
            socket->close();
            socket->deleteLater();
            cancel();
            emit loginFailed("No authorization code received");
            return;
        }

        // Send success page
        QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
                              "<html><body style=\"font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; "
                              "display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; "
                              "background-color: #313338; color: #f2f3f5;\">"
                              "<div style=\"text-align: center;\">"
                              "<h2 style=\"color: #57f287;\">Login Successful!</h2>"
                              "<p>You can close this tab and return to BSFChat.</p>"
                              "</div></body></html>";
        socket->write(response);
        socket->flush();
        socket->close();
        socket->deleteLater();

        // Stop the server and timeout
        m_timeout.stop();
        if (m_server) {
            m_server->close();
            m_server->deleteLater();
            m_server = nullptr;
        }

        // Exchange code for tokens
        exchangeCodeForTokens(code);
    });
}

void IdentityClient::exchangeCodeForTokens(const QString& code)
{
    QString redirectUri = QString("http://localhost:%1/oauth/callback").arg(m_port);

    QUrl tokenUrl(m_providerUrl + "/token");
    QNetworkRequest request(tokenUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");

    QUrlQuery body;
    body.addQueryItem("grant_type", "authorization_code");
    body.addQueryItem("code", code);
    body.addQueryItem("redirect_uri", redirectUri);
    body.addQueryItem("client_id", "bsfchat-desktop");
    body.addQueryItem("code_verifier", m_codeVerifier);
    // RFC 8707 2.2: repeated at the token endpoint; the provider refuses it
    // unless it matches the grant, so a mixed-up code cannot be redeemed for
    // a different server's token.
    if (!m_resource.isEmpty()) body.addQueryItem("resource", m_resource);

    auto* reply = m_nam.post(request, body.toString(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();

        if (reply->error() != QNetworkReply::NoError) {
            const int status = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            QString msg = QString("Token exchange failed [%1]: %2")
                .arg(status).arg(reply->errorString());
            if (!data.isEmpty()) {
                msg += " — " + QString::fromUtf8(data).left(200);
            }
            qWarning().noquote() << "[IdentityClient]" << msg;
            emit loginFailed(msg);
            return;
        }
        qDebug().noquote() << "[IdentityClient] token exchange OK";

        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isObject()) {
            emit loginFailed("Invalid token response");
            return;
        }

        QJsonObject obj = doc.object();
        QString idToken = obj.value("id_token").toString();
        QString accessToken = obj.value("access_token").toString();
        QString refreshToken = obj.value("refresh_token").toString();

        if (idToken.isEmpty() && accessToken.isEmpty()) {
            QString error = obj.value("error_description").toString();
            if (error.isEmpty()) error = obj.value("error").toString();
            if (error.isEmpty()) error = "No tokens in response";
            emit loginFailed(error);
            return;
        }

        // A sign-in for a chat server needs an id_token, and it must be the
        // one this attempt asked for before it goes anywhere. The server-list
        // sync (no resource) uses only the access token and never posts the
        // id_token, so it is not held to this — which also keeps it working
        // against an identity provider from before nonce support.
        if (!m_resource.isEmpty()) {
            if (idToken.isEmpty()) {
                emit loginFailed("The identity provider returned no identity token");
                return;
            }
            const QString why = oidc::checkIdToken(idToken, m_resource, m_nonce);
            if (!why.isEmpty()) {
                qWarning().noquote() << "[IdentityClient] refused id_token:" << why;
                emit loginFailed(why);
                return;
            }
        }

        emit loginCompleted(idToken, accessToken, refreshToken);
    });
}

QString IdentityClient::generateCodeVerifier()
{
    // 43 characters from [A-Za-z0-9-._~]
    static const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    static const int charsetLen = sizeof(charset) - 1;

    QString verifier;
    verifier.reserve(43);
    auto* rng = QRandomGenerator::global();
    for (int i = 0; i < 43; ++i) {
        verifier.append(QLatin1Char(charset[rng->bounded(charsetLen)]));
    }
    return verifier;
}

QString IdentityClient::computeCodeChallenge(const QString& verifier)
{
    QByteArray hash = QCryptographicHash::hash(verifier.toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(hash.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QString IdentityClient::generateState()
{
    // 32 hex characters = 16 random bytes
    QByteArray bytes(16, Qt::Uninitialized);
    QRandomGenerator::global()->fillRange(reinterpret_cast<quint32*>(bytes.data()), 4);
    return QString::fromLatin1(bytes.toHex());
}
