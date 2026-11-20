// Reordering a channel must not un-file it (audit 3, #7).
//
// setChannelOrder used to PUT the whole bsfchat.room.category state event as
// {"order":N}. That event also holds parent_id, so every drag in the sidebar
// moved the channel out of its category. The client now calls the server's
// PUT /rooms/{id}/order, which keeps the stored parent_id; this pins the
// request it sends and that a refusal is reported.

#include "net/MatrixClient.h"

#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

namespace {

class CaptureServer : public QObject
{
    Q_OBJECT
public:
    explicit CaptureServer(int status, QByteArray body = "{}")
        : m_status(status), m_body(std::move(body))
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            auto* sock = m_server.nextPendingConnection();
            auto* seen = new QByteArray;
            connect(sock, &QTcpSocket::readyRead, this, [this, sock, seen]() {
                *seen += sock->readAll();
                const int headerEnd = seen->indexOf("\r\n\r\n");
                if (headerEnd < 0) return;
                const QByteArray head = seen->left(headerEnd);
                int length = 0;
                for (const QByteArray& line : head.split('\n')) {
                    if (line.toLower().startsWith("content-length:"))
                        length = line.mid(15).trimmed().toInt();
                }
                if (seen->size() < headerEnd + 4 + length) return;
                requestLine = head.left(head.indexOf("\r\n"));
                requestBody = seen->mid(headerEnd + 4, length);
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

    QByteArray requestLine;
    QByteArray requestBody;

private:
    QTcpServer m_server;
    int m_status;
    QByteArray m_body;
};

} // namespace

class TestChannelOrder : public QObject
{
    Q_OBJECT

private slots:
    void usesTheOrderEndpointAndSendsOnlyTheOrder()
    {
        CaptureServer server(200);
        QVERIFY(server.listen());
        MatrixClient client;
        client.setHomeserver(server.url());
        client.setAccessToken(QStringLiteral("tok"));

        QSignalSpy done(&client, &MatrixClient::channelOrderSet);
        client.setChannelOrder(QStringLiteral("!chan:test"), 3);
        QVERIFY(done.wait(5000));

        QVERIFY2(server.requestLine.startsWith("PUT "), server.requestLine.constData());
        QVERIFY2(server.requestLine.contains("/_matrix/client/v3/rooms/")
                     && server.requestLine.contains("chan")
                     && server.requestLine.endsWith("/order HTTP/1.1"),
                 server.requestLine.constData());
        // Never the raw state event: that write replaced parent_id.
        QVERIFY(!server.requestLine.contains("bsfchat.room.category"));
        QVERIFY2(!server.requestBody.contains("parent_id"), server.requestBody.constData());
        QVERIFY(server.requestBody.contains("\"order\":3"));
    }

    void aRefusalIsReported()
    {
        CaptureServer server(403, "{\"errcode\":\"M_FORBIDDEN\",\"error\":\"no\"}");
        QVERIFY(server.listen());
        MatrixClient client;
        client.setHomeserver(server.url());
        client.setAccessToken(QStringLiteral("tok"));

        QSignalSpy failed(&client, &MatrixClient::channelOrderFailed);
        client.setChannelOrder(QStringLiteral("!chan:test"), 1);
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("!chan:test"));
    }
};

QTEST_MAIN(TestChannelOrder)
#include "test_channel_order.moc"
