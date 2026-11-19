// A LIVE since token the server refuses is dropped, not retried forever.
//
// Servers from 2026-11 answer /sync with 400 M_INVALID_PARAM for a garbage or
// foreign `since` instead of quietly doing an initial sync. SyncLoop only ever
// abandoned a token resumed from disk, so a token it had itself read off a
// reply (server restored from backup, database reset) was retried with
// backoff for the rest of the session and the client never synced again.
//
// Real MatrixClient + SyncLoop against a loopback server: the first /sync
// (no since) answers next_batch "s1"; any /sync carrying since=s1 gets the
// 400; the loop must then come back with no since at all.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

#include "net/MatrixClient.h"
#include "net/SyncLoop.h"

class RejectingHomeserver : public QObject {
    Q_OBJECT
public:
    RejectingHomeserver()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            auto* sock = m_server.nextPendingConnection();
            auto* seen = new QByteArray;
            connect(sock, &QTcpSocket::readyRead, this, [this, sock, seen]() {
                *seen += sock->readAll();
                if (!seen->contains("\r\n\r\n")) return;
                const QByteArray line = seen->left(seen->indexOf("\r\n"));
                QByteArray status = "200 OK";
                QByteArray body;
                if (line.contains("/sync")) {
                    requests.append(line);
                    if (line.contains("since=")) {
                        status = "400 Bad Request";
                        body = "{\"errcode\":\"M_INVALID_PARAM\",\"error\":\"Unknown since token\"}";
                    } else {
                        body = "{\"next_batch\":\"s1\",\"rooms\":{\"join\":{}}}";
                    }
                } else {
                    body = "{}";
                }
                sock->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\n"
                            "Content-Length: " + QByteArray::number(body.size())
                            + "\r\nConnection: close\r\n\r\n" + body);
                sock->flush();
                sock->disconnectFromHost();
            });
            connect(sock, &QTcpSocket::disconnected, sock, [sock, seen]() {
                delete seen;
                sock->deleteLater();
            });
        });
        [[maybe_unused]] const bool ok = m_server.listen(QHostAddress::LocalHost, 0);
        Q_ASSERT(ok);
    }
    ~RejectingHomeserver() override
    {
        for (QTcpSocket* s : m_server.findChildren<QTcpSocket*>()) s->disconnect(this);
    }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }

    QList<QByteArray> requests;

private:
    QTcpServer m_server;
};

class TestSyncRejectedSince : public QObject {
    Q_OBJECT
private slots:
    void aLiveTokenRefusedWithInvalidParamIsDropped()
    {
        RejectingHomeserver server;
        MatrixClient client;
        client.setHomeserver(server.url());
        client.setAccessToken(QStringLiteral("tok"));
        SyncLoop loop(&client);
        QSignalSpy abandoned(&loop, &SyncLoop::sinceTokenAbandoned);

        loop.start();  // no resumed token: everything it holds is "live"

        // initial (no since) -> s1; since=s1 -> 400; then a fresh initial.
        QElapsedTimer t;
        t.start();
        auto freshAfterRejection = [&]() {
            bool rejected = false;
            for (const QByteArray& r : server.requests) {
                if (r.contains("since=")) rejected = true;
                else if (rejected) return true;
            }
            return false;
        };
        while (!freshAfterRejection() && t.elapsed() < 15000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

        QVERIFY2(freshAfterRejection(),
                 "the loop kept retrying a since token the server refused");
        QVERIFY(abandoned.count() >= 1);
        loop.stop();
    }

    void otherErrorsKeepALiveToken()
    {
        // A transport error or an HTML page is not the server refusing the
        // token; the sync position must survive it.
        QVERIFY(!SyncBackoff::rejectsSinceTokenOutright(QStringLiteral("<html>502</html>")));
        QVERIFY(!SyncBackoff::rejectsSinceTokenOutright(QString()));
        QVERIFY(!SyncBackoff::rejectsSinceTokenOutright(
            QStringLiteral("{\"errcode\":\"M_UNKNOWN_TOKEN\"}")));
        QVERIFY(SyncBackoff::rejectsSinceTokenOutright(
            QStringLiteral("{\"errcode\":\"M_INVALID_PARAM\",\"error\":\"x\"}")));
    }
};

QTEST_MAIN(TestSyncRejectedSince)
#include "test_sync_rejected_since.moc"
