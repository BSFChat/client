// ONE /sync outstanding per connection. Ever.
//
// THE BUG
//
// SyncLoop::stop() used to clear m_running and stop the retry timer, and
// that was all. The /sync request that was already in the air kept going —
// nothing aborted it, and MatrixClient did not even keep a handle to it.
// So a stop()/start() pair inside the 30-second poll window left TWO polls
// alive on one loop:
//
//   1. Poll A is parked on the server.
//   2. stop()  — m_running = false. A is untouched and still running.
//   3. start() — m_running = true, and doSync() issues poll B.
//   4. A's reply lands, finds m_running true, is treated as an ordinary
//      reply, and schedules A'.
//   5. B's reply lands and schedules B'.
//
// From then on the connection has two independent, self-sustaining poll
// chains on one credential, each writing next_batch over the other's. That
// sequence is reachable from re-authentication: ServerConnection::
// beginReauth() stops the loop, and applyLoginResponse() starts it again
// when the identity round trip comes back — which, with a warm IdP session,
// is well inside 30 seconds.
//
// The test drives a real MatrixClient and SyncLoop against a loopback
// server that behaves like the real one: it holds a /sync open (a long
// poll) instead of answering at once. That is the only shape in which the
// bug exists — against a server that answers immediately there is never an
// overlapping request to duplicate.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

#include "net/MatrixClient.h"
#include "net/SyncLoop.h"

// A homeserver that parks every /sync and answers when told to, counting
// how many are parked at once.
class ParkingHomeserver : public QObject {
    Q_OBJECT
public:
    explicit ParkingHomeserver(QObject* parent = nullptr) : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this,
                &ParkingHomeserver::onConnection);
        [[maybe_unused]] const bool ok = m_server.listen(QHostAddress::LocalHost, 0);
        Q_ASSERT(ok);
    }

    ~ParkingHomeserver() override
    {
        // ~QTcpServer deletes the sockets it parented, and each one emits
        // disconnected() on its way out. Those handlers touch m_parked,
        // m_buffers and m_seen — plain members declared AFTER m_server, so
        // destroyed BEFORE it. Without this the teardown of every test in
        // this file is a heap-use-after-free on a destroyed QList.
        for (QTcpSocket* s : m_server.findChildren<QTcpSocket*>()) s->disconnect(this);
        m_parked.clear();
        m_buffers.clear();
        m_seen.clear();
    }

    QString url() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
    }

    // Total /sync requests the server has ever seen.
    int totalSyncRequests() const { return m_totalSyncs; }
    // How many are parked right now.
    int parkedNow() const { return static_cast<int>(m_parked.size()); }
    // The most that were ever parked simultaneously. THIS is the assertion
    // the bug trips: with two chains alive it reaches 2 and stays there.
    int peakConcurrentSyncs() const { return m_peak; }

    // Answer every parked poll with an empty, progressing sync.
    void releaseAll()
    {
        const auto parked = m_parked;
        m_parked.clear();
        for (QTcpSocket* s : parked) {
            if (!s || s->state() != QAbstractSocket::ConnectedState) continue;
            const QByteArray body =
                QByteArray("{\"next_batch\":\"t_") + QByteArray::number(++m_batch)
                + "\",\"rooms\":{\"join\":{},\"invite\":{},\"leave\":{}}}";
            QByteArray head = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                              "Content-Length: " + QByteArray::number(body.size())
                              + "\r\nConnection: close\r\n\r\n";
            s->write(head);
            s->write(body);
            s->flush();
            s->disconnectFromHost();
        }
    }

private slots:
    void onConnection()
    {
        while (QTcpSocket* sock = m_server.nextPendingConnection()) {
            connect(sock, &QTcpSocket::readyRead, this, [this, sock] {
                m_buffers[sock] += sock->readAll();
                if (!m_buffers[sock].contains("\r\n\r\n")) return;   // headers incomplete
                if (m_seen.contains(sock)) return;                   // one request per socket
                m_seen.insert(sock);
                if (!m_buffers[sock].contains("/sync")) {
                    sock->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                                "Connection: close\r\n\r\n");
                    sock->disconnectFromHost();
                    return;
                }
                ++m_totalSyncs;
                // Park it. A real long poll holds the socket for up to 30s.
                m_parked.append(sock);
                m_peak = std::max(m_peak, static_cast<int>(m_parked.size()));
            });
            connect(sock, &QTcpSocket::disconnected, this, [this, sock] {
                m_parked.removeAll(sock);
                m_buffers.remove(sock);
                m_seen.remove(sock);
                sock->deleteLater();
            });
        }
    }

private:
    QTcpServer m_server;
    QList<QTcpSocket*> m_parked;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    QSet<QTcpSocket*> m_seen;
    int m_totalSyncs = 0;
    int m_peak = 0;
    int m_batch = 0;
};

