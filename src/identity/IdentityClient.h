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

    void startLogin(const QString& providerUrl);
    void cancel();
    bool isActive() const;

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
    int m_port = 0;
};
