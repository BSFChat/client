#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QTimer>
#include <QString>
#include <QUrl>

// How the authorization callback gets back to us, per platform. RFC 8252
// offers two shapes and this client needs both.
//
//   Desktop (macOS / Windows / Linux) — loopback redirect (§7.3): open the
//     system browser and listen on a QTcpServer bound to an ephemeral port.
//     Unchanged by this file's mobile work.
//
//   iOS and Android — private-use URI scheme (§7.1), `bsfchat://oauth/callback`.
//     Neither platform can use the loopback redirect, for reasons that look
//     different and amount to the same thing: the listening socket stops
//     being serviced the moment the browser takes the foreground.
//
//       * iOS suspends the app outright when Safari comes forward.
//       * Android pauses the activity, and Qt's Android event dispatcher
//         responds by excluding socket notifiers and timers from
//         processEvents (QAndroidEventDispatcherStopper), then blocking the
//         main thread entirely on ApplicationSuspended. The browser's
//         connection completes into the listen backlog and is never
//         accepted.
//
//     On both, sign-in hung on "Waiting for browser login…" until the
//     five-minute timeout — which on Android is itself frozen, so it did not
//     even time out until the user came back by hand.
//
//     They differ only in how the page is shown and the redirect collected:
//       * iOS presents ASWebAuthenticationSession, which keeps the app in
//         the foreground and hands the redirect straight to a completion
//         handler (src/identity/IosAuthSession.h).
//       * Android keeps QDesktopServices::openUrl — which maps to an
//         ACTION_VIEW intent — and the redirect comes back as an
//         ACTION_VIEW intent of our own, through UrlHandler.
//
// Every transport converges on handleCallback(), so state and PKCE are
// validated by the same code on every platform (oidc::parseCallbackQuery).
#if defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
#  define BSFCHAT_NATIVE_OIDC_REDIRECT 1
#endif

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

#ifdef BSFCHAT_NATIVE_OIDC_REDIRECT
    // Offer an inbound `bsfchat://…` URL to the sign-in. Returns true if it
    // was a sign-in callback and has been dealt with; false if it is an
    // ordinary deep link the caller should still open as a message.
    //
    // This is the whole delivery path on Android: the provider redirects to
    // bsfchat://oauth/callback, the OS resolves the intent-filter back to us,
    // and UrlHandler surfaces it as urlReceived.
    //
    // On iOS it is the belt to ASWebAuthenticationSession's braces — the
    // session normally intercepts the redirect before the OS ever sees it —
    // covering a redirect chain that escapes the sheet, or a future move to
    // universal links.
    //
    // Not compiled on desktop, on purpose. There the loopback server is the
    // only way in, and exposing a second one would let any web page fire a
    // `bsfchat://oauth/callback?…` at a sign-in in progress. (It would be
    // refused on `state`, but an attack surface that buys nothing is one not
    // worth having.)
    static bool deliverCallbackUrl(const QString& url);
#endif

signals:
    void loginCompleted(const QString& idToken, const QString& accessToken, const QString& refreshToken);
    void loginFailed(const QString& error);

private:
#ifndef BSFCHAT_NATIVE_OIDC_REDIRECT
    void onNewConnection();
#endif
    // The single place a parsed callback is acted on, whichever transport
    // delivered it: validates via oidc::parseCallbackQuery, then either
    // exchanges the code or fails the sign-in with a reason.
    void handleCallback(const QUrl& callbackUrl);
    void exchangeCodeForTokens(const QString& code);
    // The redirect_uri this attempt asked for — loopback with the live port
    // on desktop, the private-use scheme on iOS. /authorize and /token must
    // send the identical string, so both read it from here.
    QString redirectUri() const;
    QString generateCodeVerifier();
    QString computeCodeChallenge(const QString& verifier);
    QString generateState();

#ifdef BSFCHAT_NATIVE_OIDC_REDIRECT
    // What isActive() reports where there is no listening socket to ask:
    // true from startLogin() until the callback, a failure, or cancel().
    bool m_sessionActive = false;
#else
    QTcpServer* m_server = nullptr;
#endif
    QNetworkAccessManager m_nam;
    QTimer m_timeout;
    QString m_providerUrl;
    QString m_codeVerifier;
    QString m_state;
    QString m_nonce;
    QString m_resource;
    int m_port = 0;
};
