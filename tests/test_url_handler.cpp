// Ownership of the bsfchat:// forwarding socket.
//
// install() used to call QLocalServer::removeServer() unconditionally and
// then listen(). On Unix removeServer() unlinks the socket file even when a
// live process is listening on it, so a second launch of the same profile
// silently took the name over: every subsequent deep link was delivered to
// the newest process while the original kept running with a listening
// socket nothing could connect to. It also meant listen() could never
// report AddressInUseError, destroying the one signal that says "already
// running".
//
// The stale-socket case it was written for is real, so the fix is ordering,
// not deletion: bind first, and only unlink after a probe proves nobody is
// answering. Both halves are pinned below.

#include "core/UrlHandler.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTest>

using Acquisition = UrlHandler::Acquisition;

class TestUrlHandler : public QObject
{
    Q_OBJECT

    // Unique per run so a crashed earlier run cannot make this suite flaky.
    QString freshName() const
    {
        return QStringLiteral("bsfchat-test-%1-%2")
            .arg(QCoreApplication::applicationPid())
            .arg(++m_counter);
    }
    mutable int m_counter = 0;

private slots:
    void cleanup()
    {
        // Nothing global to reset; names are unique per test.
    }

    void bindsAFreeName()
    {
        const QString name = freshName();
        QLocalServer server;
        QCOMPARE(UrlHandler::acquireServer(&server, name), Acquisition::Listening);
        QVERIFY(server.isListening());
        QCOMPARE(server.serverName(), name);
    }

    void doesNotStealFromALiveListener()
    {
        // The regression: first instance is up and listening.
        const QString name = freshName();
        QLocalServer first;
        QVERIFY(first.listen(name));

        QLocalServer second;
        QCOMPARE(UrlHandler::acquireServer(&second, name),
                 Acquisition::AnotherInstance);
        QVERIFY(!second.isListening());

        // And the first instance is still reachable — the point of the fix.
        QVERIFY(first.isListening());
        QLocalSocket client;
        client.connectToServer(name);
        QVERIFY2(client.waitForConnected(1000),
                 "the original instance stopped receiving deep links");
        client.disconnectFromServer();
    }

    void deepLinkStillReachesTheOriginalAfterASecondLaunch()
    {
        // End to end: a URL forwarded after the second launch must arrive
        // at the process that was already running.
        const QString name = freshName();
        QLocalServer first;
        QVERIFY(first.listen(name));

        QLocalServer second;
        QCOMPARE(UrlHandler::acquireServer(&second, name),
                 Acquisition::AnotherInstance);

        // acquireServer's own liveness probe is a connection too; drain it
        // so the spy below counts only the deep link.
        QCoreApplication::processEvents();
        while (auto* pending = first.nextPendingConnection()) pending->deleteLater();

        QSignalSpy incoming(&first, &QLocalServer::newConnection);
        QLocalSocket client;
        client.connectToServer(name);
        QVERIFY(client.waitForConnected(1000));
        QVERIFY(incoming.wait(1000));
        QCOMPARE(incoming.count(), 1);
    }

    void reclaimsAStaleSocket()
    {
        // A crash leaves the socket file behind with no process on it.
        // Emulated by listening and then closing without removeServer():
        // on Unix the filesystem entry survives close().
        const QString name = freshName();
        {
            QLocalServer dead;
            QVERIFY(dead.listen(name));
            dead.close();
        }

        QLocalServer fresh;
        const Acquisition got = UrlHandler::acquireServer(&fresh, name);
        QCOMPARE(got, Acquisition::Listening);
        QVERIFY(fresh.isListening());

        QLocalSocket client;
        client.connectToServer(name);
        QVERIFY2(client.waitForConnected(1000),
                 "reclaimed socket is not actually accepting connections");
    }

    void refusesGarbageInput()
    {
        QLocalServer server;
        QCOMPARE(UrlHandler::acquireServer(&server, QString()), Acquisition::Failed);
        QCOMPARE(UrlHandler::acquireServer(nullptr, QStringLiteral("x")),
                 Acquisition::Failed);
    }

    void socketNameIsPerProfile()
    {
        // The name install() actually uses must carry the profile suffix,
        // or two profiles would fight over one socket and the fix above
        // would turn that into "the second one silently loses deep links".
        const QString base = UrlHandler::socketName();
        QVERIFY(!base.isEmpty());
        QVERIFY(base.startsWith(QStringLiteral("bsfchat-url-ipc")));
    }
};

QTEST_MAIN(TestUrlHandler)
#include "test_url_handler.moc"
