#pragma once
// The app's own BSFChat ID session: sign in once, then join servers without
// the browser.
//
// After one browser sign-in the app holds an identity REFRESH token. The
// identity provider's refresh grant accepts a `resource` (a chat server's
// URL) and a nonce from the first-party apps and returns an id_token for
// exactly that server, so joining a server is one request, not a browser
// trip and a consent page. Every refresh also rotates the token.
//
// Two rules this class exists to keep:
//
//   1. ONE REFRESH AT A TIME. Refresh tokens are single-use, and presenting
//      an already-spent one is treated by the provider as theft: the whole
//      grant is revoked and the user is signed out everywhere. Two joins
//      started together must therefore queue, each using the token the
//      previous one was handed. Requests are FIFO through one slot.
//
//   2. PERSIST THE NEW TOKEN BEFORE USING WHAT IT BOUGHT. The rotated token
//      is written to the secure store before any callback runs, so a crash
//      between "the provider rotated" and "we saved it" cannot strand the
//      app holding a spent token.
//
// The refresh token lives in SecretStore (the OS keychain) and nowhere else.
// If the keychain is unavailable it is kept in memory for this run only.

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

#include <functional>

class QNetworkAccessManager;
class SecretStore;

class IdentitySession : public QObject {
    Q_OBJECT
public:
    // POSTs a form to the token endpoint; calls `done` with the HTTP status
    // (0 for a transport failure) and the body. Replaceable for tests.
    using Transport = std::function<void(const QUrl& tokenUrl, const QUrlQuery& form,
                                         std::function<void(int status, const QByteArray& body)> done)>;
    using ServerTokenCallback = std::function<void(const QString& idToken, const QString& error)>;
    using AccessTokenCallback = std::function<void(const QString& accessToken, const QString& error)>;

    explicit IdentitySession(QObject* parent = nullptr);

    void setTransport(Transport transport) { m_transport = std::move(transport); }
    void setStore(SecretStore* store) { m_store = store; }

    // Load a refresh token saved by an earlier run. True when one was found.
    bool restore(const QString& providerUrl);
    // Take over the refresh token from a browser sign-in, and save it.
    void adopt(const QString& providerUrl, const QString& refreshToken);
    // Forget it locally (sign out of BSFChat ID on this device).
    void signOut();

    bool isSignedIn() const { return !m_refreshToken.isEmpty(); }
    // Whether the token is in the secure store (false = memory only).
    bool isPersisted() const { return m_persisted; }
    QString providerUrl() const { return m_providerUrl; }
    // Does a server whose login flows name `serverProvider` trust this ID?
    bool servesProvider(const QString& serverProvider) const;

    // An id_token for one chat server (`resource` = oidc::resourceForHomeserver),
    // checked for audience and nonce before it is handed back.
    void requestServerToken(const QString& resource, ServerTokenCallback callback);
    // A fresh identity access token (for /api/servers).
    void requestAccessToken(AccessTokenCallback callback);

    static QString storeKey(const QString& providerUrl);
    static QString normaliseProvider(const QString& url);

signals:
    // The provider rejected the refresh token (expired, revoked, replayed):
    // the session is gone and the user must sign in through the browser.
    void signedOut(const QString& reason);

private:
    struct Pending {
        QString resource; // empty = access-token request
        ServerTokenCallback serverCallback;
        AccessTokenCallback accessCallback;
    };
    void pump();
    void complete(const Pending& p, const QString& value, const QString& error);
    void persist();

    Transport m_transport;
    SecretStore* m_store = nullptr;
    QNetworkAccessManager* m_nam = nullptr;
    QString m_providerUrl;
    QString m_refreshToken;
    bool m_persisted = false;
    bool m_busy = false;
    QList<Pending> m_queue;
};
