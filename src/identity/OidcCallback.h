#pragma once
// The sign-in's client ids and its claimed mobile redirect, and the one test
// for "is this URL that redirect". Header-only and free of the protocol
// library on purpose: core/UrlHandler routes inbound Android intents with
// isNativeCallbackUrl() and must not drag the JWT helpers in with it.
// OidcRequest.h includes this; include it directly only where that header's
// dependencies are unwelcome.

#include <QString>
#include <QUrl>
#include <QtGlobal>

namespace oidc {

// ---- Clients and redirect URIs ----------------------------------------
//
// Two first-party public clients, one per redirect shape (RFC 8252):
//
//   * Desktop: "bsfchat-desktop", redirecting to a loopback server the client
//     owns for the duration of the sign-in (§7.3).
//
//   * iOS and Android: "bsfchat-mobile", redirecting to a CLAIMED https URL
//     (§7.2) that the OS hands only to the verified BSFChat app — Android
//     App Links checked against https://bsfchat.com/.well-known/assetlinks.json,
//     iOS associated domains against apple-app-site-association. Loopback
//     cannot work on a phone: the browser hand-off suspends the app and the
//     callback connection is never accepted.
//
// Mobile used to share desktop's client through the private-use scheme
// bsfchat://oauth/callback. Any app can claim a custom scheme, and while PKCE
// stopped another app from spending OUR code, nothing stopped it starting its
// OWN sign-in as bsfchat-desktop: the consent page said "BSFChat Desktop" and
// one tap of Allow gave it a token for the user's chat server. The identity
// provider no longer registers that redirect (identity ClientRegistration.cpp).
inline constexpr const char* kDesktopClientId = "bsfchat-desktop";
inline constexpr const char* kMobileClientId = "bsfchat-mobile";
#if defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
inline constexpr const char* kClientId = kMobileClientId;
#else
inline constexpr const char* kClientId = kDesktopClientId;
#endif

// The claimed mobile redirect. Host and path are what the platform glue
// matches on (ASWebAuthenticationSession's https callback on iOS, the
// autoVerify intent filter on Android), so they are spelled out separately;
// all three must agree with the identity provider's [apps] mobile_redirect_uri.
inline constexpr const char* kNativeCallbackHost = "bsfchat.com";
inline constexpr const char* kNativeCallbackPath = "/oauth/callback";
inline constexpr const char* kNativeRedirectUri = "https://bsfchat.com/oauth/callback";

// Is `url` the claimed https redirect this client asked for?
// Case-insensitive on scheme and host because the OS and the browser both
// feel free to normalise them; the query is left exactly as delivered.
//
// Strict about the path, and https only: any other bsfchat.com page, and any
// `bsfchat://` deep link, must never be mistaken for a sign-in callback (nor
// the reverse — see main.cpp, where a callback must not reach openMessageLink).
// An explicit port is refused too: the OS only verifies the default one.
inline bool isNativeCallbackUrl(const QUrl& url)
{
    if (url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) != 0) return false;
    if (url.port() != -1 && url.port() != 443) return false;
    if (!url.userInfo().isEmpty()) return false;
    QString path = url.path();
    while (path.endsWith('/')) path.chop(1);
    return url.host().compare(QLatin1String(kNativeCallbackHost), Qt::CaseInsensitive) == 0
        && path.compare(QLatin1String(kNativeCallbackPath), Qt::CaseInsensitive) == 0;
}

} // namespace oidc
