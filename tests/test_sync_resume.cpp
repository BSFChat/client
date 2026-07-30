// Covers the two halves of "don't pay for a full initial sync on every
// launch, and don't hammer a server that's down":
//
//   * LocalCache — the persisted (user, homeserver) sync token and room
//     snapshot, including every path that must degrade to "no cache" rather
//     than resume against wrong state.
//   * SyncBackoff — the retry schedule, jitter bounds, and the errcode
//     classification that decides whether a `since` token gets abandoned.
//
// Both are pure/file-local: no network, no event loop, no server.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTest>

#include "net/SyncBackoff.h"
#include "store/LocalCache.h"

#include <bsfchat/Constants.h>
#include <bsfchat/MatrixTypes.h>

class TestSyncResume : public QObject {
    Q_OBJECT

private:
    static bsfchat::RoomEvent nameEvent(const std::string& roomId,
                                         const std::string& name)
    {
        bsfchat::RoomEvent e;
        e.event_id = "$name_" + roomId;
        e.room_id = roomId;
        e.sender = "@admin:server";
        e.type = std::string(bsfchat::event_type::kRoomName);
        e.state_key = "";
        e.origin_server_ts = 1234;
        e.content.data = {{"name", name}};
        return e;
    }

    static bsfchat::RoomEvent memberEvent(const std::string& roomId,
                                           const std::string& user,
                                           const std::string& displayName)
    {
        bsfchat::RoomEvent e;
        e.event_id = "$member_" + roomId + user;
        e.room_id = roomId;
        e.sender = user;
        e.type = std::string(bsfchat::event_type::kRoomMember);
        e.state_key = user;
        e.origin_server_ts = 2345;
        e.content.data = {{"membership", "join"}, {"displayname", displayName}};
        return e;
    }

