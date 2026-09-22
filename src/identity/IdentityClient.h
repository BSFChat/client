#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QTimer>
#include <QString>

class IdentityClient : public QObject {
    Q_OBJECT
public:
    explicit IdentityClient(QObject* parent = nullptr);
    ~IdentityClient() override;

    // `resource` is the canonical URL of the chat server the id_token is for
    // (oidc::resourceForHomeserver of the address it will be posted to), or
    // empty for a sign-in that is not for a chat server — the account-level
    // server-list sync, which only uses the access token. See OidcRequest.h.
    void startLogin(const QString& providerUrl, const QString& resource = QString());
    void cancel();
    bool isActive() const;
    QString providerUrl() const { return m_providerUrl; }

signals:
    void loginCompleted(const QString& idToken, const QString& accessToken, const QString& refreshToken);
    void loginFailed(const QString& error);

private:
    void onNewConnection();
    void exchangeCodeForTokens(const QString& code);
    QString generateCodeVerifier();
    QString computeCodeChallenge(const QString& verifier);
    QString generateState();

    QTcpServer* m_server = nullptr;
    QNetworkAccessManager m_nam;
    QTimer m_timeout;
    QString m_providerUrl;
    QString m_codeVerifier;
    QString m_state;
    QString m_nonce;
    QString m_resource;
    int m_port = 0;
};
