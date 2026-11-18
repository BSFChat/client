// Which server a BSFChat ID sign-in is for (identity audit 2026-09, C1).
//
// Every id_token used to carry aud=bsfchat-desktop, so a token handed to one
// chat server signed its holder in at all of them — a hostile server could
// replay its users' tokens against chat.bsfchat.com, and through
// link_identity take their sign-in over for good. The client now names the
// server it is signing in to (RFC 8707 `resource`), the provider audiences
// the token to exactly that, and the server accepts only its own URL.
//
// Pinned here: the resource is the canonical form of the homeserver the token
// will be posted to (the same spelling the provider and the server compute);
// the authorization request carries it and a fresh nonce; and a token that is
// not audienced to exactly that server, or not for this attempt, is refused
// before it is posted anywhere. Header-only (src/identity/OidcRequest.h): no
// browser, no socket, no provider, no GUI.

#include "identity/OidcRequest.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTest>
#include <QUrlQuery>

namespace {

// An unsigned JWS with the given payload. The client does not verify
// signatures (the server does); it only reads aud and nonce.
QString tokenWith(const QJsonObject& payload)
{
    const auto enc = [](const QByteArray& b) {
        return QString::fromLatin1(
            b.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    };
    return enc(R"({"alg":"RS256","typ":"JWT"})") + "." +
           enc(QJsonDocument(payload).toJson(QJsonDocument::Compact)) + ".c2ln";
}

} // namespace

class TestOidcRequest : public QObject
{
    Q_OBJECT

private slots:
    void resourceIsTheCanonicalHomeserver()
    {
        QCOMPARE(oidc::resourceForHomeserver("https://chat.bsfchat.com"),
                 QString("https://chat.bsfchat.com"));
        QCOMPARE(oidc::resourceForHomeserver("https://Chat.BSFChat.com:443/"),
                 QString("https://chat.bsfchat.com"));
        QCOMPARE(oidc::resourceForHomeserver("http://localhost:8448"),
                 QString("http://localhost:8448"));
    }

    void anUnusableHomeserverYieldsNoResource()
    {
        // The caller refuses to start the sign-in on an empty resource rather
        // than requesting none — which would mint the legacy audience.
        QVERIFY(oidc::resourceForHomeserver("").isEmpty());
        QVERIFY(oidc::resourceForHomeserver("chat.example").isEmpty());
        QVERIFY(oidc::resourceForHomeserver("https://real.example@evil.example").isEmpty());
    }

    void authorizeRequestNamesTheServerAndTheAttempt()
    {
        const QUrl url = oidc::authorizeUrl("https://id.example", "http://localhost:5/oauth/callback",
                                            "challenge", "state1", "nonce1",
                                            "https://chat.example");
        const QUrlQuery q(url);
        QCOMPARE(url.path(), QString("/authorize"));
        QCOMPARE(q.queryItemValue("resource", QUrl::FullyDecoded), QString("https://chat.example"));
        QCOMPARE(q.queryItemValue("nonce"), QString("nonce1"));
        QCOMPARE(q.queryItemValue("client_id"), QString("bsfchat-desktop"));
        QCOMPARE(q.queryItemValue("code_challenge_method"), QString("S256"));
        QCOMPARE(q.allQueryItemValues("resource").size(), 1);

        // The server-list sync is not a sign-in to any chat server.
        const QUrlQuery sync(oidc::authorizeUrl("https://id.example", "r", "c", "s", "n", {}));
        QVERIFY(!sync.hasQueryItem("resource"));
    }

