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

#include "identity/OidcCallback.h"

namespace oidc {

// Client ids, the mobile redirect and isNativeCallbackUrl() live in
// OidcCallback.h, which has no dependency on the protocol library so the URL
// routing in core/UrlHandler can use them too.

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

// The authorization_code request to /token. `heldRefreshToken` is the BSFChat
// ID refresh token this device already holds for the SAME provider, if any
// (I2-2): the provider then carries that grant's server consents into the new
// grant and ends the old one, so a browser sign-in does not drop consent for
// every server joined so far. Once the exchange succeeds the held token is
// spent and must not be presented again.
inline QUrlQuery codeExchangeForm(const QString& code, const QString& redirectUri,
                                  const QString& codeVerifier, const QString& resource,
                                  const QString& heldRefreshToken)
{
    QUrlQuery body;
    body.addQueryItem("grant_type", "authorization_code");
    body.addQueryItem("code", code);
    body.addQueryItem("redirect_uri", redirectUri);
    // The same client the authorization was started as (kClientId): /token
    // refuses a code redeemed by a different client.
    body.addQueryItem("client_id", QString::fromLatin1(kClientId));
    body.addQueryItem("code_verifier", codeVerifier);
    // RFC 8707 2.2: repeated at the token endpoint; the provider refuses it
    // unless it matches the grant, so a mixed-up code cannot be redeemed for
    // a different server's token.
    if (!resource.isEmpty()) body.addQueryItem("resource", resource);
    if (!heldRefreshToken.isEmpty()) body.addQueryItem("refresh_token", heldRefreshToken);
    return body;
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

// The RFC 6749 `error` code, reduced to something safe to put in a sentence:
// the spec's own alphabet for it is printable ASCII minus `"` and `\`, and
// every code it defines is lower-case words and underscores. Anything else is
// dropped, and the length capped, so a crafted callback cannot use this field
// to put a paragraph of its own in front of the user either.
inline QString sanitizedErrorCode(const QString& raw)
{
    QString out;
    for (const QChar c : raw) {
        if (out.size() >= 64) break;
        const char16_t u = c.unicode();
        if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9')
            || u == '_' || u == '-' || u == '.')
            out.append(c);
    }
    return out;
}

// What the user is told for a provider error. Deliberately NOT the callback's
// error_description: the callback is an unauthenticated request — on desktop
// any page the browser loads can fetch the loopback URL, and on mobile any page
// can link to the claimed https callback — so its free text is attacker-chosen
// and has no business being shown as if this app or the provider said it
// (audit 2026-11, C-9). The code is kept because it is the useful part for a
// bug report, and after sanitizedErrorCode it cannot say anything else.
inline QString providerErrorMessage(const QString& rawCode)
{
    const QString code = sanitizedErrorCode(rawCode);
    if (code == QLatin1String("access_denied"))
        return QStringLiteral("Sign-in was declined at the identity provider (access_denied).");
    if (code.isEmpty())
        return QStringLiteral("The identity provider reported an error.");
    return QStringLiteral("The identity provider reported an error (%1).").arg(code);
}

// The one place that decides what a callback means.
//
// Order is load-bearing: the CSRF guard comes FIRST, for errors as well as for
// codes. It used to come second, so an `?error=` without `state` was reported
// as the provider's — which let anything able to reach the callback abort a
// sign-in in progress with a message of its choosing (C-9). The provider this
// client talks to echoes `state` on its error redirects (RFC 6749 4.1.2.1
// requires it whenever the request carried one, and ours always does), so
// requiring it costs a genuine refusal nothing. `expectedState` is the value
// generated for THIS attempt; an empty one can never match, so a callback
// arriving when no sign-in is in flight is refused rather than accepted.
//
// The text in `error` is ours, never the callback's: see providerErrorMessage.
inline CallbackParse parseCallbackQuery(const QUrlQuery& query,
                                        const QString& expectedState)
{
    const QString state = query.queryItemValue("state");
    if (expectedState.isEmpty() || state != expectedState) {
        return {CallbackStatus::StateMismatch, {},
                QStringLiteral("State mismatch - possible CSRF attack")};
    }

    const QString error = query.queryItemValue("error");
    if (!error.isEmpty()) {
        return {CallbackStatus::ProviderError, {}, providerErrorMessage(error)};
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
