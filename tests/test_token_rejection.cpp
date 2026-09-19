// One 401 anywhere must tell the client its session is over — and must not
// eat the response body on the way past.
//
// Before this, dead-token handling hung off SyncLoop alone. Every other
// authenticated request reported its 401 on whatever surface it owned, so
// when the production access_tokens table was purged on 2026-09-19 the first
// thing the user saw was a raw {"errcode":"M_UNKNOWN_TOKEN",...} toast thrown
// by the voice join path, not a prompt to sign in. MatrixClient now watches
// every authenticated reply centrally.
//
// The second half of each case is the hazard that shape introduces. The
// watcher is connected inside makeRequest(), i.e. BEFORE the call site's own
// finished handler, and Qt invokes slots in connection order — so a watcher
// that read the body with readAll() would drain the buffer and hand every
// existing call site an empty response. It uses peek(); these tests fail if
// anybody ever "simplifies" that.

#include "net/MatrixClient.h"

#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

// NB: no R"(...)" literals holding a double quote — moc mis-lexes those and
// drops the rest of the class, surfacing as a missing-vtable link error.

namespace {

class StubServer : public QObject
{
    Q_OBJECT
public:
    StubServer(int status, QByteArray body)
        : m_status(status), m_body(std::move(body))
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            auto* sock = m_server.nextPendingConnection();
            auto* seen = new QByteArray;
            connect(sock, &QTcpSocket::readyRead, this, [this, sock, seen]() {
                *seen += sock->readAll();
                if (!seen->contains("\r\n\r\n")) return;
                m_sawAuthHeader = seen->contains("Authorization: Bearer ");
                const QByteArray reply =
                    "HTTP/1.1 " + QByteArray::number(m_status) + " X\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: " + QByteArray::number(m_body.size()) + "\r\n"
                    "Connection: close\r\n\r\n" + m_body;
                sock->write(reply);
                sock->flush();
                sock->disconnectFromHost();
            });
            connect(sock, &QTcpSocket::disconnected, sock, [sock, seen]() {
                delete seen;
                sock->deleteLater();
            });
        });
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost); }
    QString url() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
    }
    bool sawAuthHeader() const { return m_sawAuthHeader; }

private:
    QTcpServer m_server;
    int m_status;
    QByteArray m_body;
    bool m_sawAuthHeader = false;
};

const char* kDeadToken =
    "{\"errcode\":\"M_UNKNOWN_TOKEN\",\"error\":\"Invalid or missing access token\"}";

} // namespace

class TestTokenRejection : public QObject
{
    Q_OBJECT

private slots:
    // The purge, seen from an ordinary room request rather than from /sync.
    void anyAuthenticatedRequestReportsADeadToken()
    {
        StubServer server(401, kDeadToken);
        QVERIFY(server.listen());

        MatrixClient client;
        client.setHomeserver(server.url());
        client.setAccessToken(QStringLiteral("purged-token"));

        QSignalSpy rejected(&client, &MatrixClient::accessTokenRejected);
        client.redactEvent(QStringLiteral("req-1"), QStringLiteral("!room:test"),
                           QStringLiteral("$event:test"));

        QVERIFY2(rejected.wait(5000),
                 "a 401 M_UNKNOWN_TOKEN outside /sync said nothing about the session");
        QCOMPARE(rejected.count(), 1);
        QVERIFY(rejected.at(0).at(0).toString().contains(QStringLiteral("M_UNKNOWN_TOKEN")));
        QVERIFY(server.sawAuthHeader());
    }

    // The ordering hazard. If the watcher ever goes back to readAll(), the
    // call site's own handler gets an empty body and every error message in
    // the app becomes blank — silently, because nothing else would fail.
    void theWatcherDoesNotConsumeTheBodyItInspects()
    {
        StubServer server(401, kDeadToken);
        QVERIFY(server.listen());

        MatrixClient client;
        client.setHomeserver(server.url());
        client.setAccessToken(QStringLiteral("purged-token"));

        QSignalSpy rejected(&client, &MatrixClient::accessTokenRejected);
        QSignalSpy redactFailed(&client, &MatrixClient::redactFailed);
        client.redactEvent(QStringLiteral("req-1"), QStringLiteral("!room:test"),
                           QStringLiteral("$event:test"));

        QVERIFY(rejected.wait(5000));
        QVERIFY(redactFailed.count() == 1 || redactFailed.wait(2000));
        const QString body = redactFailed.at(0).at(1).toString();
        QVERIFY2(!body.isEmpty(),
                 "the call site got an empty body — the watcher drained the reply");
        QVERIFY(body.contains(QStringLiteral("M_UNKNOWN_TOKEN")));
    }

    // /sync is the path that already had 401 handling; it must keep it, and
    // must also route through the central signal so one code path answers
    // the rejection wherever it was noticed.
    void syncAlsoReportsThroughTheCentralSignal()
    {
        StubServer server(401, kDeadToken);
        QVERIFY(server.listen());

        MatrixClient client;
        client.setHomeserver(server.url());
        client.setAccessToken(QStringLiteral("purged-token"));

        QSignalSpy rejected(&client, &MatrixClient::accessTokenRejected);
        QSignalSpy syncFailed(&client, &MatrixClient::syncError);
        client.sync(QString(), 1000);

        QVERIFY(rejected.wait(5000));
        QVERIFY(syncFailed.count() == 1 || syncFailed.wait(2000));
        QVERIFY2(syncFailed.at(0).at(0).toString().contains(QStringLiteral("M_UNKNOWN_TOKEN")),
                 "SyncLoop's own classifier lost its input");
    }

    // A 403 is a permission problem, not a revoked session. Answering it
    // with a sign-in prompt would send people to re-authenticate over
    // something signing in cannot fix.
    void otherFailuresAreNotTreatedAsARevokedSession()
    {
        StubServer server(403, "{\"errcode\":\"M_FORBIDDEN\",\"error\":\"no\"}");
        QVERIFY(server.listen());

        MatrixClient client;
        client.setHomeserver(server.url());
        client.setAccessToken(QStringLiteral("good-token"));

        QSignalSpy rejected(&client, &MatrixClient::accessTokenRejected);
        QSignalSpy redactFailed(&client, &MatrixClient::redactFailed);
        client.redactEvent(QStringLiteral("req-1"), QStringLiteral("!room:test"),
                           QStringLiteral("$event:test"));

        QVERIFY(redactFailed.wait(5000));
        QCOMPARE(rejected.count(), 0);
    }

    // A 401 from an endpoint we called WITHOUT a bearer token is the server
    // refusing a credential we offered in the body — a wrong password, a
    // spent OIDC id_token. Treating it as a revoked session would make a
    // failed sign-in look like the thing it was supposed to repair.
    void unauthenticatedRequestsAreNotWatched()
    {
        StubServer server(401, kDeadToken);
        QVERIFY(server.listen());

        MatrixClient client;
        client.setHomeserver(server.url());
        // No setAccessToken: nothing to revoke.

        QSignalSpy rejected(&client, &MatrixClient::accessTokenRejected);
        QSignalSpy failed(&client, &MatrixClient::loginError);
        client.login(QStringLiteral("someone"), QStringLiteral("wrong"));

        QVERIFY(failed.wait(5000));
        QCOMPARE(rejected.count(), 0);
        QVERIFY(!server.sawAuthHeader());
    }
};

QTEST_MAIN(TestTokenRejection)
#include "test_token_rejection.moc"
