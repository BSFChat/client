// One browser, one sign-in at a time.
//
// There is a single system browser and a single user in front of it, but
// several IdentityClients exist at once by design: ServerManager owns one for
// the account-level sign-in and every ServerConnection owns another. Until
// 2026-09-24 each of them opened the browser the moment it was asked to, so
// an identity sign-in that found N servers fired N authorizations at once and
// a retry landed on top of whatever was already open.
//
// The damage is done at the provider. Each authorization mints its own
// consent prompt bound to the same browser session; the browser ends up
// showing one of them; approving it posts a consent token the provider has
// already superseded or spent, and the provider answers 403 "Authorization
// request does not belong to this session". That is the provider being
// right — it is handed a stale token — and it is why this had to be fixed
// here rather than by loosening anything there.
//
// Traced on the owner's Pixel 6 Pro, 2026-09-24, from ONE tap on "Sign in
// with BSFChat ID": two Chrome launches 17 ms apart and two
// bsfchat://oauth/callback intents.
//
// No browser is launched by this file — IdentityClient::setPagePresenterForTesting
// replaces the platform's "show the page" step, and only that step. The
// queue, the PKCE material, the state and the callback routing all run for
// real.

#include "identity/IdentityClient.h"

#include <QSignalSpy>
#include <QTest>
#include <QUrl>
#include <QUrlQuery>
#include <QVector>

namespace {

const auto kProvider = QStringLiteral("https://id.bsfchat.com");
const auto kUat = QStringLiteral("https://uat.bsfchat.com");
const auto kOther = QStringLiteral("https://chat.bsfchat.com");

} // namespace

class OidcSerialisationTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        m_shown.clear();
        IdentityClient::setPagePresenterForTesting(
            [this](const QUrl& url) { m_shown.append(url); });
    }

    void cleanup()
    {
        IdentityClient::setPagePresenterForTesting(nullptr);
    }

    // The reviewer's shape, and the one the Pixel trace shows going wrong:
    // the account-level sign-in is still open when the server-bound one is
    // asked for. Exactly one page is shown.
    void secondSignInWaitsForTheFirst()
    {
        IdentityClient account;
        IdentityClient server;

        account.startLogin(kProvider);
        QCOMPARE(m_shown.size(), 1);
        QVERIFY(account.isActive());

        server.startLogin(kProvider, kUat);
        QCOMPARE(m_shown.size(), 1);            // still just the one page
        QVERIFY(server.isActive());             // ...but it is going to happen
        QVERIFY(server.isWaitingForBrowser());
        QCOMPARE(IdentityClient::waitingForBrowserCount(), 1);
    }

    // ...and it happens as soon as the first one lets go. Promotion goes
    // through the event loop on purpose (releaseBrowser runs from destructors
    // and from inside signal handlers), so this waits a turn.
    void theWaiterGetsTheBrowserWhenTheFirstFinishes()
    {
        IdentityClient account;
        IdentityClient server;

        account.startLogin(kProvider);
        server.startLogin(kProvider, kUat);
        QCOMPARE(m_shown.size(), 1);

        account.cancel();
        QTRY_COMPARE(m_shown.size(), 2);
        QVERIFY(!server.isWaitingForBrowser());
        QCOMPARE(IdentityClient::waitingForBrowserCount(), 0);

        // The second page is the server-bound one, with everything the
        // audience binding needs (identity audit C1) — a queued attempt
        // generates its PKCE material when it is SHOWN, not when it is asked
        // for, so none of it is stale by the time it is used.
        const QUrlQuery query(m_shown.at(1).query());
        QCOMPARE(query.queryItemValue("resource"), kUat);
        QVERIFY(!query.queryItemValue("state").isEmpty());
        QVERIFY(!query.queryItemValue("nonce").isEmpty());
        QCOMPARE(query.queryItemValue("code_challenge_method"), QStringLiteral("S256"));
        QVERIFY(!query.queryItemValue("code_challenge").isEmpty());

        // ...and it is a different attempt from the first, not a replay of it.
        const QUrlQuery first(m_shown.at(0).query());
        QVERIFY(query.queryItemValue("state") != first.queryItemValue("state"));
        QVERIFY(query.queryItemValue("code_challenge") != first.queryItemValue("code_challenge"));
    }

    // Joining several servers genuinely needs several authorizations: an
    // id_token is audience-bound to exactly ONE of them (identity/OidcRequest.h),
    // so they cannot be collapsed. They can be sequenced, and must be.
    void manyServersAreSignedInToOneAtATime()
    {
        IdentityClient account;
        IdentityClient first;
        IdentityClient second;

        account.startLogin(kProvider);
        first.startLogin(kProvider, kUat);
        second.startLogin(kProvider, kOther);

        QCOMPARE(m_shown.size(), 1);
        QCOMPARE(IdentityClient::waitingForBrowserCount(), 2);

        account.cancel();
        QTRY_COMPARE(m_shown.size(), 2);
        QCOMPARE(IdentityClient::waitingForBrowserCount(), 1);

        first.cancel();
        QTRY_COMPARE(m_shown.size(), 3);
        QCOMPARE(IdentityClient::waitingForBrowserCount(), 0);

        // In order, and each for its own server.
        QCOMPARE(QUrlQuery(m_shown.at(0).query()).queryItemValue("resource"), QString());
        QCOMPARE(QUrlQuery(m_shown.at(1).query()).queryItemValue("resource"), kUat);
        QCOMPARE(QUrlQuery(m_shown.at(2).query()).queryItemValue("resource"), kOther);
    }

    // Two attempts at the SAME server are not two sign-ins. Queueing them
    // would make the user approve the same server twice in a row, which is
    // not a fix for having asked twice.
    void aDuplicateSignInIsRefusedRatherThanQueued()
    {
        IdentityClient first;
        IdentityClient duplicate;
        QSignalSpy failures(&duplicate, &IdentityClient::loginFailed);

        first.startLogin(kProvider, kUat);
        duplicate.startLogin(kProvider, kUat);

        QCOMPARE(m_shown.size(), 1);
        QCOMPARE(IdentityClient::waitingForBrowserCount(), 0);
        QCOMPARE(failures.size(), 1);
        QVERIFY(!duplicate.isActive());
        // ...and the one that IS running is left alone.
        QVERIFY(first.isActive());
    }

    // Restarting your own sign-in is one sign-in, not a duplicate of itself.
    void restartingTheSameClientIsNotADuplicate()
    {
        IdentityClient client;
        QSignalSpy failures(&client, &IdentityClient::loginFailed);

        client.startLogin(kProvider, kUat);
        client.startLogin(kProvider, kUat);

        QCOMPARE(failures.size(), 0);
        QCOMPARE(m_shown.size(), 2);
        QVERIFY(client.isActive());
    }

    // A waiter destroyed before its turn must not strand the queue behind a
    // dangling pointer.
    void aWaiterDestroyedWhileWaitingIsSkipped()
    {
        IdentityClient account;
        auto* abandoned = new IdentityClient;
        IdentityClient survivor;

        account.startLogin(kProvider);
        abandoned->startLogin(kProvider, kUat);
        survivor.startLogin(kProvider, kOther);
        QCOMPARE(IdentityClient::waitingForBrowserCount(), 2);

        delete abandoned;
        QCOMPARE(IdentityClient::waitingForBrowserCount(), 1);

        account.cancel();
        QTRY_COMPARE(m_shown.size(), 2);
        QCOMPARE(QUrlQuery(m_shown.at(1).query()).queryItemValue("resource"), kOther);
    }

    // Cancelling the attempt that is merely WAITING must not disturb the one
    // that has the browser open.
    void cancellingAWaiterLeavesTheOpenPageAlone()
    {
        IdentityClient account;
        IdentityClient waiter;

        account.startLogin(kProvider);
        waiter.startLogin(kProvider, kUat);
        QCOMPARE(IdentityClient::waitingForBrowserCount(), 1);

        waiter.cancel();
        QCOMPARE(IdentityClient::waitingForBrowserCount(), 0);
        QVERIFY(!waiter.isActive());
        QVERIFY(account.isActive());

        // Nothing new was shown, and the queue did not promote a ghost.
        QTest::qWait(10);
        QCOMPARE(m_shown.size(), 1);
    }

private:
    QVector<QUrl> m_shown;
};

QTEST_GUILESS_MAIN(OidcSerialisationTest)
#include "test_oidc_serialisation.moc"
