#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QTimer>
#include <QString>
#include <QUrl>

#include <functional>

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
    //
    // ONE AT A TIME, PROCESS-WIDE. There is a single browser and a single
    // user in front of it, and until 2026-09-24 nothing in this client said
    // so. Several IdentityClients exist at once by design — ServerManager
    // owns one for the account-level sign-in and every ServerConnection owns
    // another — and each of them opened the system browser the moment it was
    // asked to, so an identity sign-in that found N servers fired N
    // authorizations at once and a retry landed on top of whatever was
    // already open.
    //
    // What that does to the provider is not obvious from here, which is why
    // it survived: each authorization mints its own consent prompt bound to
    // the same browser session, the browser ends up showing one of them, and
    // approving it posts a token the provider has already superseded or
    // spent. The provider answers 403 "Authorization request does not belong
    // to this session" — correctly; it is being handed a stale token — and
    // the sign-in dies in a browser tab while the app waits out its
    // five-minute timeout. Traced on a Pixel 6 Pro on 2026-09-24: two Chrome
    // launches 17 ms apart and two `bsfchat://oauth/callback` intents, from a
    // single tap on "Sign in with BSFChat ID".
    //
    // So: a request that arrives while another one owns the browser WAITS,
    // and is opened when that one finishes. A request for a provider and
    // resource some in-flight attempt is already running is refused outright
    // via loginFailed, because a second browser trip for the same server is
    // not something the user should be made to sit through twice.
    //
    // Queueing rather than refusing matters for more than tidiness: an
    // id_token is audience-bound to exactly ONE chat server (see
    // identity/OidcRequest.h), so joining three servers genuinely needs three
    // authorizations. They just have to be three in a row, not three at once.
    void startLogin(const QString& providerUrl, const QString& resource = QString());
    void cancel();
    // True from startLogin() until this attempt ends — including while it is
    // waiting its turn for the browser, so a caller cannot conclude from a
    // false here that it is free to start another one.
    bool isActive() const;
    // True only while waiting for the browser: nothing has been shown to the
    // user yet and no timeout is running.
    bool isWaitingForBrowser() const { return m_queued; }
    QString providerUrl() const { return m_providerUrl; }
    // The chat server this attempt's id_token is for, or empty for the
    // account-level sign-in. Two attempts that agree on both this and
    // providerUrl() are the same sign-in, not two of them.
    QString resource() const { return m_resource; }

    // Test seam for the serialisation above.
    //
    // The alternative is a test that launches the system browser on whichever
    // machine runs the suite, which is not something a unit test gets to do.
    // Installing a presenter replaces the platform's "show the authorization
    // page" step and nothing else — the queue, the PKCE material, the state
    // and the callback routing all still run for real. Production installs
    // none and this is a null check on a cold path.
    static void setPagePresenterForTesting(std::function<void(const QUrl&)> presenter);
    // How many attempts are waiting for the browser right now.
    static int waitingForBrowserCount();

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

    // Mints this attempt's PKCE material, arms the callback transport and
    // shows the authorization page. Split out of startLogin() because an
    // attempt that has to wait for the browser must generate none of it yet:
    // the five-minute timeout, and the window in which a `state` is valid,
    // both start when the page is actually shown, not when it was asked for.
    void openAuthorizationPage();
    // Give the browser up and let the next attempt (if any) have it. Every
    // terminal path runs through here — cancel(), and the callback, which
    // releases as soon as it has the code rather than holding on through the
    // token exchange.
    void releaseBrowser();
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
    // Asked for, but another attempt owns the browser. Nothing has been
    // shown, no PKCE material exists yet, and no timeout is running.
    bool m_queued = false;
};