    // A response shaped like an initial sync: two rooms with state, unread
    // counts, and a next_batch.
    static bsfchat::SyncResponse initialSync(const std::string& nextBatch = "s42")
    {
        bsfchat::SyncResponse r;
        r.next_batch = nextBatch;

        auto& general = r.rooms.join["!general:server"];
        general.state.events.push_back(nameEvent("!general:server", "general"));
        general.state.events.push_back(
            memberEvent("!general:server", "@alice:server", "Alice"));
        general.unread_count = 3;

        auto& random = r.rooms.join["!random:server"];
        random.state.events.push_back(nameEvent("!random:server", "random"));
        random.unread_count = 0;
        return r;
    }

private slots:
    void initTestCase()
    {
        // Keeps every cache file this test writes inside the Qt test sandbox
        // (~/.qttest/...) instead of the user's real BSFChat profile.
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("BSFChatTest"));
        QCoreApplication::setApplicationName(QStringLiteral("test_sync_resume"));
        cleanup();
    }

    void cleanup()
    {
        // Each test starts from a genuinely empty profile; a leftover file
        // would let a "first run" case accidentally pass by resuming.
        const QString dir = QStandardPaths::writableLocation(
                                QStandardPaths::AppDataLocation)
                          + QStringLiteral("/cache");
        QDir(dir).removeRecursively();
    }

    // --- token shape -----------------------------------------------------

    void testTokenValidation()
    {
        QVERIFY(LocalCache::isValidSyncToken(QStringLiteral("s0")));
        QVERIFY(LocalCache::isValidSyncToken(QStringLiteral("s1")));
        QVERIFY(LocalCache::isValidSyncToken(QStringLiteral("s1234567890")));

        // The server does not reject a token it can't parse — it silently
        // treats it as position 0 and answers with a state-less incremental
        // sync, which is exactly the "resumed into an empty sidebar" failure.
        // So anything off-shape has to be caught here.
        QVERIFY(!LocalCache::isValidSyncToken(QString()));
        QVERIFY(!LocalCache::isValidSyncToken(QStringLiteral("s")));
        QVERIFY(!LocalCache::isValidSyncToken(QStringLiteral("42")));
        QVERIFY(!LocalCache::isValidSyncToken(QStringLiteral("t42")));
        QVERIFY(!LocalCache::isValidSyncToken(QStringLiteral("s4 2")));
        QVERIFY(!LocalCache::isValidSyncToken(QStringLiteral("s-1")));
        QVERIFY(!LocalCache::isValidSyncToken(QStringLiteral("s12a")));
        QVERIFY(!LocalCache::isValidSyncToken(QStringLiteral("sABC")));
        QVERIFY(!LocalCache::isValidSyncToken(
            QStringLiteral("s") + QString(64, QLatin1Char('9'))));
    }

    // --- first run -------------------------------------------------------

    void testFirstRunHasNothingToResumeFrom()
    {
        LocalCache cache;
        QVERIFY(cache.open(QStringLiteral("@josh:h"), QStringLiteral("https://h")));
        QVERIFY(cache.isOpen());
        QVERIFY(cache.syncToken().isEmpty());
        QCOMPARE(cache.syncTokenAgeMs(), -1);
        QVERIFY(cache.cachedRoomIds().isEmpty());

        bsfchat::SyncResponse hydrated;
        QVERIFY(!cache.buildHydrationSync(hydrated));
        QVERIFY(hydrated.rooms.join.empty());
    }

    void testOpenRejectsEmptyIdentity()
    {
        LocalCache cache;
        QVERIFY(!cache.open(QString(), QStringLiteral("https://h")));
        QVERIFY(!cache.open(QStringLiteral("@josh:h"), QString()));
        QVERIFY(!cache.isOpen());
        // Every accessor has to stay safe on a closed cache.
        QVERIFY(cache.syncToken().isEmpty());
        QVERIFY(cache.cachedRoomIds().isEmpty());
        bsfchat::SyncResponse out;
        QVERIFY(!cache.buildHydrationSync(out));
        cache.cacheSyncToken(QStringLiteral("s1"));
        QVERIFY(cache.syncToken().isEmpty());
    }

    // --- round trip ------------------------------------------------------

    void testTokenAndSnapshotSurviveRestart()
    {
        const QString user = QStringLiteral("@josh:h");
        const QString host = QStringLiteral("https://h");
        {
            LocalCache cache;
            QVERIFY(cache.open(user, host));
            cache.recordSync(initialSync("s42"));
            QCOMPARE(cache.syncToken(), QStringLiteral("s42"));
        }

        // Second process, same account.
        LocalCache reopened;
        QVERIFY(reopened.open(user, host));
        QCOMPARE(reopened.syncToken(), QStringLiteral("s42"));
        QVERIFY(reopened.syncTokenAgeMs() >= 0);
        QCOMPARE(reopened.cachedRoomIds().size(), 2);

        bsfchat::SyncResponse hydrated;
        QVERIFY(reopened.buildHydrationSync(hydrated));
        QCOMPARE(hydrated.rooms.join.size(), std::size_t(2));
        // next_batch must stay empty: a replay has not advanced the stream.
        QVERIFY(hydrated.next_batch.empty());

        const auto& general = hydrated.rooms.join.at("!general:server");
        QCOMPARE(general.state.events.size(), std::size_t(2));
        QVERIFY(general.timeline.events.empty()); // never replay history
        QCOMPARE(general.unread_count.value_or(-1), 3);

        bool sawName = false;
        bool sawMember = false;
        for (const auto& e : general.state.events) {
            if (e.type == std::string(bsfchat::event_type::kRoomName)) {
                QCOMPARE(e.content.data.value("name", ""), "general");
                sawName = true;
            } else if (e.type == std::string(bsfchat::event_type::kRoomMember)) {
                QVERIFY(e.state_key.has_value());
                QCOMPARE(*e.state_key, "@alice:server");
                QCOMPARE(e.content.data.value("displayname", ""), "Alice");
                sawMember = true;
            }
        }
        QVERIFY(sawName);
        QVERIFY(sawMember);
    }

    void testIncrementalStateReplacesRatherThanDuplicates()
    {
        LocalCache cache;
        QVERIFY(cache.open(QStringLiteral("@josh:h"), QStringLiteral("https://h")));
        cache.recordSync(initialSync("s42"));

        // An incremental sync carries only the state that changed.
        bsfchat::SyncResponse delta;
        delta.next_batch = "s43";
        auto& general = delta.rooms.join["!general:server"];
        general.state.events.push_back(nameEvent("!general:server", "general-renamed"));
        cache.recordSync(delta);

        QCOMPARE(cache.syncToken(), QStringLiteral("s43"));
        bsfchat::SyncResponse hydrated;
        QVERIFY(cache.buildHydrationSync(hydrated));
        const auto& room = hydrated.rooms.join.at("!general:server");
        // Still two rows: the name row was replaced, the member row kept.
        QCOMPARE(room.state.events.size(), std::size_t(2));
        for (const auto& e : room.state.events) {
            if (e.type == std::string(bsfchat::event_type::kRoomName))
                QCOMPARE(e.content.data.value("name", ""), "general-renamed");
        }
        // A delta that omits unread_count must not clear the badge.
        QCOMPARE(room.unread_count.value_or(-1), 3);
        // The untouched room survives the delta.
        QCOMPARE(hydrated.rooms.join.count("!random:server"), std::size_t(1));
    }

    // --- isolation -------------------------------------------------------

    void testAccountsAndServersDoNotShareState()
    {
        LocalCache a;
        LocalCache b;
        LocalCache c;
        QVERIFY(a.open(QStringLiteral("@josh:h"), QStringLiteral("https://h")));
        QVERIFY(b.open(QStringLiteral("@other:h"), QStringLiteral("https://h")));
        QVERIFY(c.open(QStringLiteral("@josh:h"), QStringLiteral("https://other")));

        QVERIFY(a.databasePath() != b.databasePath());
        QVERIFY(a.databasePath() != c.databasePath());

        a.recordSync(initialSync("s100"));
        QCOMPARE(a.syncToken(), QStringLiteral("s100"));
        // Handing @josh's stream position to another account, or to another
        // server whose positions are unrelated integers, is the worst
        // available failure — it would resume into someone else's timeline.
        QVERIFY(b.syncToken().isEmpty());
        QVERIFY(c.syncToken().isEmpty());
        QVERIFY(b.cachedRoomIds().isEmpty());
        QVERIFY(c.cachedRoomIds().isEmpty());
    }

    // --- refusal / recovery ---------------------------------------------

    void testMalformedTokensAreNeverPersisted()
    {
        LocalCache cache;
        QVERIFY(cache.open(QStringLiteral("@josh:h"), QStringLiteral("https://h")));

        cache.cacheSyncToken(QStringLiteral("not-a-token"));
        QVERIFY(cache.syncToken().isEmpty());

        cache.cacheSyncToken(QStringLiteral("s7"));
        QCOMPARE(cache.syncToken(), QStringLiteral("s7"));

        // A later bad value must not overwrite a good one either.
        cache.cacheSyncToken(QStringLiteral("garbage"));
        QCOMPARE(cache.syncToken(), QStringLiteral("s7"));

        bsfchat::SyncResponse bad;
        bad.next_batch = "junk";
        bad.rooms.join["!r:server"].state.events.push_back(nameEvent("!r:server", "r"));
        cache.recordSync(bad);
        QCOMPARE(cache.syncToken(), QStringLiteral("s7"));
    }

    void testClearSyncTokenKeepsSnapshot()
    {
        LocalCache cache;
        QVERIFY(cache.open(QStringLiteral("@josh:h"), QStringLiteral("https://h")));
        cache.recordSync(initialSync("s42"));

        // What happens when the loop gives up on a resumed token: the token
        // goes, the snapshot can stay (the following full sync overwrites it).
        cache.clearSyncToken();
        QVERIFY(cache.syncToken().isEmpty());
        QCOMPARE(cache.syncTokenAgeMs(), -1);
        QCOMPARE(cache.cachedRoomIds().size(), 2);

        cache.clearAll();
        QVERIFY(cache.cachedRoomIds().isEmpty());
        bsfchat::SyncResponse hydrated;
        QVERIFY(!cache.buildHydrationSync(hydrated));
    }

    void testCorruptDatabaseFallsBackInsteadOfFailing()
    {
        const QString user = QStringLiteral("@josh:h");
        const QString host = QStringLiteral("https://h");
        QString path;
        {
            LocalCache cache;
            QVERIFY(cache.open(user, host));
            cache.recordSync(initialSync("s42"));
            path = cache.databasePath();
            QVERIFY(!path.isEmpty());
        }

        // Simulate a truncated / garbage SQLite image: killed mid-write, a
        // full disk, a file-sync tool. The client must come up and full-sync,
        // not refuse to start.
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(QByteArray(4096, '\xa5'));
        }

        LocalCache cache;
        QVERIFY(cache.open(user, host));
        QVERIFY(cache.isOpen());
        QVERIFY(cache.syncToken().isEmpty());
        bsfchat::SyncResponse hydrated;
        QVERIFY(!cache.buildHydrationSync(hydrated));

        // And it is usable again from here.
        cache.recordSync(initialSync("s50"));
        QCOMPARE(cache.syncToken(), QStringLiteral("s50"));
    }

    void testSchemaVersionMismatchWipesRatherThanMigrates()
    {
        const QString user = QStringLiteral("@josh:h");
        const QString host = QStringLiteral("https://h");
        QString path;
        {
            LocalCache cache;
            QVERIFY(cache.open(user, host));
            cache.recordSync(initialSync("s42"));
            path = cache.databasePath();
        }

        {
            // Pretend the file was written by a future build.
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                QStringLiteral("bump"));
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("PRAGMA user_version = %1")
                               .arg(LocalCache::kSchemaVersion + 7)));
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("bump"));

        LocalCache cache;
        QVERIFY(cache.open(user, host));
        QVERIFY(cache.syncToken().isEmpty());
        QVERIFY(cache.cachedRoomIds().isEmpty());
    }

    void testEmptySyncIsNotRecordedAsAResumePoint()
    {
        LocalCache cache;
        QVERIFY(cache.open(QStringLiteral("@josh:h"), QStringLiteral("https://h")));

        // A long-poll timeout: no rooms, and next_batch is where we already
        // were. Must not create a snapshot that looks resumable.
        bsfchat::SyncResponse empty;
        empty.next_batch = "s9";
        cache.recordSync(empty);
        QCOMPARE(cache.syncToken(), QStringLiteral("s9"));
        QVERIFY(cache.cachedRoomIds().isEmpty());

        bsfchat::SyncResponse hydrated;
        QVERIFY(!cache.buildHydrationSync(hydrated));
    }

    // --- backoff schedule ------------------------------------------------

    void testBackoffScheduleIsExponentialAndCapped()
    {
        // The old loop retried on a flat 5s timer forever. 1s → 60s, then
        // flat, so a downed server sees ~1 request/minute/client instead of
        // 12, and a client that comes back after a long outage still
        // reconnects within a minute.
        QCOMPARE(SyncBackoff::baseDelayMs(0), 1000);
        QCOMPARE(SyncBackoff::baseDelayMs(1), 2000);
        QCOMPARE(SyncBackoff::baseDelayMs(2), 4000);
        QCOMPARE(SyncBackoff::baseDelayMs(3), 8000);
        QCOMPARE(SyncBackoff::baseDelayMs(4), 16000);
        QCOMPARE(SyncBackoff::baseDelayMs(5), 32000);
        QCOMPARE(SyncBackoff::baseDelayMs(6), SyncBackoff::kMaxDelayMs);
        QCOMPARE(SyncBackoff::baseDelayMs(7), SyncBackoff::kMaxDelayMs);

        // Monotonic, never negative, and never past the cap — including for a
        // failure counter that has run away (weeks offline).
        int previous = 0;
        for (int i = 0; i < 4096; ++i) {
            const int d = SyncBackoff::baseDelayMs(i);
            QVERIFY(d >= previous);
            QVERIFY(d > 0);
            QVERIFY(d <= SyncBackoff::kMaxDelayMs);
            previous = d;
        }
        // Negative / nonsense input degrades to the base delay.
        QCOMPARE(SyncBackoff::baseDelayMs(-1), SyncBackoff::kBaseDelayMs);
    }

    void testJitterStaysWithinHalfToFullOfBase()
    {
        // Equal jitter: never faster than base/2 (so jitter can't make a
        // client retry harder than the schedule intends), never slower than
        // base (so the cap still means something).
        QCOMPARE(SyncBackoff::applyJitter(1000, 0.0), 500);
        QCOMPARE(SyncBackoff::applyJitter(1000, 1.0), 1000);
        QCOMPARE(SyncBackoff::applyJitter(0, 0.5), 0);

        for (int failures = 0; failures < 12; ++failures) {
            const int base = SyncBackoff::baseDelayMs(failures);
            for (double j : {0.0, 0.01, 0.25, 0.5, 0.75, 0.99, 1.0}) {
                const int d = SyncBackoff::delayForFailure(failures, j);
                QVERIFY(d >= base / 2);
                QVERIFY(d <= base);
            }
            // Out-of-range jitter must be clamped, not extrapolated.
            QVERIFY(SyncBackoff::delayForFailure(failures, -5.0) >= base / 2);
            QVERIFY(SyncBackoff::delayForFailure(failures, 5.0) <= base);
        }

        // Distinct jitter inputs must actually spread, or a fleet stays in
        // lockstep and hits a recovering server in the same millisecond.
        QVERIFY(SyncBackoff::delayForFailure(5, 0.1)
                != SyncBackoff::delayForFailure(5, 0.9));
    }

    void testOnlyTokenErrorsAbandonTheSyncPosition()
    {
        QVERIFY(SyncBackoff::indicatesRejectedSinceToken(
            QStringLiteral(R"({"errcode":"M_UNKNOWN","error":"bad since"})")));
        QVERIFY(SyncBackoff::indicatesRejectedSinceToken(
            QStringLiteral(R"({"errcode":"M_INVALID_PARAM"})")));
        QVERIFY(SyncBackoff::indicatesRejectedSinceToken(
            QStringLiteral(R"({"errcode":"M_BAD_JSON"})")));

        // M_UNKNOWN_TOKEN *contains* M_UNKNOWN but means the access token is
        // dead. Throwing away the sync position for it would hide a needed
        // re-login behind an expensive full sync that fails identically —
        // which is why this matches on the parsed errcode, not a substring.
        QVERIFY(!SyncBackoff::indicatesRejectedSinceToken(
            QStringLiteral(R"({"errcode":"M_UNKNOWN_TOKEN"})")));
        QVERIFY(!SyncBackoff::indicatesRejectedSinceToken(
            QStringLiteral(R"({"errcode":"M_FORBIDDEN"})")));

        // Transport failures and proxy error pages are not the server
        // rejecting our token and must not cost us the position.
        QVERIFY(!SyncBackoff::indicatesRejectedSinceToken(QString()));
        QVERIFY(!SyncBackoff::indicatesRejectedSinceToken(
            QStringLiteral("Connection refused")));
        QVERIFY(!SyncBackoff::indicatesRejectedSinceToken(
            QStringLiteral("<html><body>502 Bad Gateway</body></html>")));
        QVERIFY(!SyncBackoff::indicatesRejectedSinceToken(
            QStringLiteral(R"(["M_UNKNOWN"])")));
    }

    void testSyncFloorAndResumeCapAreSane()
    {
        // A floor at all is the point: onSyncSuccess used to re-enter
        // doSync() unconditionally, so a proxy answering 200 instantly was a
        // tight request loop. It also has to stay small enough to be
        // invisible on the live-message path.
        QVERIFY(SyncBackoff::kMinSyncIntervalMs > 0);
        QVERIFY(SyncBackoff::kMinSyncIntervalMs <= 1000);
        // A resumed token must be given up on quickly — the fallback is a
        // full sync, which always works.
        QVERIFY(SyncBackoff::kMaxResumeAttempts >= 1);
        QVERIFY(SyncBackoff::kMaxResumeAttempts <= 5);
    }
};

QTEST_GUILESS_MAIN(TestSyncResume)
#include "test_sync_resume.moc"
