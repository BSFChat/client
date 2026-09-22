#pragma once

// The parts of a BSFChat ID sign-in that decide WHICH server the resulting
// id_token is good for, pulled out of IdentityClient so they can be tested
// without a browser, a socket or a provider.
//
// Why this exists — identity audit 2026-09, finding C1. Every id_token used
// to carry aud=bsfchat-desktop, and every chat server checked for that same
// value. So a server a user signed in to received a token that signed them in
// at EVERY other server trusting the same provider: a hostile server could
// replay it against chat.bsfchat.com, and through link_identity attach the
// victim's identity to the attacker's account there for good.
//
// The fix: this client names the server it is signing in to as an RFC 8707
// `resource`, the provider audiences the token to exactly that, and each
// server accepts only its own URL. The one property this file must keep:
//
//   THE RESOURCE COMES FROM THE ADDRESS THE TOKEN WILL BE POSTED TO.
//
// Never from anything a server says — not its login flows, not a well-known
// file, not a redirect. ServerConnection passes MatrixClient::homeserver(),
// the base URL m.login.token is sent to. A hostile server therefore cannot
// get this client to request a token audienced to some other server: the only
// token it can cause to exist is one for itself. (A .well-known file that
// points at another server makes the client CONNECT there, and the token then
// goes there too — the hostile server never sees it.)
//
// Header-only so the three test targets that already compile
// IdentityClient.cpp need no new source lines.

#include <bsfchat/JwtUtils.h>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

