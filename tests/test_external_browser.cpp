// A sign-in must never fail silently, on any platform.
//
// The bug this pins: on Linux, QDesktopServices::openUrl shells out, and the
// call site in IdentityClient::startLogin threw its answer away. A machine
// with no xdg-utils — or the release tarball, whose launcher exports
// LD_LIBRARY_PATH into every process it spawns and so kills the browser
// xdg-open execs (src/util/ExternalBrowser.h) — clicked "Sign in with
// BSFChat ID" and got nothing: no browser, no error, no log line, then a
// five-minute silence ending in "Login timed out".
//
// Two halves, both reachable from a Mac:
//
//   1. The argv we hand `env` in the bundled-Linux case. Pure function, so
//      the shape of the fix is checked where the fix cannot be run.
//   2. That a refused open is REPORTED and the attempt SURVIVES. The test
//      seam (setOpenUrlHandlerForTesting) stands in for a machine with no
//      browser, which is the only way this path is otherwise reachable.
//
// The survival half matters as much as the reporting half: the loopback
// listener is what makes the offered link usable, so a "fix" that cancelled
// the attempt would hand the user a URL that 404s on arrival.

#include <QtTest/QtTest>
#include <QSignalSpy>
#include <QUrlQuery>

#include "identity/IdentityClient.h"
#include "util/ExternalBrowser.h"

class TestExternalBrowser : public QObject {
    Q_OBJECT

private slots:
    void cleanup()
    {
        bsfchat::setOpenUrlHandlerForTesting(nullptr);
        // The browser owner/queue are process-wide. Every case above ends by
        // cancelling what it started, and this turn of the event loop lets
        // any pending promotion or drain run out before the next case looks
        // at waitingForBrowserCount().
        QTest::qWait(10);
        QCOMPARE(IdentityClient::waitingForBrowserCount(), 0);
    }

    // `env [-u NAME]... [NAME=VALUE]... COMMAND [ARG]...` — options first.
    // An assignment before an option is not an option, it is the command's
    // name, and `env LD_LIBRARY_PATH=x -u FOO xdg-open URL` would try to run
    // a program called "-u".
    void argvPutsOptionsBeforeAssignments()
    {
        const QStringList argv = bsfchat::sanitizedLaunchArgv(
            QStringLiteral("/opt/host/lib"), QStringLiteral("/usr/bin/xdg-open"),
            QUrl(QStringLiteral("https://id.bsfchat.com/authorize?state=abc")));

        const int assignment = argv.indexOf(QStringLiteral("LD_LIBRARY_PATH=/opt/host/lib"));
        QVERIFY2(assignment >= 0, "the host's own LD_LIBRARY_PATH must be restored, not dropped");
        const int lastOption = argv.lastIndexOf(QStringLiteral("-u"));
        QVERIFY(lastOption >= 0);
        QVERIFY2(lastOption + 1 < assignment, "every -u must precede the assignment");
    }

    // The normal case: nothing had LD_LIBRARY_PATH set before the launcher
    // did. Unset it — an empty LD_LIBRARY_PATH is not "no LD_LIBRARY_PATH",
    // it contains one empty entry, which some loaders read as ".".
    void argvUnsetsWhenTheHostHadNothing()
    {
        const QStringList argv = bsfchat::sanitizedLaunchArgv(
            QString(), QStringLiteral("/usr/bin/xdg-open"),
            QUrl(QStringLiteral("https://id.bsfchat.com/authorize")));

        QVERIFY(!argv.contains(QStringLiteral("LD_LIBRARY_PATH=")));
        const int u = argv.indexOf(QStringLiteral("LD_LIBRARY_PATH"));
        QVERIFY2(u > 0 && argv.at(u - 1) == QStringLiteral("-u"),
                 "LD_LIBRARY_PATH must be unset for the browser");
    }

    // Qt's own path variables would send a Qt-based browser into our bundle
    // looking for plugins; the URL is the last word either way, and passed
    // as one argv entry so no shell ever sees its query.
    void argvClearsQtPathsAndEndsWithTheUrl()
    {
        const QString url = QStringLiteral(
            "https://id.bsfchat.com/authorize?state=a&code_challenge=b");
        const QStringList argv = bsfchat::sanitizedLaunchArgv(
            QString(), QStringLiteral("/usr/bin/xdg-open"), QUrl(url));

        for (const auto& name : {"QT_PLUGIN_PATH", "QML2_IMPORT_PATH", "QML_IMPORT_PATH"})
            QVERIFY2(argv.contains(QString::fromLatin1(name)),
                     qPrintable(QStringLiteral("%1 must be cleared for the child")
                                    .arg(QString::fromLatin1(name))));

        QCOMPARE(argv.last(), url);
        QCOMPARE(argv.at(argv.size() - 2), QStringLiteral("/usr/bin/xdg-open"));
    }

