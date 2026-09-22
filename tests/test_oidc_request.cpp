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
};

QTEST_GUILESS_MAIN(TestOidcRequest)
#include "test_oidc_request.moc"
