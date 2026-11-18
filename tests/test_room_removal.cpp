// A room that is gone for this account has to leave the sidebar (audit R-1).
//
// Before this, nothing told other devices that a channel was deleted, that the
// account left / was kicked / was banned elsewhere, or that VIEW_CHANNEL was
// taken away: the room sat in the sidebar answering 403 forever. The client
// now learns it two ways — `rooms.leave` in /sync, read from the raw body
// because the protocol's SyncResponse has no field for it, and a GET
// /joined_rooms reconcile for servers that send nothing — and the decisions
// behind both are the pure functions in net/RoomRemoval.h, pinned here.
//
// LocalCache::forgetRoom is here too: without it the next launch hydrates a
// gone room straight back from the snapshot.

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QTest>

#include "net/RoomRemoval.h"
#include "store/LocalCache.h"

#include <bsfchat/MatrixTypes.h>
#include <nlohmann/json.hpp>

using bsfchat::client::SidebarRoom;
using bsfchat::client::SyncLeave;
using bsfchat::client::parseSyncLeaves;
using bsfchat::client::roomsToDropAfterReconcile;
using bsfchat::client::roomsToDropForSyncLeaves;

namespace {

const QString kMe = QStringLiteral("@me:h");

bsfchat::RoomEvent member(const QString& userId, const char* membership, qint64 ts)
{
    bsfchat::RoomEvent ev;
    ev.type = "m.room.member";
    ev.state_key = userId.toStdString();
    ev.sender = userId.toStdString();
    ev.content.data = nlohmann::json{{"membership", membership}};
    ev.origin_server_ts = ts;
    return ev;
}

bsfchat::RoomEvent stateEvent(const char* type, nlohmann::json content)
{
    bsfchat::RoomEvent ev;
    ev.type = type;
    ev.state_key = std::string();
    ev.content.data = std::move(content);
    return ev;
}

QSet<QString> set(std::initializer_list<const char*> ids)
{
    QSet<QString> s;
    for (const char* id : ids) s.insert(QString::fromLatin1(id));
    return s;
}

QSet<QString> asSet(const QStringList& l) { return QSet<QString>(l.begin(), l.end()); }

} // namespace

