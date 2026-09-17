// Deleting a message has to be able to fail.
//
// MatrixClient::redactEvent used to be fire-and-forget:
//
//     auto* reply = makeRequest("PUT", path, payload);
//     connect(reply, &QNetworkReply::finished, this, [reply]() { reply->deleteLater(); });
//
// — no signal, no error path, nothing. ServerConnection::redactEvent removed
// the row from the model before sending, so a redaction the server REFUSED
// (403 after the author's power level dropped, a moderator whose role was
// just changed, an offline client) looked exactly like one it accepted: the
// message disappeared, no error appeared, nothing rolled it back, and it
// silently reappeared on the next backfill or restart. The user had every
// reason to believe it was deleted.
//
// This drives the real MatrixClient against a stub homeserver, because the
// bug was the absence of a reply handler and only a real reply exercises it.

#include "net/MatrixClient.h"

#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

// NB: no R"(...)" literals below that contain a double quote — moc mis-lexes
// those and silently drops the rest of the class, which surfaces as a
// "missing vtable" link error rather than anything resembling the cause.

namespace {

// Answers the first request it receives with a fixed status and body.
class StubServer : public QObject
{
    Q_OBJECT
public:
    StubServer(int status, QByteArray body)
        : m_status(status), m_body(std::move(body))
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            auto* sock = m_server.nextPendingConnection();
            connect(sock, &QTcpSocket::readyRead, this, [this, sock]() {
                m_request += sock->readAll();
                // Headers complete is enough; we do not care about the body.
                if (!m_request.contains("\r\n\r\n")) return;
                const QByteArray reply =
                    "HTTP/1.1 " + QByteArray::number(m_status) + " X\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: " + QByteArray::number(m_body.size()) + "\r\n"
                    "Connection: close\r\n\r\n" + m_body;
                sock->write(reply);
                sock->flush();
                sock->disconnectFromHost();
            });
            connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
        });
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost); }
    QString url() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
    }
    QByteArray request() const { return m_request; }

private:
    QTcpServer m_server;
    int m_status;
    QByteArray m_body;
    QByteArray m_request;
};

} // namespace

class TestRedactionResult : public QObject
{
    Q_OBJECT

private slots:
    void refusedRedactionReportsFailure()
    {
        StubServer server(403, "{\"errcode\":\"M_FORBIDDEN\",\"error\":\"no permission to redact\"}");
        QVERIFY(server.listen());

        MatrixClient client;
        client.setHomeserver(server.url());
        client.setAccessToken(QStringLiteral("token"));

        QSignalSpy failed(&client, &MatrixClient::redactFailed);
        QSignalSpy ok(&client, &MatrixClient::redactSucceeded);

        client.redactEvent(QStringLiteral("req-1"), QStringLiteral("!room:test"),
                           QStringLiteral("$event:test"));

        QVERIFY2(failed.wait(5000), "a refused delete reported nothing at all");
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("req-1"));
        QVERIFY(failed.at(0).at(1).toString().contains(QStringLiteral("M_FORBIDDEN")));
        QCOMPARE(ok.count(), 0);
    }

    void acceptedRedactionReportsSuccessWithTheEventId()
    {
        StubServer server(200, "{\"event_id\":\"$redaction:test\"}");
        QVERIFY(server.listen());

        MatrixClient client;
        client.setHomeserver(server.url());
        client.setAccessToken(QStringLiteral("token"));

        QSignalSpy ok(&client, &MatrixClient::redactSucceeded);
        QSignalSpy failed(&client, &MatrixClient::redactFailed);

        client.redactEvent(QStringLiteral("req-2"), QStringLiteral("!room:test"),
                           QStringLiteral("$event:test"));

        QVERIFY(ok.wait(5000));
        QCOMPARE(ok.count(), 1);
        QCOMPARE(ok.at(0).at(0).toString(), QStringLiteral("req-2"));
        // The REDACTED event's id, not the redaction's — that is what the
        // model removes a row by.
        QCOMPARE(ok.at(0).at(1).toString(), QStringLiteral("$event:test"));
        QCOMPARE(failed.count(), 0);
    }

    void serverErrorIsAFailureToo()
    {
        // A 500 or a proxy page must not be mistaken for a successful delete.
        StubServer server(502, "<html>Bad Gateway</html>");
        QVERIFY(server.listen());

        MatrixClient client;
        client.setHomeserver(server.url());
        client.setAccessToken(QStringLiteral("token"));

        QSignalSpy failed(&client, &MatrixClient::redactFailed);
        QSignalSpy ok(&client, &MatrixClient::redactSucceeded);

        client.redactEvent(QStringLiteral("req-3"), QStringLiteral("!room:test"),
                           QStringLiteral("$event:test"));

        QVERIFY(failed.wait(5000));
        QCOMPARE(ok.count(), 0);
    }

    void reactionTogglesStayFireAndForget()
    {
        // redactReaction passes no request id, so nothing claims its reply —
        // a failed un-react must not raise a "couldn't delete that message"
        // toast about a message the user never touched.
        StubServer server(403, "{\"errcode\":\"M_FORBIDDEN\"}");
        QVERIFY(server.listen());

        MatrixClient client;
        client.setHomeserver(server.url());
        client.setAccessToken(QStringLiteral("token"));

        QSignalSpy failed(&client, &MatrixClient::redactFailed);
        client.redactReaction(QStringLiteral("!room:test"), QStringLiteral("$rx:test"));

        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.count(), 1);
        QVERIFY2(failed.at(0).at(0).toString().isEmpty(),
                 "a reaction redaction must carry no request id");
    }
};

QTEST_MAIN(TestRedactionResult)
#include "test_redaction_result.moc"
