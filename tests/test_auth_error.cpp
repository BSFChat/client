// Recognising a dead access token in a homeserver error body.
//
// Why this matters: the server started expiring access tokens (90 days) and
// revoking them on logout/ban/device removal, and answers M_UNKNOWN_TOKEN
// when one is presented. The client had no 401 handling at all — the sync
// loop backed off to its 60 s ceiling and retried the same dead token for
// the life of the process while the UI showed "Reconnecting…". This suite
// pins the classifier that turns that into an honest "sign in again".
//
// The prefix cases are the point: SyncBackoff::indicatesRejectedSinceToken
// matches M_UNKNOWN, which is a *prefix* of M_UNKNOWN_TOKEN and means
// something entirely different. Neither classifier may leak into the other.

#include "net/AuthError.h"

#include <QTest>

class TestAuthError : public QObject
{
    Q_OBJECT

private slots:
    void unknownTokenIsDead()
    {
        QVERIFY(AuthError::indicatesDeadAccessToken(
            R"({"errcode":"M_UNKNOWN_TOKEN","error":"Invalid or missing access token"})"));
    }

    void missingTokenIsDead()
    {
        // The client always sends a token once credentials are set, so the
        // server telling us none arrived means our stored credential is gone.
        QVERIFY(AuthError::indicatesDeadAccessToken(
            R"({"errcode":"M_MISSING_TOKEN","error":"Missing access token"})"));
    }

    void expiryRealBodyFromServer()
    {
        // Shape the server's Middleware::auth_error actually emits, including
        // fields we ignore, to prove we key off errcode and nothing else.
        QVERIFY(AuthError::indicatesDeadAccessToken(
            R"({"errcode":"M_UNKNOWN_TOKEN","error":"Access token has expired","soft_logout":false})"));
    }

    void unknownIsNotDead()
    {
        // M_UNKNOWN is a prefix of M_UNKNOWN_TOKEN and is the sync-position
        // error. Treating it as a dead token would log the user out on a
        // perfectly recoverable sync failure.
        QVERIFY(!AuthError::indicatesDeadAccessToken(
            R"({"errcode":"M_UNKNOWN","error":"Unrecognised since token"})"));
    }

    void otherMatrixErrorsAreNotDead()
    {
        QVERIFY(!AuthError::indicatesDeadAccessToken(R"({"errcode":"M_FORBIDDEN"})"));
        QVERIFY(!AuthError::indicatesDeadAccessToken(R"({"errcode":"M_LIMIT_EXCEEDED"})"));
        QVERIFY(!AuthError::indicatesDeadAccessToken(R"({"errcode":"M_BAD_JSON"})"));
        QVERIFY(!AuthError::indicatesDeadAccessToken(R"({"errcode":"M_INVALID_PARAM"})"));
    }

    void transportFailuresAreNotDead()
    {
        // QNetworkReply hands us an empty body on DNS/TLS failure, and a
        // reverse proxy in front of a stopped server hands us HTML. Signing
        // the user out because the WiFi dropped would be much worse than
        // the bug being fixed.
        QVERIFY(!AuthError::indicatesDeadAccessToken(QString()));
        QVERIFY(!AuthError::indicatesDeadAccessToken(QStringLiteral("")));
        QVERIFY(!AuthError::indicatesDeadAccessToken(
            QStringLiteral("<html><head><title>502 Bad Gateway</title></head></html>")));
        QVERIFY(!AuthError::indicatesDeadAccessToken(
            QStringLiteral("Host bsfchat.com not found")));
    }

    void nonObjectJsonIsNotDead()
    {
        QVERIFY(!AuthError::indicatesDeadAccessToken(QStringLiteral("[]")));
        QVERIFY(!AuthError::indicatesDeadAccessToken(QStringLiteral("\"M_UNKNOWN_TOKEN\"")));
        QVERIFY(!AuthError::indicatesDeadAccessToken(QStringLiteral("null")));
    }

    void errcodeMustBeAString()
    {
        QVERIFY(!AuthError::indicatesDeadAccessToken(R"({"errcode":401})"));
        QVERIFY(!AuthError::indicatesDeadAccessToken(R"({"errcode":null})"));
        QVERIFY(!AuthError::indicatesDeadAccessToken(R"({"error":"M_UNKNOWN_TOKEN"})"));
    }

    void substringInProseDoesNotCount()
    {
        // A server that mentions the errcode in its human-readable text
        // must not trip us; only the machine-readable field counts.
        QVERIFY(!AuthError::indicatesDeadAccessToken(
            R"({"errcode":"M_FORBIDDEN","error":"not M_UNKNOWN_TOKEN, you are banned"})"));
    }
};

QTEST_MAIN(TestAuthError)
#include "test_auth_error.moc"