class TestRoomRemoval : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("BSFChatTest"));
        QCoreApplication::setApplicationName(QStringLiteral("test_room_removal"));
        cleanup();
    }

    void cleanup()
    {
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
             + QStringLiteral("/cache")).removeRecursively();
    }

    // --- rooms.leave parsing ---------------------------------------------

    void parsesTheWireFormat()
    {
        const auto body = nlohmann::json::parse(R"({
            "next_batch": "s9",
            "rooms": {
                "join": {},
                "leave": {
                    "!kicked:h": {
                        "bsfchat.reason": "kick",
                        "timeline": {"events": [
                            {"type": "m.room.member", "state_key": "@me:h",
                             "sender": "@mod:h", "origin_server_ts": 500,
                             "content": {"membership": "leave"}},
                            {"type": "m.room.message", "content": {"body": "x"}}
                        ]}
                    },
                    "!deleted:h": {"bsfchat.reason": "deleted"},
                    "!bare:h": {}
                }
            }
        })");
        const auto leaves = parseSyncLeaves(body);
        QCOMPARE(leaves.size(), 3);

        QHash<QString, SyncLeave> byId;
        for (const auto& l : leaves) byId.insert(l.roomId, l);
        QVERIFY(byId.contains(QStringLiteral("!kicked:h")));
        QVERIFY(byId.contains(QStringLiteral("!deleted:h")));
        QVERIFY(byId.contains(QStringLiteral("!bare:h")));

        const auto& kicked = byId[QStringLiteral("!kicked:h")];
        QCOMPARE(kicked.reason, QStringLiteral("kick"));
        QCOMPARE(kicked.members.size(), 1);  // the message is not a member event
        QCOMPARE(kicked.members[0].userId, kMe);
        QCOMPARE(kicked.members[0].membership, QStringLiteral("leave"));
        QCOMPARE(kicked.members[0].ts, qint64(500));

        QCOMPARE(byId[QStringLiteral("!deleted:h")].reason, QStringLiteral("deleted"));
        QVERIFY(byId[QStringLiteral("!bare:h")].reason.isEmpty());
        QVERIFY(byId[QStringLiteral("!bare:h")].members.isEmpty());
    }

    void absentOrMalformedLeaveSectionYieldsNothingAndNeverThrows()
    {
        // Every server that predates the section.
        QVERIFY(parseSyncLeaves(nlohmann::json::parse(
            R"({"next_batch":"s1","rooms":{"join":{}}})")).isEmpty());
        QVERIFY(parseSyncLeaves(nlohmann::json::parse(R"({"next_batch":"s1"})")).isEmpty());
        // Wrong shapes at every level.
        QVERIFY(parseSyncLeaves(nlohmann::json::parse(R"([])")).isEmpty());
        QVERIFY(parseSyncLeaves(nlohmann::json::parse(R"({"rooms":[]})")).isEmpty());
        QVERIFY(parseSyncLeaves(nlohmann::json::parse(R"({"rooms":{"leave":[]}})")).isEmpty());
        QVERIFY(parseSyncLeaves(nlohmann::json::parse(R"({"rooms":{"leave":"x"}})")).isEmpty());

        // A garbled VALUE still names the room: the key is the instruction.
        const auto leaves = parseSyncLeaves(nlohmann::json::parse(R"({"rooms":{"leave":{
            "!a:h": 7,
            "!b:h": {"bsfchat.reason": 3, "timeline": []},
            "!c:h": {"timeline": {"events": [1, {"type":"m.room.member"},
                     {"type":"m.room.member","state_key":"@me:h","origin_server_ts":"x"}]}}
        }}})"));
        QCOMPARE(leaves.size(), 3);
        for (const auto& l : leaves) QVERIFY(l.reason.isEmpty());
        for (const auto& l : leaves) {
            if (l.roomId == QLatin1String("!c:h")) {
                QCOMPARE(l.members.size(), 1);
                QCOMPARE(l.members[0].ts, qint64(-1));
            }
        }
    }

    // --- join vs leave in one batch --------------------------------------

    void leaveOnlyIsDropped()
    {
        bsfchat::SyncResponse resp;
        const QVector<SyncLeave> leaves{{QStringLiteral("!r:h"), QStringLiteral("deleted"), {}}};
        QCOMPARE(roomsToDropForSyncLeaves(leaves, resp, kMe), QStringList{QStringLiteral("!r:h")});
    }

    void rejoinLaterInTheSameBatchKeepsTheRoom()
    {
        bsfchat::SyncResponse resp;
        resp.rooms.join["!r:h"].timeline.events.push_back(member(kMe, "join", 200));
        SyncLeave leave{QStringLiteral("!r:h"), QStringLiteral("leave"), {}};
        leave.members.append({kMe, QStringLiteral("leave"), 100});
        QVERIFY(roomsToDropForSyncLeaves({leave}, resp, kMe).isEmpty());
    }

    void joinThenLeaveInTheSameBatchDropsTheRoom()
    {
        bsfchat::SyncResponse resp;
        resp.rooms.join["!r:h"].timeline.events.push_back(member(kMe, "join", 100));
        SyncLeave leave{QStringLiteral("!r:h"), QStringLiteral("kick"), {}};
        leave.members.append({kMe, QStringLiteral("leave"), 200});
        QCOMPARE(roomsToDropForSyncLeaves({leave}, resp, kMe), QStringList{QStringLiteral("!r:h")});
    }

    void joinWithoutProvableOrderDropsTheRoom()
    {
        // No own member event in the leave: nothing can be proven later than
        // it, so the server's "gone" wins.
        bsfchat::SyncResponse resp;
        resp.rooms.join["!r:h"].timeline.events.push_back(member(kMe, "join", 999));
        const QVector<SyncLeave> leaves{{QStringLiteral("!r:h"), QStringLiteral("no_access"), {}}};
        QCOMPARE(roomsToDropForSyncLeaves(leaves, resp, kMe), QStringList{QStringLiteral("!r:h")});
    }

    void somebodyElsesJoinDoesNotCount()
    {
        bsfchat::SyncResponse resp;
        resp.rooms.join["!r:h"].timeline.events.push_back(
            member(QStringLiteral("@other:h"), "join", 300));
        SyncLeave leave{QStringLiteral("!r:h"), QStringLiteral("leave"), {}};
        leave.members.append({kMe, QStringLiteral("leave"), 100});
        QCOMPARE(roomsToDropForSyncLeaves({leave}, resp, kMe), QStringList{QStringLiteral("!r:h")});
    }

    void duplicateAndEmptyIdsAreIgnored()
    {
        bsfchat::SyncResponse resp;
        const QVector<SyncLeave> leaves{{QStringLiteral("!r:h"), {}, {}},
                                        {QStringLiteral("!r:h"), {}, {}},
                                        {QString(), {}, {}}};
        QCOMPARE(roomsToDropForSyncLeaves(leaves, resp, kMe), QStringList{QStringLiteral("!r:h")});
    }

    // --- /joined_rooms reconcile -----------------------------------------

    void channelsAndDmsMissingFromJoinedAreDropped()
    {
        const QVector<SidebarRoom> sidebar{
            {QStringLiteral("!general:h"), false, {}},
            {QStringLiteral("!deleted:h"), false, {}},
            {QStringLiteral("!dm:h"), false, {}},
            {QStringLiteral("!olddm:h"), false, {}},
        };
        const auto drop = roomsToDropAfterReconcile(
            sidebar, set({"!general:h", "!dm:h"}), {}, {});
        QCOMPARE(asSet(drop), set({"!deleted:h", "!olddm:h"}));
    }

    void everythingJoinedDropsNothing()
    {
        const QVector<SidebarRoom> sidebar{
            {QStringLiteral("!cat:h"), true, {}},
            {QStringLiteral("!a:h"), false, QStringLiteral("!cat:h")},
        };
        QVERIFY(roomsToDropAfterReconcile(sidebar, set({"!cat:h", "!a:h"}), {}, {}).isEmpty());
    }

    void pendingInvitesAndProtectedRoomsAreKept()
    {
        const QVector<SidebarRoom> sidebar{
            {QStringLiteral("!invited:h"), false, {}},
            {QStringLiteral("!new:h"), false, {}},   // arrived after the request
            {QStringLiteral("!gone:h"), false, {}},
        };
        const auto drop = roomsToDropAfterReconcile(
            sidebar, {}, set({"!invited:h"}), set({"!new:h"}));
        QCOMPARE(drop, QStringList{QStringLiteral("!gone:h")});
    }

    void categoryNotListedIsKeptWhileItStillHasChannels()
    {
        // A server that does not list categories in /joined_rooms must not
        // collapse the sidebar's grouping.
        const QVector<SidebarRoom> sidebar{
            {QStringLiteral("!cat:h"), true, {}},
            {QStringLiteral("!a:h"), false, QStringLiteral("!cat:h")},
        };
        QVERIFY(roomsToDropAfterReconcile(sidebar, set({"!a:h"}), {}, {}).isEmpty());
    }

    void categoryNotListedGoesOnceItsLastChannelGoes()
    {
        const QVector<SidebarRoom> sidebar{
            {QStringLiteral("!cat:h"), true, {}},
            {QStringLiteral("!a:h"), false, QStringLiteral("!cat:h")},
            {QStringLiteral("!b:h"), false, {}},
            {QStringLiteral("!cat2:h"), true, {}},
            {QStringLiteral("!c:h"), false, QStringLiteral("!cat2:h")},
        };
        // !a gone -> !cat empty and unlisted -> gone. !cat2 keeps !c.
        const auto drop = roomsToDropAfterReconcile(sidebar, set({"!b:h", "!c:h"}), {}, {});
        QCOMPARE(asSet(drop), set({"!a:h", "!cat:h"}));
    }

    void listedCategoryIsKeptEvenWhenEmpty()
    {
        const QVector<SidebarRoom> sidebar{
            {QStringLiteral("!cat:h"), true, {}},
            {QStringLiteral("!a:h"), false, QStringLiteral("!cat:h")},
        };
        QCOMPARE(roomsToDropAfterReconcile(sidebar, set({"!cat:h"}), {}, {}),
                 QStringList{QStringLiteral("!a:h")});
    }

    void emptySidebarDropsNothing()
    {
        QVERIFY(roomsToDropAfterReconcile({}, set({"!a:h"}), {}, {}).isEmpty());
    }

    // --- /messages errors ------------------------------------------------

    void onlyForbiddenAndNotFoundTriggerAReconcile()
    {
        using bsfchat::client::messagesErrorSuggestsRoomGone;
        QVERIFY(messagesErrorSuggestsRoomGone(
            QStringLiteral(R"({"errcode":"M_FORBIDDEN","error":"no"})")));
        QVERIFY(messagesErrorSuggestsRoomGone(
            QStringLiteral(R"({"errcode":"M_NOT_FOUND","error":"no"})")));
        QVERIFY(!messagesErrorSuggestsRoomGone(
            QStringLiteral(R"({"errcode":"M_LIMIT_EXCEEDED"})")));
        QVERIFY(!messagesErrorSuggestsRoomGone(QStringLiteral("<html>502</html>")));
        QVERIFY(!messagesErrorSuggestsRoomGone(QString()));
    }

    // --- LocalCache::forgetRoom ------------------------------------------

    void forgetRoomRemovesOnlyThatRoom()
    {
        LocalCache cache;
        QVERIFY(cache.open(kMe, QStringLiteral("https://h")));

        bsfchat::SyncResponse resp;
        resp.next_batch = "s7";
        resp.rooms.join["!keep:h"].state.events.push_back(
            stateEvent("m.room.name", {{"name", "keep"}}));
        resp.rooms.join["!gone:h"].state.events.push_back(
            stateEvent("m.room.name", {{"name", "gone"}}));
        cache.recordSync(resp);
        bsfchat::RoomEvent msg;
        msg.event_id = "$1";
        msg.type = "m.room.message";
        msg.content.data = nlohmann::json{{"body", "hi"}};
        cache.recordTimeline(QStringLiteral("!gone:h"), {msg});
        cache.recordTimeline(QStringLiteral("!keep:h"), {msg});
        QCOMPARE(cache.cachedRoomIds().size(), 2);

        cache.forgetRoom(QStringLiteral("!gone:h"));

        QCOMPARE(cache.cachedRoomIds(), QStringList{QStringLiteral("!keep:h")});
        bsfchat::SyncResponse hydrated;
        QVERIFY(cache.buildHydrationSync(hydrated));
        QCOMPARE(hydrated.rooms.join.size(), std::size_t(1));
        QVERIFY(hydrated.rooms.join.count("!keep:h"));
        const auto timelines = cache.timelines();
        QVERIFY(!timelines.contains(QStringLiteral("!gone:h")));
        QVERIFY(timelines.contains(QStringLiteral("!keep:h")));
        // The resume token is untouched: forgetting a room is not a reset.
        QCOMPARE(cache.syncToken(), QStringLiteral("s7"));

        // Idempotent, and safe for unknown / empty ids and a closed cache.
        cache.forgetRoom(QStringLiteral("!gone:h"));
        cache.forgetRoom(QString());
        cache.close();
        cache.forgetRoom(QStringLiteral("!keep:h"));
    }
};

QTEST_GUILESS_MAIN(TestRoomRemoval)
#include "test_room_removal.moc"