    // I2-2: the code exchange hands the provider the grant it replaces, so
    // the new grant keeps the old one's server consents.
    void theCodeExchangeCarriesTheHeldRefreshToken()
    {
        const QUrlQuery q = oidc::codeExchangeForm("code1", "http://localhost:5/oauth/callback",
                                                   "verifier1", "https://chat.example", "rt-old");
        QCOMPARE(q.queryItemValue("grant_type"), QString("authorization_code"));
        QCOMPARE(q.queryItemValue("code"), QString("code1"));
        QCOMPARE(q.queryItemValue("code_verifier"), QString("verifier1"));
        QCOMPARE(q.queryItemValue("resource", QUrl::FullyDecoded), QString("https://chat.example"));
        QCOMPARE(q.queryItemValue("refresh_token"), QString("rt-old"));
        QCOMPARE(q.allQueryItemValues("refresh_token").size(), 1);

        const QUrlQuery none = oidc::codeExchangeForm("code1", "r", "v", {}, {});
        QVERIFY(!none.hasQueryItem("refresh_token"));
        QVERIFY(!none.hasQueryItem("resource"));
    }

    void noncesAreFreshAndLongEnough()
    {
        QSet<QString> seen;
        for (int i = 0; i < 64; ++i) {
            const QString n = oidc::generateNonce();
            QCOMPARE(n.size(), 32);  // 128 bits, hex
            QVERIFY(!seen.contains(n));
            seen.insert(n);
        }
    }

    void theRequestedTokenIsAccepted()
    {
        const auto t = tokenWith({{"aud", "https://chat.example"}, {"nonce", "n1"}});
        QVERIFY(oidc::checkIdToken(t, "https://chat.example", "n1").isEmpty());
        // A one-element array is the same audience.
        const auto a = tokenWith({{"aud", QJsonArray{"https://chat.example"}}, {"nonce", "n1"}});
        QVERIFY(oidc::checkIdToken(a, "https://chat.example", "n1").isEmpty());
    }

    void aTokenForAnyOtherAudienceIsNotPosted()
    {
        // Another server; the legacy client-id audience (a provider that
        // ignored `resource`); and a token good at this server AND another.
        for (const QJsonValue& aud : {QJsonValue("https://evil.example"),
                                      QJsonValue("bsfchat-desktop"),
                                      QJsonValue(QJsonArray{"https://chat.example",
                                                            "https://evil.example"}),
                                      QJsonValue()}) {
            const auto t = tokenWith({{"aud", aud}, {"nonce", "n1"}});
            QVERIFY2(!oidc::checkIdToken(t, "https://chat.example", "n1").isEmpty(),
                     qPrintable(QJsonDocument(QJsonObject{{"aud", aud}}).toJson()));
        }
    }

    void aTokenForAnotherAttemptIsNotPosted()
    {
        const auto t = tokenWith({{"aud", "https://chat.example"}, {"nonce", "other"}});
        QVERIFY(!oidc::checkIdToken(t, "https://chat.example", "n1").isEmpty());
        const auto none = tokenWith({{"aud", "https://chat.example"}});
        QVERIFY(!oidc::checkIdToken(none, "https://chat.example", "n1").isEmpty());
    }

    void garbageIsRefused()
    {
        QVERIFY(!oidc::checkIdToken("not-a-jwt", "https://chat.example", "n1").isEmpty());
        QVERIFY(!oidc::checkIdToken("a.!!!.c", "https://chat.example", "n1").isEmpty());
    }

    // ---- The authorization callback ------------------------------------
    //
    // iOS cannot use the desktop loopback redirect: handing off to Safari
    // suspends the app, so the callback connection is never accepted and
    // sign-in hangs on "Waiting for browser login…" until the timeout. It
    // redirects to the private-use scheme instead, delivered by
    // ASWebAuthenticationSession. What must NOT change with the transport is
    // what a callback is allowed to mean — hence one shared parser, pinned
    // here.

    void theMobileCallbackIsTheClaimedHttpsUrl()
    {
        // Pinned against three other places that must agree byte for byte:
        // the identity provider's [apps] mobile_redirect_uri, the Android
        // App Link filter (host + path), and the iOS associated domain.
        QCOMPARE(QString::fromLatin1(oidc::kNativeRedirectUri),
                 QString("https://bsfchat.com/oauth/callback"));
        QCOMPARE(QString::fromLatin1(oidc::kNativeCallbackHost), QString("bsfchat.com"));
        QCOMPARE(QString::fromLatin1(oidc::kNativeCallbackPath), QString("/oauth/callback"));
        const QUrl u(QString::fromLatin1(oidc::kNativeRedirectUri));
        QVERIFY(u.isValid());
        QCOMPARE(u.host(), QString::fromLatin1(oidc::kNativeCallbackHost));
        QCOMPARE(u.path(), QString::fromLatin1(oidc::kNativeCallbackPath));
        QVERIFY(oidc::isNativeCallbackUrl(u));
    }