    // The bug, from the outside: no browser, and the user is told, with the
    // link. Not loginFailed — that one is terminal, and ServerManager
    // answers it by tearing the connection (and the listener) down.
    void aRefusedOpenIsReportedWithTheUrl()
    {
        QUrl offered;
        bsfchat::setOpenUrlHandlerForTesting([&offered](const QUrl& u) {
            offered = u;
            return false; // a machine with no browser
        });

        IdentityClient client;
        QSignalSpy manual(&client, &IdentityClient::browserOpenFailed);
        QSignalSpy failed(&client, &IdentityClient::loginFailed);

        client.startLogin(QStringLiteral("https://id.bsfchat.com"));

        QCOMPARE(manual.count(), 1);
        QCOMPARE(failed.count(), 0);

        // The link has to be the real one — it is the only way in. Same URL
        // we tried to open, carrying this attempt's state and the loopback
        // port the callback comes back to.
        const QString shown = manual.takeFirst().at(0).toString();
        QCOMPARE(shown, offered.toString());
        QVERIFY(shown.startsWith(QStringLiteral("https://id.bsfchat.com/")));
        QVERIFY(shown.contains(QStringLiteral("redirect_uri")));
        QVERIFY(shown.contains(QStringLiteral("code_challenge")));

        // Still armed: the offered link is worth something only while the
        // loopback server is listening for its callback.
        QVERIFY2(client.isActive(),
                 "a browser that would not open must not cancel the sign-in");

        client.cancel();
    }

    // The ordinary path stays quiet. A dialog that announced a manual link
    // every time the browser DID open would be its own bug.
    void aSuccessfulOpenSaysNothing()
    {
        bsfchat::setOpenUrlHandlerForTesting([](const QUrl&) { return true; });

        IdentityClient client;
        QSignalSpy manual(&client, &IdentityClient::browserOpenFailed);
        QSignalSpy failed(&client, &IdentityClient::loginFailed);

        client.startLogin(QStringLiteral("https://id.bsfchat.com"));

        QCOMPARE(manual.count(), 0);
        QCOMPARE(failed.count(), 0);
        QVERIFY(client.isActive());

        client.cancel();
    }

    // ── Where this meets the one-browser-at-a-time queue ─────────────────
    //
    // Sign-ins are serialised process-wide (tests/test_oidc_serialisation.cpp):
    // one attempt owns the browser, the rest wait their turn. An attempt that
    // cannot open a browser KEEPS the browser — handing it on would put a
    // second authorization against the same provider session, which is the
    // 403 that serialisation exists to prevent, and the user is being asked
    // to open this one's link by hand.
    void aFailedOpenKeepsTheBrowserRatherThanPromotingTheNextAttempt()
    {
        int opens = 0;
        bsfchat::setOpenUrlHandlerForTesting([&opens](const QUrl&) {
            ++opens;
            return false;
        });

        IdentityClient account;
        IdentityClient server;

        account.startLogin(kProvider);
        server.startLogin(kProvider, kUat);

        QCOMPARE(opens, 1);
        // The waiter is not promoted into a browser that is not there.
        QTest::qWait(20);
        QCOMPARE(opens, 1);

        account.cancel();
        server.cancel();
    }

