#include "identity/IdentityClient.h"
#include "identity/OidcRequest.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRandomGenerator>
#include <QUrl>
#include <QUrlQuery>

#ifdef Q_OS_IOS
#  include "identity/IosAuthSession.h"
#endif
#ifndef BSFCHAT_NATIVE_OIDC_REDIRECT
#  include <QTcpSocket>
#endif

namespace {

// Who owns the browser, and who is waiting for it. See the long note on
// IdentityClient::startLogin: there is one browser and one user, several
// IdentityClients exist at once by design, and before 2026-09-24 nothing
// said so — which is how a single tap on "Sign in with BSFChat ID" opened
// Chrome twice and left the provider refusing a superseded consent token.
//
// QPointer throughout: a queued attempt whose owner is destroyed while it
// waits becomes null and is skipped rather than resurrected.
QPointer<IdentityClient> g_browserOwner;
QList<QPointer<IdentityClient>> g_browserQueue;

// Null in production; see setPagePresenterForTesting.
std::function<void(const QUrl&)> g_pagePresenter;

// Is some other live attempt already signing in to the same place? Two
// authorizations differing only in `resource` are two different sign-ins and
// both are needed; two with the SAME resource are the same sign-in twice.
bool sameSignInAlreadyRunning(const IdentityClient* self, const QString& providerUrl,
                              const QString& resource)
{
    const auto matches = [&](const IdentityClient* other) {
        return other && other != self && other->providerUrl() == providerUrl
            && other->resource() == resource;
    };
    if (matches(g_browserOwner)) return true;
    for (const auto& waiting : g_browserQueue) {
        if (matches(waiting)) return true;
    }
    return false;
}

} // namespace

#ifdef BSFCHAT_NATIVE_OIDC_REDIRECT
namespace {
// The sign-in an OS-delivered `bsfchat://oauth/callback` belongs to.
//
// At most one at a time, and now that is enforced rather than assumed:
// g_browserOwner above guarantees only one attempt has a page open, so this
// pointer cannot be stolen from an attempt still waiting on its redirect.
// A QPointer so a deleted client cannot be called into. Cleared in cancel(),
// which every terminal path runs through.
QPointer<IdentityClient> g_awaitingCallback;
} // namespace
#endif

void IdentityClient::setPagePresenterForTesting(std::function<void(const QUrl&)> presenter)
{
    g_pagePresenter = std::move(presenter);
}

int IdentityClient::waitingForBrowserCount()
{
    int waiting = 0;
    for (const auto& client : g_browserQueue) {
        if (client) ++waiting;
    }
    return waiting;
}

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
    // Waiting for the browser counts. A caller that asked for a sign-in and
    // has been told nothing yet must not be able to read "not active" here
    // and start a second one — that is the shape of the bug this file's
    // serialisation exists to stop.
    if (m_queued) return true;
#ifdef BSFCHAT_NATIVE_OIDC_REDIRECT
    return m_sessionActive;
#else
    return m_server && m_server->isListening();
#endif
}

void IdentityClient::releaseBrowser()
{
    m_queued = false;
    g_browserQueue.removeAll(QPointer<IdentityClient>(this));
    if (g_browserOwner != this) return;

    g_browserOwner = nullptr;
    while (!g_browserQueue.isEmpty()) {
        QPointer<IdentityClient> next = g_browserQueue.takeFirst();
        if (!next) continue;   // its owner was destroyed while it waited
        g_browserOwner = next;
        // Through the event loop, not straight down the stack. This runs from
        // cancel(), which runs from destructors and from inside signal
        // handlers; opening the next page from there would re-enter this
        // function on a half-torn-down object. A zero timer costs nothing and
        // makes the ordering obvious.
        QTimer::singleShot(0, next, [next]() {
            if (!next || g_browserOwner != next) return;
            next->openAuthorizationPage();
        });
        return;
    }
}

QString IdentityClient::redirectUri() const
{
#ifdef BSFCHAT_NATIVE_OIDC_REDIRECT
    return QString::fromLatin1(oidc::kNativeRedirectUri);
#else
    return QString("http://localhost:%1/oauth/callback").arg(m_port);
#endif
}

void IdentityClient::startLogin(const QString& providerUrl, const QString& resource)
{
    // Clean up any previous attempt. Also drops us from the queue and, if we
    // held the browser, hands it on — so a caller restarting its own sign-in
    // never counts as two.
    cancel();

    m_providerUrl = providerUrl;
    while (m_providerUrl.endsWith('/'))
        m_providerUrl.chop(1);
    m_resource = resource;

    if (sameSignInAlreadyRunning(this, m_providerUrl, m_resource)) {
        // Refused rather than queued: making the user approve the same server
        // twice in a row is not a fix for asking twice.
        qWarning().noquote()
            << "[IdentityClient] refusing a second sign-in for" << m_providerUrl
            << (m_resource.isEmpty() ? QStringLiteral("(account)") : m_resource)
            << "- one is already in flight";
        emit loginFailed(tr("A BSFChat ID sign-in for this server is already open. "
                            "Finish it in your browser, or cancel it and try again."));
        return;
    }

    if (g_browserOwner && g_browserOwner != this) {
        // Wait our turn. Nothing is generated and no timeout starts until the
        // page is actually shown — see openAuthorizationPage.
        m_queued = true;
        g_browserQueue.append(this);
        return;
    }

    g_browserOwner = this;
    openAuthorizationPage();
}