    void eachPlatformSignsInAsItsOwnClient()
    {
        // Mobile is its own client so the provider can stop registering the
        // custom scheme for desktop's. These run on a desktop build.
        QCOMPARE(QString::fromLatin1(oidc::kDesktopClientId), QString("bsfchat-desktop"));
        QCOMPARE(QString::fromLatin1(oidc::kMobileClientId), QString("bsfchat-mobile"));
#if defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
        QCOMPARE(QString::fromLatin1(oidc::kClientId), QString("bsfchat-mobile"));
#else
        QCOMPARE(QString::fromLatin1(oidc::kClientId), QString("bsfchat-desktop"));
#endif
    }

    void aCallbackIsRecognisedHoweverTheOsSpellsIt()
    {
        for (const QString& u : {QStringLiteral("https://bsfchat.com/oauth/callback"),
                                 QStringLiteral("https://bsfchat.com/oauth/callback/"),
                                 QStringLiteral("https://bsfchat.com/oauth/callback?code=c&state=s"),
                                 QStringLiteral("HTTPS://BSFChat.com/oauth/callback?code=c"),
                                 QStringLiteral("https://bsfchat.com:443/oauth/callback")}) {
            QVERIFY2(oidc::isNativeCallbackUrl(QUrl(u)), qPrintable(u));
        }
    }

    void nothingElseIsACallback()
    {
        // Not the custom scheme (any app can claim it), not any other page
        // on the site, not a lookalike host, and never a room deep link.
        for (const QString& u : {QStringLiteral("bsfchat://oauth/callback"),
                                 QStringLiteral("bsfchat://room/!abc:example/$evt"),
                                 QStringLiteral("http://bsfchat.com/oauth/callback"),
                                 QStringLiteral("https://bsfchat.com/oauth"),
                                 QStringLiteral("https://bsfchat.com/oauth/callback/extra"),
                                 QStringLiteral("https://bsfchat.com/privacy"),
                                 QStringLiteral("https://bsfchat.com.evil/oauth/callback"),
                                 QStringLiteral("https://evil.bsfchat.com/oauth/callback"),
                                 QStringLiteral("https://bsfchat.com:8443/oauth/callback"),
                                 QStringLiteral("https://user@bsfchat.com/oauth/callback"),
                                 QStringLiteral("http://localhost:1234/oauth/callback"),
                                 QStringLiteral("")}) {
            QVERIFY2(!oidc::isNativeCallbackUrl(QUrl(u)), qPrintable(u));
        }
    }

    void aGoodCallbackYieldsItsCode()
    {
        const QUrlQuery q(QStringLiteral("code=abc123&state=s1"));
        const auto r = oidc::parseCallbackQuery(q, "s1");
        QCOMPARE(r.status, oidc::CallbackStatus::Code);
        QCOMPARE(r.code, QString("abc123"));
        QVERIFY(r.error.isEmpty());
    }

    void aReplyForAnotherAttemptIsRefused()
    {
        // The CSRF guard. Same wording the loopback flow has always shown,
        // because the desktop path now renders its page from this verdict.
        for (const QString& state : {QStringLiteral("state=s2"),
                                     QStringLiteral(""),
                                     QStringLiteral("state=")}) {
            const QUrlQuery q(QStringLiteral("code=abc123&") + state);
            const auto r = oidc::parseCallbackQuery(q, "s1");
            QVERIFY2(r.status == oidc::CallbackStatus::StateMismatch, qPrintable(state));
            QCOMPARE(r.error, QString("State mismatch - possible CSRF attack"));
            QVERIFY(r.code.isEmpty());
        }
    }

