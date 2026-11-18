#include "identity/IdentitySession.h"

#include "core/SecretStore.h"
#include "identity/OidcRequest.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

IdentitySession::IdentitySession(QObject* parent)
    : QObject(parent)
{
    m_transport = [this](const QUrl& url, const QUrlQuery& form,
                         std::function<void(int, const QByteArray&)> done) {
        if (!m_nam) m_nam = new QNetworkAccessManager(this);
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
        // Without one a dead connection leaves m_busy set forever, and every
        // later join and the launch-time restore queue behind it (C-6).
        req.setTransferTimeout(kTokenTimeoutMs);
        auto* reply = m_nam->post(req, form.toString(QUrl::FullyEncoded).toUtf8());
        connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)]() {
            reply->deleteLater();
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            done(status, reply->readAll());
        });
    };
}

namespace {
// The provider URL as requests use it: trimmed, no trailing slash.
QString trimProvider(const QString& url)
{
    QString out = url.trimmed();
    while (out.endsWith('/')) out.chop(1);
    return out;
}
} // namespace

// For comparisons and the store key only: case-folded as well.
QString IdentitySession::normaliseProvider(const QString& url)
{
    return trimProvider(url).toLower();
}

QString IdentitySession::storeKey(const QString& providerUrl)
{
    return QStringLiteral("identity-refresh:") + normaliseProvider(providerUrl);
}

bool IdentitySession::servesProvider(const QString& serverProvider) const
{
    return isSignedIn() && !serverProvider.isEmpty()
        && normaliseProvider(serverProvider) == normaliseProvider(m_providerUrl);
}

bool IdentitySession::restore(const QString& providerUrl)
{
    SecretStore& store = m_store ? *m_store : SecretStore::instance();
    const auto saved = store.read(storeKey(providerUrl));
    if (!saved || saved->isEmpty()) return false;
    // Already holding exactly this: leave any refresh in flight alone (its
    // reply is from the same grant; revoking it would revoke this token too).
    if (trimProvider(providerUrl) == m_providerUrl && *saved == m_refreshToken) return true;
    abandonInFlight(true, {});
    m_providerUrl = trimProvider(providerUrl);
    m_refreshToken = *saved;
    m_persisted = true;
    pump();
    return true;
}

void IdentitySession::adopt(const QString& providerUrl, const QString& refreshToken)
{
    if (refreshToken.isEmpty()) return;
    const QString provider = trimProvider(providerUrl);
    if (provider == m_providerUrl && refreshToken == m_refreshToken) return;
    // A browser sign-in is a new grant. A refresh still running presents the
    // old one; its answer is discarded (and revoked), and its caller is
    // served again with the new token.
    abandonInFlight(true, {});
    m_providerUrl = provider;
    m_refreshToken = refreshToken;
    persist();
    pump();
}

void IdentitySession::abandonInFlight(bool requeue, const QString& error)
{
    ++m_generation;
    m_busy = false;
    if (!m_inFlight) return;
    const Pending p = *m_inFlight;
    m_inFlight.reset();
    if (requeue) m_queue.prepend(p);
    else complete(p, {}, error);
}

void IdentitySession::persist()
{
    SecretStore& store = m_store ? *m_store : SecretStore::instance();
    m_persisted = store.write(storeKey(m_providerUrl), m_refreshToken);
    if (!m_persisted) {
        // Deliberately no file fallback: see SecretStore.h.
        qWarning() << "[IdentitySession] no secure store available; BSFChat ID sign-in is "
                      "kept for this run only";
        // Whatever an earlier write left there is now SPENT (the provider
        // has just rotated it). Restoring it next launch would present a
        // used refresh token, which the provider treats as theft and answers
        // by revoking the whole grant on every device. Gone is far better.
        store.remove(storeKey(m_providerUrl));
    }
}

void IdentitySession::signOut()
{
    clearSession(true);
}

void IdentitySession::clearSession(bool revokeAtProvider)
{
    const QString provider = m_providerUrl;
    const QString token = m_refreshToken;
    if (!m_providerUrl.isEmpty()) {
        SecretStore& store = m_store ? *m_store : SecretStore::instance();
        store.remove(storeKey(m_providerUrl));
    }
    m_refreshToken.clear();
    m_persisted = false;
    const QString why = tr("Signed out of BSFChat ID");
    abandonInFlight(false, why);
    // Anything still queued would present a token we no longer hold.
    const auto pending = m_queue;
    m_queue.clear();
    for (const auto& p : pending) complete(p, {}, why);
    // Revoking takes the whole grant with it, so nothing rotated out of this
    // token (including a reply still in flight) stays usable elsewhere.
    if (revokeAtProvider && !token.isEmpty() && !provider.isEmpty()) revoke(provider, token);
}

void IdentitySession::revoke(const QString& providerUrl, const QString& refreshToken)
{
    // RFC 7009; the provider's discovery document names this as its
    // revocation_endpoint (issuer + "/token/revoke"), as it names
    // issuer + "/token" for the token endpoint used below.
    QUrlQuery form;
    form.addQueryItem("token", refreshToken);
    form.addQueryItem("token_type_hint", "refresh_token");
    form.addQueryItem("client_id", QString::fromLatin1(oidc::kClientId));
    m_transport(QUrl(providerUrl + "/token/revoke"), form, [](int status, const QByteArray&) {
        // Best effort: a failure here leaves the grant to expire on its own.
        if (status != 200) qWarning() << "[IdentitySession] revocation failed, HTTP" << status;
    });
}