void IdentityClient::openAuthorizationPage()
{
    m_queued = false;

    // Generate PKCE parameters
    m_codeVerifier = generateCodeVerifier();
    QString codeChallenge = computeCodeChallenge(m_codeVerifier);
    m_state = generateState();
    m_nonce = oidc::generateNonce();

#ifndef BSFCHAT_NATIVE_OIDC_REDIRECT
    // Start local HTTP server
    m_server = new QTcpServer(this);
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        delete m_server;
        m_server = nullptr;
        // Give the browser up before reporting, or an attempt that never
        // opened a page would hold the turn it could not use.
        releaseBrowser();
        emit loginFailed("Failed to start local callback server");
        return;
    }
    m_port = m_server->serverPort();

    connect(m_server, &QTcpServer::newConnection, this, &IdentityClient::onNewConnection);
#endif

    // Build authorization URL
    // Names the chat server (resource) and this attempt (nonce) — C1; see
    // OidcRequest.h for why the resource must never come from a server.
    QUrl authUrl = oidc::authorizeUrl(m_providerUrl, redirectUri(), codeChallenge, m_state,
                                      m_nonce, m_resource);

#ifdef BSFCHAT_NATIVE_OIDC_REDIRECT
    // Arm the callback route before the page is shown: on Android the OS can
    // bring us back with the redirect intent, and there must be no window in
    // which that arrives with nothing waiting for it.
    g_awaitingCallback = this;
    m_sessionActive = true;
#endif

#ifdef Q_OS_IOS
    // Present in-process. QDesktopServices::openUrl here would hand off to
    // Safari and suspend us, which is the bug this branch exists to fix.
    const bool presented = ios_auth_session::start(
        authUrl, QString::fromLatin1(oidc::kNativeCallbackScheme),
        [this](ios_auth_session::Outcome outcome, const QUrl& callbackUrl,
               const QString& error) {
            switch (outcome) {
            case ios_auth_session::Outcome::Callback:
                handleCallback(callbackUrl);
                return;
            case ios_auth_session::Outcome::Cancelled:
                cancel();
                // Said out loud. The caller closes its spinner on
                // loginFailed, and a cancel that emitted nothing would
                // reproduce the exact "Waiting for browser login…" hang
                // this change removes.
                emit loginFailed(tr("Sign-in was cancelled"));
                return;
            case ios_auth_session::Outcome::Failed:
                cancel();
                emit loginFailed(error.isEmpty()
                                     ? tr("The sign-in sheet could not be opened")
                                     : error);
                return;
            }
        });
    if (!presented) {
        cancel();
        emit loginFailed(tr("The sign-in sheet could not be opened"));
        return;
    }
#else
    // Desktop and Android both open the system browser. On Android that maps
    // to an ACTION_VIEW intent, and the redirect comes back the same way —
    // see the intent-filter in android/AndroidManifest.xml and
    // UrlHandler::checkAndroidLaunchIntent().
    if (g_pagePresenter) {
        g_pagePresenter(authUrl);
    } else {
        QDesktopServices::openUrl(authUrl);
    }
#endif

    // Start 5-minute timeout
    m_timeout.start(5 * 60 * 1000);
}

void IdentityClient::cancel()
{
    m_timeout.stop();
#ifdef BSFCHAT_NATIVE_OIDC_REDIRECT
    m_sessionActive = false;
    // Only tear the sheet down if it is ours: the iOS session is a
    // process-wide singleton, and a stale client's destructor must not
    // dismiss a sign-in somebody else just started.
    if (g_awaitingCallback == this) {
        g_awaitingCallback = nullptr;
#  ifdef Q_OS_IOS
        ios_auth_session::cancel();
#  endif
    }
#else
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
#endif
    m_port = 0;
    // The CSRF guard for an attempt that is over. parseCallbackQuery refuses
    // an empty expected state outright, so a callback that arrives late —
    // after a timeout, a cancel, or a completed sign-in — cannot be replayed
    // into a second token exchange.
    m_state.clear();
    // Last, so everything above is already torn down before the next attempt
    // can be handed the browser.
    releaseBrowser();
}

#ifdef BSFCHAT_NATIVE_OIDC_REDIRECT
bool IdentityClient::deliverCallbackUrl(const QString& url)
{
    const QUrl parsed(url);
    if (!oidc::isNativeCallbackUrl(parsed)) return false; // an ordinary deep link

    if (!g_awaitingCallback) {
        // A sign-in reply with no sign-in waiting for it. On Android this is
        // what a cold start looks like: the process died while the browser
        // had the foreground, so the PKCE verifier — which is deliberately
        // memory-only — died with it and the code cannot be redeemed by
        // anyone, us included. Consumed rather than returned: it is not a
        // message link, and handing it to openMessageLink would send the user
        // looking for a room called "oauth".
        qWarning() << "[IdentityClient] a sign-in callback arrived with no "
                      "attempt in flight; ignoring it";
        return true;
    }

    g_awaitingCallback->handleCallback(parsed);
    return true;
}
#endif