    void aCallbackWithNoAttemptInFlightIsRefused()
    {
        // IdentityClient::cancel() clears m_state, so this is what a callback
        // arriving after a timeout, a user cancellation or a finished sign-in
        // meets. An empty expected state must never match an empty presented
        // one — otherwise a late or replayed redirect starts a second token
        // exchange for a sign-in nobody asked for.
        for (const QString& q : {QStringLiteral("code=abc123&state="),
                                 QStringLiteral("code=abc123")}) {
            const auto r = oidc::parseCallbackQuery(QUrlQuery(q), QString());
            QVERIFY2(r.status == oidc::CallbackStatus::StateMismatch, qPrintable(q));
        }
    }

    void aProviderErrorIsReportedNotSwallowed()
    {
        // Includes the user declining on the provider's own consent page —
        // the other half of "cancelled", the half that arrives as a redirect
        // rather than as an ASWebAuthenticationSession cancellation. The
        // provider echoes `state` on these (OidcHandler redirect_with_error).
        const auto declined = oidc::parseCallbackQuery(
            QUrlQuery(QStringLiteral("error=access_denied&state=s1")), "s1");
        QCOMPARE(declined.status, oidc::CallbackStatus::ProviderError);
        QVERIFY(declined.code.isEmpty());
        QVERIFY(!declined.error.isEmpty());
        QVERIFY2(declined.error.contains(QLatin1String("access_denied")),
                 qPrintable(declined.error));

        const auto other = oidc::parseCallbackQuery(
            QUrlQuery(QStringLiteral("error=server_error&state=s1")), "s1");
        QCOMPARE(other.status, oidc::CallbackStatus::ProviderError);
        QVERIFY2(other.error.contains(QLatin1String("server_error")),
                 qPrintable(other.error));
    }

    // Audit 2026-11, C-9. The callback is an unauthenticated request: any
    // page can make the browser hit the loopback URL, any app can fire the
    // bsfchat:// intent. An error without THIS attempt's state is not the
    // provider's, and must not be able to end the sign-in.
    void anErrorWithoutTheAttemptsStateIsNotTheProviders()
    {
        for (const QString& q : {QStringLiteral("error=access_denied"),
                                 QStringLiteral("error=access_denied&state="),
                                 QStringLiteral("error=access_denied&state=s2"),
                                 QStringLiteral("error=server_error&error_description=x")}) {
            const auto r = oidc::parseCallbackQuery(QUrlQuery(q), "s1");
            QVERIFY2(r.status == oidc::CallbackStatus::StateMismatch, qPrintable(q));
        }
        // ...and with no attempt in flight there is nothing for it to match.
        const auto idle = oidc::parseCallbackQuery(
            QUrlQuery(QStringLiteral("error=server_error&state=")), QString());
        QCOMPARE(idle.status, oidc::CallbackStatus::StateMismatch);
    }

    // Even a callback carrying the right state is not shown verbatim: the
    // words on screen are ours, plus the error code reduced to the RFC 6749
    // alphabet. error_description never reaches the user.
    void theProvidersFreeTextIsNeverShown()
    {
        const auto r = oidc::parseCallbackQuery(
            QUrlQuery(QStringLiteral(
                "error=access_denied&state=s1"
                "&error_description=Your%20account%20is%20locked.%20Call%20555-0100")),
            "s1");
        QCOMPARE(r.status, oidc::CallbackStatus::ProviderError);
        QVERIFY2(!r.error.contains(QLatin1String("555")), qPrintable(r.error));
        QVERIFY2(!r.error.contains(QLatin1String("locked")), qPrintable(r.error));

        // A code that tries to be a sentence is cut down to its safe letters
        // and bounded in length.
        const auto crafted = oidc::parseCallbackQuery(
            QUrlQuery(QStringLiteral("error=%3Cb%3EVisit%20evil.example%3C%2Fb%3E&state=s1")),
            "s1");
        QCOMPARE(crafted.status, oidc::CallbackStatus::ProviderError);
        QVERIFY2(!crafted.error.contains(QLatin1Char('<')), qPrintable(crafted.error));
        QVERIFY2(!crafted.error.contains(QLatin1Char(' ') + QStringLiteral("evil")),
                 qPrintable(crafted.error));

        QCOMPARE(oidc::sanitizedErrorCode(QStringLiteral("invalid_request")),
                 QStringLiteral("invalid_request"));
        QCOMPARE(oidc::sanitizedErrorCode(QStringLiteral("a b\"c<d>")),
                 QStringLiteral("abcd"));
        QCOMPARE(oidc::sanitizedErrorCode(QString(500, QLatin1Char('x'))).size(), 64);
        QVERIFY(!oidc::providerErrorMessage(QStringLiteral("<>")).isEmpty());
    }

