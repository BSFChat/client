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

    static bsfchat::RoomEvent messageEvent(const std::string& roomId, int i)
    {
        bsfchat::RoomEvent e;
        e.event_id = "$msg" + std::to_string(i);
        e.room_id = roomId;
        e.sender = "@alice:server";
        e.type = std::string(bsfchat::event_type::kRoomMessage);
        e.origin_server_ts = 1000 + i;
        e.content.data = {{"msgtype", "m.text"},
                          {"body", "body " + std::to_string(i)}};
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
        // A real-shaped token as the server mints it now: "t_" + exactly 32
        // lowercase hex digits, 34 characters in all. next_batch used to be the
        // raw global stream head, which handed every authenticated user a
        // server-wide volume-and-timing oracle.
        //
        // Both spellings must pass. A client that accepts only the new one
        // discards a legacy token on upgrade and does a full initial sync; one
        // that accepts only the old one — which is what this validator did —
        // silently never PERSISTS the new one, so every launch either resumes
        // from an ancient position or starts from scratch. Neither errors,
        // which is what makes this worth pinning.
        const QString hex32 = QStringLiteral("0123456789abcdef0123456789abcdef");
        QCOMPARE(hex32.size(), 32);
        QVERIFY(LocalCache::isValidSyncToken(QStringLiteral("t_") + hex32));
        QVERIFY(LocalCache::isValidSyncToken(
            QStringLiteral("t_") + QString(32, QLatin1Char('f'))));

        // Shape only — the client must not interpret the contents — but the
        // shape is EXACT. The token is compared as a string by the server and
        // by SyncBackoff's no-progress guard, so a near-miss is not something
        // to repair into a token we then hand back; it is something to refuse.
        QVERIFY(!LocalCache::isValidSyncToken(
            QStringLiteral("t_") + hex32.toUpper()));         // uppercase hex
        QVERIFY(!LocalCache::isValidSyncToken(
            QStringLiteral("t_") + hex32.left(31)));          // 31 hex digits
        QVERIFY(!LocalCache::isValidSyncToken(
            QStringLiteral("t_") + hex32 + QLatin1Char('0')));// 33 hex digits
        QVERIFY(!LocalCache::isValidSyncToken(QStringLiteral("t_")));
        QVERIFY(!LocalCache::isValidSyncToken(QStringLiteral("t_deadbeef")));
        QVERIFY(!LocalCache::isValidSyncToken(
            QStringLiteral("t_") + hex32.left(31) + QLatin1Char('g')));  // non-hex
        QVERIFY(!LocalCache::isValidSyncToken(QStringLiteral("t 42")));
        QVERIFY(!LocalCache::isValidSyncToken(
            QStringLiteral("T_") + hex32));                   // uppercase prefix
        // No trimming: whitespace is a difference, not noise to be cleaned up.
        QVERIFY(!LocalCache::isValidSyncToken(
            QStringLiteral(" t_") + hex32));
        QVERIFY(!LocalCache::isValidSyncToken(
            QStringLiteral("t_") + hex32 + QLatin1Char('\n')));

        // Legacy form, still accepted so a token persisted before the upgrade
        // survives it. The server takes this on the way in for one release.
        QVERIFY(LocalCache::isValidSyncToken(QStringLiteral("s0")));
        QVERIFY(LocalCache::isValidSyncToken(QStringLiteral("s1")));
        QVERIFY(LocalCache::isValidSyncToken(QStringLiteral("s12345")));
        QVERIFY(LocalCache::isValidSyncToken(QStringLiteral("s1234567890")));
        QVERIFY(LocalCache::isValidSyncToken(
            QStringLiteral("s") + QString(19, QLatin1Char('9'))));  // u64 width

        // The server does not reject a token it can't parse — it silently
        // treats it as position 0 and answers with a state-less incremental
        // sync, which is exactly the "resumed into an empty sidebar" failure.
        // So anything off-shape has to be caught here.
        QVERIFY(!LocalCache::isValidSyncToken(QString()));
        QVERIFY(!LocalCache::isValidSyncToken(QStringLiteral("s")));
        QVERIFY(!LocalCache::isValidSyncToken(QStringLiteral("sabc")));
        QVERIFY(!LocalCache::isValidSyncToken(QStringLiteral("s-1")));
        QVERIFY(!LocalCache::isValidSyncToken(QStringLiteral("42")));
        QVERIFY(!LocalCache::isValidSyncToken(QStringLiteral("t42")));
        QVERIFY(!LocalCache::isValidSyncToken(QStringLiteral("s4 2")));
        QVERIFY(!LocalCache::isValidSyncToken(QStringLiteral("s12a")));
        QVERIFY(!LocalCache::isValidSyncToken(QStringLiteral("sABC")));
        QVERIFY(!LocalCache::isValidSyncToken(
            QStringLiteral("s") + QString(64, QLatin1Char('9'))));
        // QChar::isDigit() is Unicode-aware and would admit these; the server
        // would not parse them as the position we think we are at.
        QVERIFY(!LocalCache::isValidSyncToken(
            QStringLiteral("s1") + QChar(0x0664)));  // Arabic-Indic four
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

    void testTimelineWindowSurvivesRestartInOrder()
    {
        // The half of the cache that is for phones. iOS kills the app on
        // nearly every backgrounding, so without this every channel is cold
        // on every launch and the first open of each costs a /messages round
        // trip (p90 679 ms against production).
        const QString user = QStringLiteral("@josh:h");
        const QString host = QStringLiteral("https://h");

        QVector<bsfchat::RoomEvent> window;
        for (int i = 0; i < 5; ++i) window.append(messageEvent("!general:server", i));

        {
            LocalCache cache;
            QVERIFY(cache.open(user, host));
            cache.recordTimeline(QStringLiteral("!general:server"), window);
        }

        LocalCache reopened;
        QVERIFY(reopened.open(user, host));
        const auto loaded = reopened.timelines();
        QCOMPARE(loaded.size(), 1);
        const auto& general = loaded.value(QStringLiteral("!general:server"));
        QCOMPARE(general.size(), 5);
        // ORDER is the whole reason `ordinal` exists: event ids carry none
        // and origin_server_ts is a clock the client does not control.
        for (int i = 0; i < 5; ++i)
            QCOMPARE(general[i].event_id, "$msg" + std::to_string(i));
        QCOMPARE(general[2].content.data.value("body", ""), "body 2");
    }

    void testTimelineWriteReplacesTheWindowAndKeepsItsTail()
    {
        LocalCache cache;
        QVERIFY(cache.open(QStringLiteral("@josh:h"), QStringLiteral("https://h")));

        QVector<bsfchat::RoomEvent> first;
        for (int i = 0; i < 4; ++i) first.append(messageEvent("!r:server", i));
        cache.recordTimeline(QStringLiteral("!r:server"), first);

        // A window is a contiguous slice whose ordinals shift every time its
        // front is trimmed, so a second write REPLACES rather than merges —
        // otherwise two slices would share one numbering and come back
        // interleaved.
        QVector<bsfchat::RoomEvent> second;
        for (int i = 100; i < 102; ++i) second.append(messageEvent("!r:server", i));
        cache.recordTimeline(QStringLiteral("!r:server"), second);

        const auto loaded = cache.timelines().value(QStringLiteral("!r:server"));
        QCOMPARE(loaded.size(), 2);
        QCOMPARE(loaded[0].event_id, std::string("$msg100"));
        QCOMPARE(loaded[1].event_id, std::string("$msg101"));

        // Oversized windows keep their TAIL — the newest events, which are
        // the ones a first open needs.
        QVector<bsfchat::RoomEvent> huge;
        const int n = LocalCache::kPersistedEventsPerRoom + 20;
        for (int i = 0; i < n; ++i) huge.append(messageEvent("!r:server", i));
        cache.recordTimeline(QStringLiteral("!r:server"), huge);

        const auto capped = cache.timelines().value(QStringLiteral("!r:server"));
        QCOMPARE(capped.size(), LocalCache::kPersistedEventsPerRoom);
        QCOMPARE(capped.first().event_id, std::string("$msg20"));
        QCOMPARE(capped.last().event_id, "$msg" + std::to_string(n - 1));
    }

    void testClearAllDropsTheTimelineToo()
    {
        LocalCache cache;
        QVERIFY(cache.open(QStringLiteral("@josh:h"), QStringLiteral("https://h")));
        cache.recordSync(initialSync("s7"));
        cache.recordTimeline(QStringLiteral("!general:server"),
                             {messageEvent("!general:server", 1)});
        QVERIFY(!cache.timelines().isEmpty());

        // A cache that turns out to belong to somebody else must not leave
        // one account's messages behind for the next.
        cache.clearAll();
        QVERIFY(cache.timelines().isEmpty());
        QVERIFY(cache.cachedRoomIds().isEmpty());
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

    // --- no-progress classification --------------------------------------
    //
    // This decides whether a SUCCESSFUL /sync is answered with an immediate
    // re-poll or with a backoff delay, so getting it wrong in the strict
    // direction is a latency bug, not a wasted request: a client pushed onto
    // the escalating curve has no request in flight for up to a minute, and
    // the next message addressed to it waits that long.

    void testEmptyFastReplyIsNoProgress()
    {
        // The case the guard exists for: an endpoint answering 200 instantly
        // with nothing and never moving the token. Re-entering with no floor
        // is what made this a tight request loop.
        QVERIFY(SyncBackoff::isNoProgressReply(true, false, 0));
    }

    void testEphemeralOnlyReplyIsProgress()
    {
        // A typing notification or a presence change wakes a long poll and
        // moves no timeline events, so next_batch stands still. The server is
        // plainly alive and must not cost the client a backoff — this is the
        // regression: typing at somebody walked their poll interval out.
        QVERIFY(!SyncBackoff::isNoProgressReply(true, false, 1));
        QVERIFY(!SyncBackoff::isNoProgressReply(true, false, 7));
    }

    void testSlowReplyIsNeverNoProgress()
    {
        // A reply that outlasted the floor genuinely blocked, which no
        // unconditional-200 endpoint does — even with an empty payload.
        QVERIFY(!SyncBackoff::isNoProgressReply(false, false, 0));
    }

    void testAdvancedTokenIsAlwaysProgress()
    {
        QVERIFY(!SyncBackoff::isNoProgressReply(true, true, 0));
        QVERIFY(!SyncBackoff::isNoProgressReply(false, true, 0));
    }

    void testNoProgressCurveStillEscalatesAndCaps()
    {
        // The backoff a no-progress reply is answered with is the same curve
        // as an error's, so a broken 200 cannot spin faster than a broken
        // connection. Worth pinning: this is the delay a real message waits
        // behind when the classification above gets it wrong.
        QCOMPARE(SyncBackoff::baseDelayMs(0), SyncBackoff::kBaseDelayMs);
        QVERIFY(SyncBackoff::baseDelayMs(1) > SyncBackoff::baseDelayMs(0));
        QCOMPARE(SyncBackoff::baseDelayMs(100), SyncBackoff::kMaxDelayMs);
    }

};

QTEST_GUILESS_MAIN(TestSyncResume)
#include "test_sync_resume.moc"