void IdentityClient::handleCallback(const QUrl& callbackUrl)
{
    const oidc::CallbackParse parsed =
        oidc::parseCallbackQuery(QUrlQuery(callbackUrl.query()), m_state);

    if (parsed.status != oidc::CallbackStatus::Code) {
        if (parsed.status == oidc::CallbackStatus::StateMismatch) {
            qWarning().noquote()
                << "[IdentityClient] refused a callback that is not this attempt's";
        }
        cancel();
        emit loginFailed(parsed.error);
        return;
    }

    // Shut the callback transport down before the token request: the attempt
    // is no longer waiting on a browser, and a second callback must not be
    // able to start a second exchange. NOT cancel() — that zeroes m_port, and
    // the desktop /token call has to echo the identical loopback redirect_uri
    // the authorization request carried.
    m_timeout.stop();
    m_state.clear();
#ifdef BSFCHAT_NATIVE_OIDC_REDIRECT
    m_sessionActive = false;
    if (g_awaitingCallback == this) g_awaitingCallback = nullptr;
#else
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
#endif
    // The browser has done its part. Release it HERE rather than after the
    // token exchange: what has to be serialised is the page in front of the
    // user, not the HTTP call behind it, and on mobile the next sign-in is
    // usually the one this code was fetched to make possible.
    releaseBrowser();

    exchangeCodeForTokens(parsed.code);
}

#ifndef BSFCHAT_NATIVE_OIDC_REDIRECT
namespace {

// The pages the loopback callback shows in the user's browser. Byte for byte
// what this flow has always served — only the decision about WHICH one is
// shared with iOS now (oidc::parseCallbackQuery). MissingCode deliberately
// renders nothing: that request never came from the provider.
QByteArray loopbackPage(oidc::CallbackStatus status)
{
    static constexpr const char* kBody =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
        "<html><body style=\"font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; "
        "display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; "
        "background-color: #313338; color: #f2f3f5;\">"
        "<div style=\"text-align: center;\">";

    switch (status) {
    case oidc::CallbackStatus::Code:
        return QByteArray(kBody) +
               "<h2 style=\"color: #57f287;\">Login Successful!</h2>"
               "<p>You can close this tab and return to BSFChat.</p>"
               "</div></body></html>";
    case oidc::CallbackStatus::ProviderError:
        return QByteArray(kBody) +
               "<h2 style=\"color: #ed4245;\">Login Failed</h2>"
               "<p>An error occurred during authentication.</p>"
               "<p>You can close this tab and try again in BSFChat.</p>"
               "</div></body></html>";
    case oidc::CallbackStatus::StateMismatch:
        return QByteArray(kBody) +
               "<h2 style=\"color: #ed4245;\">Login Failed</h2>"
               "<p>Security validation failed (state mismatch).</p>"
               "<p>You can close this tab and try again in BSFChat.</p>"
               "</div></body></html>";
    case oidc::CallbackStatus::MissingCode:
        break;
    }
    return {};
}

} // namespace

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

        // Render the browser's page from the shared verdict, then let
        // handleCallback() act on it — the same function, with the same
        // checks in the same order, that the iOS session calls. The verdict
        // is recomputed there from the same query and the same m_state, so
        // the page the user is looking at and the decision the app takes
        // cannot disagree.
        const QByteArray page =
            loopbackPage(oidc::parseCallbackQuery(QUrlQuery(requestUrl.query()), m_state).status);
        if (!page.isEmpty()) {
            socket->write(page);
            socket->flush();
        }
        socket->close();
        socket->deleteLater();

        handleCallback(requestUrl);
    });
}
#endif // !BSFCHAT_NATIVE_OIDC_REDIRECT

void IdentityClient::exchangeCodeForTokens(const QString& code)
{
    // Byte-identical to the one /authorize carried — the provider compares
    // them exactly (RFC 6749 4.1.3) and refuses the grant otherwise. Both
    // ends read redirectUri(), so the loopback port on desktop and the
    // private-use scheme on iOS cannot drift apart.
    const QString redirectUri = this->redirectUri();

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
    // system(), not global(): global() is a Mersenne Twister seeded once
    // per process, and Qt documents it as unsuitable for cryptography. A
    // verifier an observer can predict is a verifier that protects nothing
    // -- PKCE's whole point is that only this client knows it. system() is
    // the OS CSPRNG, the same source OidcRequest.h already uses for the
    // nonce. (This project shipped a mt19937-seeded token bug once already.)
    auto* rng = QRandomGenerator::system();
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
    // system(), for the same reason as the PKCE verifier above: state is
    // the CSRF guard on the redirect, so it must not be predictable.
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(bytes.data()), 4);
    return QString::fromLatin1(bytes.toHex());
}