namespace oidc {

inline constexpr const char* kClientId = "bsfchat-desktop";

// ---- Redirect URIs ------------------------------------------------------
//
// Desktop redirects to a loopback server the client owns for the duration of
// the sign-in (RFC 8252 §7.3). iOS cannot: the moment the browser takes over,
// the app is suspended, the listening socket stops being serviced, and the
// callback connection is never accepted — sign-in hangs forever. iOS uses the
// private-use URI scheme instead (RFC 8252 §7.1), delivered by
// ASWebAuthenticationSession straight back into the still-running app.
//
// The scheme is the same `bsfchat` the app already claims for deep links
// (CFBundleURLTypes / HKCU\Software\Classes\bsfchat / x-scheme-handler), so
// nothing new is registered with the OS for this.
//
// A private-use scheme cannot be reserved by anyone, so it is worth being
// explicit about what carries the security here: PKCE. Another app on the
// device can claim `bsfchat://` and race us for the redirect, but the
// authorization code it steals is useless without the S256 verifier that
// never left this process. That is exactly why the provider MUST refuse a
// token request from this client without a valid `code_verifier` — see the
// identity provider's client registration.
inline constexpr const char* kNativeCallbackScheme = "bsfchat";
inline constexpr const char* kNativeRedirectUri = "bsfchat://oauth/callback";

// The `resource` to request for a homeserver base URL: the canonical form
// the provider and the server also compute (bsfchat::canonical_audience_url),
// or empty when the URL cannot be an audience — in which case the caller must
// refuse to start the sign-in rather than fall back to requesting none, since
// a token with no resource carries the legacy audience upgraded servers refuse.
inline QString resourceForHomeserver(const QString& homeserver)
{
    auto canonical = bsfchat::canonical_audience_url(homeserver.toStdString());
    return canonical ? QString::fromStdString(*canonical) : QString();
}

// OIDC nonce: 128 bits from the OS CSPRNG, hex. Per authorization attempt.
// The provider echoes it into the id_token; we check the echo before using
// the token, and the chat server refuses a second presentation of it.
inline QString generateNonce()
{
    quint32 words[4];
    QRandomGenerator::system()->fillRange(words);
    return QString::fromLatin1(
        QByteArray(reinterpret_cast<const char*>(words), sizeof(words)).toHex());
}

inline QUrl authorizeUrl(const QString& providerUrl, const QString& redirectUri,
                         const QString& codeChallenge, const QString& state,
                         const QString& nonce, const QString& resource)
{
    QUrl url(providerUrl + "/authorize");
    QUrlQuery query;
    query.addQueryItem("client_id", kClientId);
    query.addQueryItem("redirect_uri", redirectUri);
    query.addQueryItem("response_type", "code");
    query.addQueryItem("scope", "openid profile");
    query.addQueryItem("code_challenge", codeChallenge);
    query.addQueryItem("code_challenge_method", "S256");
    query.addQueryItem("state", state);
    query.addQueryItem("nonce", nonce);
    if (!resource.isEmpty()) query.addQueryItem("resource", resource);
    url.setQuery(query);
    return url;
}

// ---- The authorization callback ----------------------------------------
//
// Two transports deliver it — a loopback HTTP request on desktop, an
// ASWebAuthenticationSession completion (or a `bsfchat://` URL open) on iOS —
// and BOTH funnel through the one function below, so the decisions that
// matter are made in exactly one place and are the same on every platform.
// Splitting them was how this sort of thing goes wrong: it takes one
// transport that forgets to compare `state` for the CSRF guard to be gone on
// that platform only, with nothing in the desktop tests to notice.

enum class CallbackStatus {
    Code,          // a usable authorization code; carry on to /token
    ProviderError, // the provider said no (includes the user declining there)
    StateMismatch, // CSRF guard tripped — the reply is not ours
    MissingCode,   // a callback with neither an error nor a code
};

struct CallbackParse {
    CallbackStatus status = CallbackStatus::MissingCode;
    QString code;  // set iff status == Code
    QString error; // user-facing reason, set for every other status
};

// Is `url` the private-use-scheme redirect this client asked for?
// Case-insensitive on scheme and host because the OS and the browser both
// feel free to normalise them; the query is left exactly as delivered.
//
// Deliberately strict about the path: `bsfchat://room/...` deep links share
// this scheme, and a deep link must never be mistaken for a sign-in callback
// (nor the reverse — see main.cpp, where a callback must not be handed to
// openMessageLink).
inline bool isNativeCallbackUrl(const QUrl& url)
{
    if (url.scheme().compare(QLatin1String(kNativeCallbackScheme),
                             Qt::CaseInsensitive) != 0)
        return false;
    // "bsfchat://oauth/callback" parses as host=oauth, path=/callback.
    QString path = url.path();
    while (path.endsWith('/')) path.chop(1);
    return url.host().compare(QLatin1String("oauth"), Qt::CaseInsensitive) == 0
        && path.compare(QLatin1String("/callback"), Qt::CaseInsensitive) == 0;
}

// The one place that decides what a callback means.
//
// Order is load-bearing and matches what the loopback path has always done:
// a provider-reported error is reported as such even when `state` is absent
// (an error redirect may legitimately arrive without one), then the CSRF
// guard, then the code. `expectedState` is the value generated for THIS
// attempt; an empty one can never match, so a callback arriving when no
// sign-in is in flight is refused rather than accepted.
inline CallbackParse parseCallbackQuery(const QUrlQuery& query,
                                        const QString& expectedState)
{
    const QString error = query.queryItemValue("error");
    if (!error.isEmpty()) {
        const QString desc = query.queryItemValue("error_description");
        return {CallbackStatus::ProviderError, {}, desc.isEmpty() ? error : desc};
    }

    const QString state = query.queryItemValue("state");
    if (expectedState.isEmpty() || state != expectedState) {
        return {CallbackStatus::StateMismatch, {},
                QStringLiteral("State mismatch - possible CSRF attack")};
    }

    const QString code = query.queryItemValue("code");
    if (code.isEmpty()) {
        return {CallbackStatus::MissingCode, {},
                QStringLiteral("No authorization code received")};
    }

    return {CallbackStatus::Code, code, {}};
}

// Checks, before the id_token is handed to anybody, that it is the token
// this attempt asked for: audienced to exactly `expectedAudience` (when one
// was requested) and carrying `expectedNonce`. Returns an empty string when
// it is, else why not.
//
// The signature is not checked here — the token came straight from the
// provider over TLS, and the server verifies it properly. This is the
// client refusing to post a token somewhere it was not meant for: a token
// good at a second server (an array audience), a token for a different
// attempt (concurrent sign-ins to two servers must not cross), or a provider
// that ignored `resource` and minted the legacy audience.
inline QString checkIdToken(const QString& idToken, const QString& expectedAudience,
                            const QString& expectedNonce)
{
    const auto parts = idToken.split('.');
    if (parts.size() != 3) return QStringLiteral("The identity provider returned a malformed token");
    const QByteArray payload = QByteArray::fromBase64(
        parts[1].toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    const QJsonObject claims = QJsonDocument::fromJson(payload).object();
    if (claims.isEmpty()) return QStringLiteral("The identity provider returned a malformed token");

    if (!expectedAudience.isEmpty()) {
        const QJsonValue aud = claims.value("aud");
        const QString single = aud.isArray() && aud.toArray().size() == 1
            ? aud.toArray().first().toString()
            : aud.toString();
        if (single != expectedAudience) {
            return QStringLiteral(
                "The identity provider issued a token for a different server than the one "
                "you are signing in to. It may need updating; the token was not used.");
        }
    }
    if (!expectedNonce.isEmpty() && claims.value("nonce").toString() != expectedNonce) {
        return QStringLiteral("The identity provider's reply does not belong to this sign-in "
                              "attempt; the token was not used.");
    }
    return {};
}

} // namespace oidc
