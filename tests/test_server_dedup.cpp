// "Are we already connected to this server?" — the rule that stops a second
// ServerConnection, and therefore a second /sync loop, being created for a
// homeserver the client is already signed into.
//
// THE BUG
//
// A desktop client was observed running TWO /sync long polls against the
// same homeserver with the same token, forever. Every request and every
// reply was doubled, microseconds apart:
//
//   03:26:20.825 /sync -> since="t_49a4…" timeout=30000ms
//   03:26:20.825 /sync -> since="t_49a4…" timeout=30000ms
//   03:26:50.849 /sync rt=30023ms rooms=0 events=0 progressed=false
//   03:26:50.861 /sync rt=30035ms rooms=0 events=0 progressed=false
//
// Only one process was running, and the two chains reported DIFFERENT
// elapsed times for polls issued in the same millisecond — so they had
// separate QElapsedTimers, so they were separate SyncLoop objects, so they
// were separate ServerConnections. The settings file confirmed it: two
// saved rows for `https://chat.bsfchat.com` under one user id, and both
// loops started 154ms apart at the next cold launch.
//
// They got there because three of ServerManager's four add paths appended a
// connection without ever asking whether one already existed, and
// persistCredentials then wrote the duplicate to disk.
//
// This file pins the policy (net/ServerDedup.h). It is deliberately free of
// Settings, ServerConnection, an event loop and the network.

#include <QTest>

#include "net/ServerDedup.h"

using bsfchat::client::ServerIdentity;
using bsfchat::client::duplicateServerRows;
using bsfchat::client::indexOfExistingServer;
using bsfchat::client::isSameServerAccount;
using bsfchat::client::serverComparisonKey;

class TestServerDedup : public QObject {
    Q_OBJECT

private slots:
    void keyIgnoresTheDifferencesThatAreNotDifferences_data()
    {
        QTest::addColumn<QString>("a");
        QTest::addColumn<QString>("b");

        QTest::newRow("trailing slash")
            << "https://chat.bsfchat.com" << "https://chat.bsfchat.com/";
        QTest::newRow("several trailing slashes")
            << "https://chat.bsfchat.com" << "https://chat.bsfchat.com///";
        QTest::newRow("host case")
            << "https://chat.bsfchat.com" << "https://CHAT.BSFChat.COM";
        QTest::newRow("scheme case")
            << "https://chat.bsfchat.com" << "HTTPS://chat.bsfchat.com";
        QTest::newRow("explicit default port")
            << "https://chat.bsfchat.com" << "https://chat.bsfchat.com:443";
        QTest::newRow("surrounding whitespace")
            << "https://chat.bsfchat.com" << "  https://chat.bsfchat.com  ";
        QTest::newRow("bare host defaults to https")
            << "https://chat.bsfchat.com" << "chat.bsfchat.com";
    }

    // These are exactly the shapes that made the ONE dedup check that did
    // exist (the identity-driven server list) miss: it compared the raw
    // string the identity service returned against the normalised string in
    // the roster.
    void keyIgnoresTheDifferencesThatAreNotDifferences()
    {
        QFETCH(QString, a);
        QFETCH(QString, b);
        QCOMPARE(serverComparisonKey(a), serverComparisonKey(b));
    }

    void keyKeepsTheDifferencesThatAre()
    {
        QVERIFY(serverComparisonKey("https://chat.bsfchat.com")
                != serverComparisonKey("https://other.bsfchat.com"));
        // A non-default port is a different server.
        QVERIFY(serverComparisonKey("https://chat.bsfchat.com")
                != serverComparisonKey("https://chat.bsfchat.com:8448"));
        // A homeserver can live under a path prefix.
        QVERIFY(serverComparisonKey("https://chat.bsfchat.com")
                != serverComparisonKey("https://chat.bsfchat.com/matrix"));
        // http is not https.
        QVERIFY(serverComparisonKey("http://chat.bsfchat.com")
                != serverComparisonKey("https://chat.bsfchat.com"));
    }

    void sameAccountOnSameHostIsADuplicate()
    {
        const ServerIdentity a{"https://chat.bsfchat.com", "@josh:chat.bsfchat.com"};
        const ServerIdentity b{"https://chat.bsfchat.com/", "@josh:chat.bsfchat.com"};
        QVERIFY(isSameServerAccount(a, b));
    }