    void aCallbackWithNeitherCodeNorErrorIsRefused()
    {
        const auto r = oidc::parseCallbackQuery(
            QUrlQuery(QStringLiteral("state=s1")), "s1");
        QCOMPARE(r.status, oidc::CallbackStatus::MissingCode);
        QCOMPARE(r.error, QString("No authorization code received"));

        const auto empty = oidc::parseCallbackQuery(
            QUrlQuery(QStringLiteral("code=&state=s1")), "s1");
        QCOMPARE(empty.status, oidc::CallbackStatus::MissingCode);
    }

    void everyVerdictCarriesSomethingToShowTheUser()
    {
        // Every non-success path must produce a reason. A blank one reaches
        // the login dialog as an empty error and looks exactly like the hang
        // this change removes.
        for (const QString& q : {QStringLiteral("error=access_denied"),
                                 QStringLiteral("code=c&state=wrong"),
                                 QStringLiteral("state=s1")}) {
            const auto r = oidc::parseCallbackQuery(QUrlQuery(q), "s1");
            QVERIFY2(r.status != oidc::CallbackStatus::Code, qPrintable(q));
            QVERIFY2(!r.error.isEmpty(), qPrintable(q));
        }
    }

    void theCallbackUrlIsParsedStraightFromTheSessionsReply()
    {
        // End to end over the shape ASWebAuthenticationSession hands back: a
        // whole URL, from which the query is taken and read exactly as the
        // loopback path reads its own. Codes and states from this provider
        // are hex (random_hex), and an extra parameter it may add alongside
        // them must not disturb either.
        const QUrl delivered(QStringLiteral(
            "https://bsfchat.com/oauth/callback"
            "?code=9f2c1ab77de40031&state=4a1f0c9e88b27d35&iss=https%3A%2F%2Fid.example"));
        QVERIFY(oidc::isNativeCallbackUrl(delivered));
        const auto r = oidc::parseCallbackQuery(QUrlQuery(delivered.query()),
                                                "4a1f0c9e88b27d35");
        QCOMPARE(r.status, oidc::CallbackStatus::Code);
        QCOMPARE(r.code, QString("9f2c1ab77de40031"));
    }

    void theAuthorizeRequestCarriesTheNativeRedirectUnchanged()
    {
        // What iOS sends to /authorize. The provider stores this string and
        // compares /token's redirect_uri to it byte for byte (RFC 6749
        // 4.1.3), so any reshaping here fails the grant on the device only.
        const QUrl url = oidc::authorizeUrl(
            "https://id.example", QString::fromLatin1(oidc::kNativeRedirectUri),
            "chal", "s1", "n1", "https://chat.example");
        const QUrlQuery q(url.query());
        QCOMPARE(q.queryItemValue("redirect_uri", QUrl::FullyDecoded),
                 QString::fromLatin1(oidc::kNativeRedirectUri));
        // PKCE binds the code to this app's verifier even if the claimed
        // redirect ever leaks it (the fallback web page). S256, always.
        QCOMPARE(q.queryItemValue("code_challenge_method"), QString("S256"));
        QCOMPARE(q.queryItemValue("code_challenge"), QString("chal"));
        // And the audience binding is untouched by the transport change.
        QCOMPARE(q.queryItemValue("resource", QUrl::FullyDecoded),
                 QString("https://chat.example"));
        QCOMPARE(q.queryItemValue("nonce"), QString("n1"));
    }
};

QTEST_GUILESS_MAIN(TestOidcRequest)
#include "test_oidc_request.moc"