class TestSyncSingleFlight : public QObject {
    Q_OBJECT

private:
    // Spin the event loop until `pred` holds or `budgetMs` elapses. Returns
    // whether it held — callers that expect a thing NOT to happen ignore it
    // and assert on the counters instead.
    template <typename Pred>
    static bool spinUntil(Pred pred, int budgetMs = 3000)
    {
        QElapsedTimer t;
        t.start();
        while (!pred() && t.elapsed() < budgetMs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        return pred();
    }

    static void spinFor(int ms)
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
    }

private slots:
    // Baseline: a healthy loop keeps exactly one poll in the air.
    void aRunningLoopParksExactlyOnePoll()
    {
        ParkingHomeserver server;
        MatrixClient client;
        client.setHomeserver(server.url());
        client.setAccessToken(QStringLiteral("tok"));
        SyncLoop loop(&client);

        loop.start();
        QVERIFY(spinUntil([&] { return server.parkedNow() == 1; }));
        spinFor(300);
        QCOMPARE(server.parkedNow(), 1);
        QCOMPARE(server.peakConcurrentSyncs(), 1);
        QVERIFY(loop.isPollInFlight());

        loop.stop();
    }

    // THE REGRESSION. stop() then start() while a poll is parked must leave
    // one poll, not two. Before the fix this reached two and never came
    // back down, which is exactly what the affected client's log shows.
    void restartingWhileAPollIsParkedDoesNotLeaveTwoChains()
    {
        ParkingHomeserver server;
        MatrixClient client;
        client.setHomeserver(server.url());
        client.setAccessToken(QStringLiteral("tok"));
        SyncLoop loop(&client);

        loop.start();
        QVERIFY(spinUntil([&] { return server.parkedNow() == 1; }));

        // This is the re-authentication shape: beginReauth() stops the loop
        // and applyLoginResponse() starts it again, with poll A still out.
        loop.stop();
        loop.start();
        QVERIFY(spinUntil([&] { return server.parkedNow() >= 1; }));

        // Let both the abandoned poll and the new one settle.
        spinFor(500);

        QCOMPARE(server.parkedNow(), 1);
        QCOMPARE(server.peakConcurrentSyncs(), 1);

        // And it must STAY one: release the parked poll and let the loop go
        // round several times. A second chain announces itself by the
        // request count marching up two at a time.
        const int before = server.totalSyncRequests();
        server.releaseAll();
        QVERIFY(spinUntil([&] { return server.totalSyncRequests() > before; }));
        spinFor(400);
        QCOMPARE(server.parkedNow(), 1);
        QCOMPARE(server.peakConcurrentSyncs(), 1);

        loop.stop();
    }

    // stop() must actually cancel the request, not merely ignore its reply.
    // A poll left running holds a socket on the client and an httplib worker
    // on the server for the rest of its 30 seconds.
    void stopCancelsTheOutstandingPoll()
    {
        ParkingHomeserver server;
        MatrixClient client;
        client.setHomeserver(server.url());
        client.setAccessToken(QStringLiteral("tok"));
        SyncLoop loop(&client);

        loop.start();
        QVERIFY(spinUntil([&] { return server.parkedNow() == 1; }));

        loop.stop();
        QVERIFY(spinUntil([&] { return server.parkedNow() == 0; }));
        QVERIFY(!loop.isPollInFlight());
    }

    // A cancelled poll is not a failure. If abandoning one reported
    // syncError, every stop() would feed the backoff and a re-authenticated
    // connection would come back slower each time.
    void abandoningAPollReportsNoError()
    {
        ParkingHomeserver server;
        MatrixClient client;
        client.setHomeserver(server.url());
        client.setAccessToken(QStringLiteral("tok"));
        SyncLoop loop(&client);
        QSignalSpy errors(&loop, &SyncLoop::syncError);

        loop.start();
        QVERIFY(spinUntil([&] { return server.parkedNow() == 1; }));
        loop.stop();
        spinFor(300);

        QCOMPARE(errors.count(), 0);
        QCOMPARE(loop.consecutiveFailures(), 0);
    }

    // refreshNow() is the foreground-resume path: it drops a poll that
    // suspension is presumed to have killed and issues a fresh one. It must
    // replace the poll, not add to it.
    void refreshNowReplacesThePollRatherThanAddingOne()
    {
        ParkingHomeserver server;
        MatrixClient client;
        client.setHomeserver(server.url());
        client.setAccessToken(QStringLiteral("tok"));
        SyncLoop loop(&client);

        loop.start();
        QVERIFY(spinUntil([&] { return server.parkedNow() == 1; }));
        const int before = server.totalSyncRequests();

        loop.refreshNow();
        QVERIFY(spinUntil([&] { return server.totalSyncRequests() > before; }));
        spinFor(300);

        QCOMPARE(server.parkedNow(), 1);
        QCOMPARE(server.peakConcurrentSyncs(), 1);

        loop.stop();
    }

    // doSync() is reachable from the retry timer, from onSyncSuccess and
    // from the reachability handler as well as from start(). None of them
    // may issue a poll on top of one that is already out.
    void aSecondStartIsANoOpWhileRunning()
    {
        ParkingHomeserver server;
        MatrixClient client;
        client.setHomeserver(server.url());
        client.setAccessToken(QStringLiteral("tok"));
        SyncLoop loop(&client);

        loop.start();
        QVERIFY(spinUntil([&] { return server.parkedNow() == 1; }));
        loop.start();
        loop.start();
        spinFor(300);

        QCOMPARE(server.totalSyncRequests(), 1);
        QCOMPARE(server.peakConcurrentSyncs(), 1);

        loop.stop();
    }
};

QTEST_MAIN(TestSyncSingleFlight)
#include "test_sync_single_flight.moc"
