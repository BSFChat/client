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