void IdentitySession::requestServerToken(const QString& resource, ServerTokenCallback callback)
{
    if (resource.isEmpty()) {
        callback({}, tr("This server's address cannot be used for BSFChat ID sign-in."));
        return;
    }
    m_queue.append({resource, std::move(callback), {}});
    pump();
}

void IdentitySession::requestAccessToken(AccessTokenCallback callback)
{
    m_queue.append({QString(), {}, std::move(callback)});
    pump();
}

void IdentitySession::complete(const Pending& p, const QString& value, const QString& error)
{
    if (p.serverCallback) p.serverCallback(value, error);
    if (p.accessCallback) p.accessCallback(value, error);
}

void IdentitySession::pump()
{
    if (m_busy || m_queue.isEmpty()) return;
    if (!isSignedIn()) {
        const auto pending = m_queue;
        m_queue.clear();
        for (const auto& p : pending) complete(p, {}, tr("Not signed in to BSFChat ID"));
        return;
    }
    m_busy = true;
    const Pending p = m_queue.takeFirst();
    m_inFlight = p;
    send(p, p.resource.isEmpty() ? QString() : oidc::generateNonce(), false);
}

void IdentitySession::send(const Pending& p, const QString& nonce, bool isRetry)
{
    QUrlQuery form;
    form.addQueryItem("grant_type", "refresh_token");
    form.addQueryItem("refresh_token", m_refreshToken);
    form.addQueryItem("client_id", QString::fromLatin1(oidc::kClientId));
    if (!p.resource.isEmpty()) {
        form.addQueryItem("resource", p.resource);
        form.addQueryItem("nonce", nonce);
    }

    const quint64 generation = m_generation;
    const QString provider = m_providerUrl;
    m_transport(QUrl(m_providerUrl + "/token"), form,
                [this, p, nonce, isRetry, generation, provider](int status, const QByteArray& body) {
        const QJsonObject obj = QJsonDocument::fromJson(body).object();
        const QString newRefresh = obj.value("refresh_token").toString();

        if (generation != m_generation) {
            // Signed out or re-signed-in while this was in flight. Its caller
            // has already been answered or requeued; the token it bought must
            // not resurrect the old session, so kill it at the provider.
            if (status == 200 && !newRefresh.isEmpty()) revoke(provider, newRefresh);
            return;
        }

        if (status == 0 && !isRetry) {
            // The reply was lost, perhaps after the provider rotated. One
            // retry with the same token, now, falls inside the provider's
            // grace window and is answered with the token it already issued;
            // giving up instead would leave us holding a spent token whose
            // next use revokes the grant.
            send(p, nonce, true);
            return;
        }
        m_inFlight.reset();

        if (status == 200 && !newRefresh.isEmpty()) {
            // Rule 2: the rotated token is saved before anything else runs.
            m_refreshToken = newRefresh;
            persist();
            QString value;
            QString error;
            if (p.resource.isEmpty()) {
                value = obj.value("access_token").toString();
                if (value.isEmpty()) error = tr("BSFChat ID returned no access token");
            } else {
                const QString idToken = obj.value("id_token").toString();
                error = idToken.isEmpty() ? tr("BSFChat ID returned no sign-in token")
                                          : oidc::checkIdToken(idToken, p.resource, nonce);
                if (error.isEmpty()) value = idToken;
            }
            m_busy = false;
            complete(p, value, error);
            pump();
            return;
        }

        const QString code = obj.value("error").toString();
        QString message = obj.value("error_description").toString();
        if (message.isEmpty()) message = code;

        if (code == QLatin1String("invalid_grant")) {
            // Expired, revoked, or (worst case) replayed elsewhere: the token
            // is dead. Forget it and say so; the next join goes through the
            // browser.
            m_busy = false;
            const QString reason = tr("Your BSFChat ID sign-in on this device has ended. "
                                      "Sign in again to continue.");
            complete(p, {}, reason);
            clearSession(false); // already dead at the provider
            emit signedOut(reason);
            return;
        }

        // Anything else (network failure, a refused resource, a server error)
        // leaves the token untouched: the provider validates before it
        // rotates, so a refused request did not spend it.
        //
        // invalid_target is the common one: the provider mints a server token
        // only for a server the account has consented to once on the
        // /authorize page. Not a sign-in failure — the session is fine, and
        // the caller (ServerManager::signInWithSessionOrBrowser) opens the
        // browser for that one server; every later join to it is silent.
        if (code == QLatin1String("invalid_target"))
            message = tr("This server needs your OK once in the browser.");
        m_busy = false;
        if (status == 0) message = tr("Could not reach BSFChat ID. Check your connection.");
        else if (message.isEmpty()) message = tr("BSFChat ID refused the request (HTTP %1)").arg(status);
        complete(p, {}, message);
        pump();
    });
}