    // ...but it must not leave the next attempt in the dark either. Silence
    // behind a stuck sign-in is the same bug as silence in front of it: the
    // attempt ahead holds the browser until its five minutes are up, and a
    // machine that could not open a browser once will not open one for the
    // waiter. So a sign-in asked for while a browserless one holds the
    // browser is refused on the spot, with a sentence that says what to do.
    void aSignInAskedForBehindABrowserlessOneIsRefusedNotParked()
    {
        bsfchat::setOpenUrlHandlerForTesting([](const QUrl&) { return false; });

        IdentityClient account;
        IdentityClient server;
        QSignalSpy waiterFailed(&server, &IdentityClient::loginFailed);
        QSignalSpy waiterManual(&server, &IdentityClient::browserOpenFailed);

        account.startLogin(kProvider);
        server.startLogin(kProvider, kUat);

        QCOMPARE(waiterFailed.count(), 1);
        QVERIFY(!server.isActive());
        QVERIFY(!server.isWaitingForBrowser());
        QCOMPARE(IdentityClient::waitingForBrowserCount(), 0);

        // loginFailed, not browserOpenFailed: this attempt has no URL to
        // offer. It mints no PKCE material until its page is shown, so there
        // is nothing for the user to paste — only the one in front.
        QCOMPARE(waiterManual.count(), 0);
        QVERIFY(waiterFailed.takeFirst().at(0).toString().contains(
            QStringLiteral("browser"), Qt::CaseInsensitive));

        // The attempt that DOES have a link is untouched by all this.
        QVERIFY(account.isActive());

        account.cancel();
    }

    // The other half of the same rule: attempts that queued while the
    // browser still worked, and are still waiting when the one promoted
    // ahead of them cannot open a page. They are already parked, so they are
    // drained rather than refused — same sentence, same reason.
    void waitersAlreadyParkedAreDrainedWhenTheirTurnCannotOpen()
    {
        bool refuse = false;
        bsfchat::setOpenUrlHandlerForTesting([&refuse](const QUrl&) { return !refuse; });

        IdentityClient account;
        IdentityClient first;
        IdentityClient second;
        QSignalSpy secondFailed(&second, &IdentityClient::loginFailed);

        account.startLogin(kProvider);           // opens fine
        first.startLogin(kProvider, kUat);       // queued
        second.startLogin(kProvider, kOther);    // queued
        QCOMPARE(IdentityClient::waitingForBrowserCount(), 2);

        // The browser goes away (the user uninstalled it, the portal died),
        // and `first` is promoted into a machine that can no longer open one.
        refuse = true;
        account.cancel();

        QTRY_COMPARE(secondFailed.count(), 1);
        QVERIFY(!second.isActive());
        QCOMPARE(IdentityClient::waitingForBrowserCount(), 0);
        // `first` keeps the browser and its own link — it is the one the
        // user can still act on.
        QVERIFY(first.isActive());

        first.cancel();
    }

    // "No browser" is a fact about one attempt, not a latch on the client or
    // on the queue. Once the stuck sign-in lets go, the next one is asked for
    // in the ordinary way and gets the browser in the ordinary way — so a
    // user who installs a browser, or retries after the portal comes back,
    // is not permanently refused by state left behind.
    void theBrowserlessStateDoesNotLatch()
    {
        bool refuse = true;
        bsfchat::setOpenUrlHandlerForTesting([&refuse](const QUrl&) { return !refuse; });

        IdentityClient account;
        account.startLogin(kProvider);
        QVERIFY(account.isActive());
        account.cancel();

        // Same client, working browser, nothing in the way.
        refuse = false;
        IdentityClient retry;
        QSignalSpy retryFailed(&retry, &IdentityClient::loginFailed);
        QSignalSpy retryManual(&retry, &IdentityClient::browserOpenFailed);
        retry.startLogin(kProvider);

        QCOMPARE(retryFailed.count(), 0);
        QCOMPARE(retryManual.count(), 0);
        QVERIFY(retry.isActive());

        // ...and a second sign-in behind it queues, rather than being
        // refused by a leftover "there is no browser".
        IdentityClient server;
        server.startLogin(kProvider, kUat);
        QVERIFY(server.isWaitingForBrowser());
        QCOMPARE(IdentityClient::waitingForBrowserCount(), 1);

        server.cancel();
        retry.cancel();
    }

private:
    const QString kProvider = QStringLiteral("https://id.bsfchat.com");
    const QString kUat = QStringLiteral("https://uat.bsfchat.com");
    const QString kOther = QStringLiteral("https://chat.bsfchat.com");
};

// GUILESS on purpose: ExternalBrowser links Qt6::Gui for QDesktopServices,
// and QTEST_MAIN would then build a QGuiApplication, which wants a display
// the CI Linux runner does not have. Nothing here reaches the desktop — the
// open handler is stubbed in every case.
QTEST_GUILESS_MAIN(TestExternalBrowser)
#include "test_external_browser.moc"