    void differentAccountsOnOneHostAreNot()
    {
        const ServerIdentity a{"https://chat.bsfchat.com", "@josh:chat.bsfchat.com"};
        const ServerIdentity b{"https://chat.bsfchat.com", "@sam:chat.bsfchat.com"};
        QVERIFY(!isSameServerAccount(a, b));
    }

    // The add flows all begin with no user id, and that is precisely when a
    // duplicate gets created — the user is re-adding a server they are
    // already on. An unknown account on a known host must therefore MATCH,
    // so the add path can adopt the existing connection instead of building
    // a parallel one.
    void anUnauthenticatedAddToAKnownHostMatches()
    {
        const ServerIdentity known{"https://chat.bsfchat.com", "@josh:chat.bsfchat.com"};
        const ServerIdentity adding{"https://chat.bsfchat.com", QString()};
        QVERIFY(isSameServerAccount(known, adding));
        QVERIFY(isSameServerAccount(adding, known));
    }

    void anEmptyUrlNeverMatchesAnything()
    {
        const ServerIdentity empty{QString(), QString()};
        const ServerIdentity real{"https://chat.bsfchat.com", "@josh:chat.bsfchat.com"};
        QVERIFY(!isSameServerAccount(empty, real));
        QVERIFY(!isSameServerAccount(empty, empty));
    }

    void indexOfExistingFindsTheFirstMatch()
    {
        const QList<ServerIdentity> roster{
            {"https://a.example", "@josh:a.example"},
            {"https://chat.bsfchat.com", "@josh:chat.bsfchat.com"},
            {"https://b.example", "@josh:b.example"},
        };
        QCOMPARE(indexOfExistingServer(
                     roster, {"https://chat.bsfchat.com/", QString()}),
                 1);
        QCOMPARE(indexOfExistingServer(roster, {"https://c.example", QString()}), -1);
    }

    // The real settings file recovered from the affected client: three rows,
    // one homeserver, one account, three device ids. Restoring it started
    // one sync loop per row.
    void theObservedDuplicateSettingsAreDetected()
    {
        const QString url = QStringLiteral("https://chat.bsfchat.com");
        const QString user =
            QStringLiteral("@@oidc_a5cdbefe-9003-4f09-b87e-238ba8df3264:chat.bsfchat.com");
        const QList<ServerIdentity> saved{{url, user}, {url, user}, {url, user}};

        const QList<int> dupes = duplicateServerRows(saved);
        // Row 0 survives; 1 and 2 are the duplicates.
        QCOMPARE(dupes, (QList<int>{1, 2}));
    }

    void duplicateRowsKeepsTheFirstOccurrence()
    {
        const QList<ServerIdentity> saved{
            {"https://a.example", "@josh:a.example"},
            {"https://chat.bsfchat.com", "@josh:chat.bsfchat.com"},
            {"https://a.example/", "@josh:a.example"},   // dupe of row 0
            {"https://b.example", "@josh:b.example"},
        };
        QCOMPARE(duplicateServerRows(saved), (QList<int>{2}));
    }

    void aCleanRosterHasNoDuplicates()
    {
        const QList<ServerIdentity> saved{
            {"https://a.example", "@josh:a.example"},
            {"https://b.example", "@josh:b.example"},
            {"https://chat.bsfchat.com", "@josh:chat.bsfchat.com"},
        };
        QVERIFY(duplicateServerRows(saved).isEmpty());
    }

    // Two accounts on one homeserver is legitimate and must survive a
    // restore untouched — the repair pass must not silently sign one of
    // them out.
    void twoAccountsOnOneHostAreNotPruned()
    {
        const QList<ServerIdentity> saved{
            {"https://chat.bsfchat.com", "@josh:chat.bsfchat.com"},
            {"https://chat.bsfchat.com", "@bot:chat.bsfchat.com"},
        };
        QVERIFY(duplicateServerRows(saved).isEmpty());
    }
};

QTEST_MAIN(TestServerDedup)
#include "test_server_dedup.moc"
