// "Message this user" must never produce a second DM room with someone.
//
// It used to, two ways:
//
//   1. ServerConnection::createDirectMessage POSTed /createRoom unconditionally.
//      Looking for an existing DM was left to each QML entry point; three of
//      five skipped it, and the two that scanned directRooms() still raced,
//      because that list only grew when a create SUCCEEDED — a second click
//      during a slow /createRoom saw nothing and made a twin.
//   2. A DM the other person opened was never recognised: nothing read the
//      server's m.direct, so the room sat in the channel tree and every entry
//      point above would open a second room with the same peer.
//
// net/DirectRooms.h holds the bookkeeping that closes both. `Opener` below is
// createDirectMessage's control flow over a fake client, so the scenarios run
// without a network, an event loop or a QSettings file.

#include "net/DirectRooms.h"
#include "net/TokenedReply.h"

#include <QTest>

using bsfchat::net::DirectRooms;

class FakeClient : public QObject
{
    Q_OBJECT
public:
    QStringList posted; // request tokens, in order
    void createDirectMessageRoom(const QString& token, const QString&) { posted.append(token); }
signals:
    void createRoomSuccess(const QString& token, const QString& roomId);
    void createRoomError(const QString& token, const QString& why);
};

// Mirrors ServerConnection::createDirectMessage step for step.
class Opener : public QObject
{
    Q_OBJECT
public:
    FakeClient client;
    DirectRooms rooms;
    QSet<QString> joined;       // stands in for RoomListModel::hasRoom
    bool firstSyncDone = true;
    QStringList activated;      // setActiveRoom calls
    int errors = 0;

    void open(const QString& peer)
    {
        const QString existing = rooms.roomWith(peer, [this](const QString& rid) {
            return !firstSyncDone || joined.contains(rid);
        });
        if (!existing.isEmpty()) { activated.append(existing); return; }
        if (!rooms.beginCreate(peer)) return;

        const QString token = bsfchat::net::newRequestToken();
        bsfchat::net::awaitTokenedReply(
            &client, this, token,
            &FakeClient::createRoomSuccess, &FakeClient::createRoomError,
            [this, peer](const QString& roomId) {
                rooms.endCreate(peer);
                rooms.record(roomId, peer);
                joined.insert(roomId);
                activated.append(roomId);
            },
            [this, peer](const QString&) { rooms.endCreate(peer); ++errors; });
        client.createDirectMessageRoom(token, peer);
    }
};

class TestDirectRooms : public QObject
{
    Q_OBJECT

private slots:
    // Cause 1, the three entry points that never looked.
    void existingDmIsOpenedNotRecreated()
    {
        Opener o;
        o.rooms.record("!dm:x", "@bob:x");
        o.joined.insert("!dm:x");

        o.open("@bob:x");
        QCOMPARE(o.client.posted.size(), 0);
        QCOMPARE(o.activated, QStringList{"!dm:x"});
    }

    // Cause 1, the race the other two entry points left open: the map only
    // learns about a room when the reply lands.
    void secondClickDuringSlowCreateDoesNotPostAgain()
    {
        Opener o;
        o.open("@bob:x");
        o.open("@bob:x");
        o.open("@bob:x");
        QCOMPARE(o.client.posted.size(), 1);

        emit o.client.createRoomSuccess(o.client.posted[0], "!dm:x");
        QCOMPARE(o.activated, QStringList{"!dm:x"});

        // And afterwards it is an existing DM like any other.
        o.open("@bob:x");
        QCOMPARE(o.client.posted.size(), 1);
        QCOMPARE(o.activated.size(), 2);
    }

    // The guard is per peer — it must not serialise unrelated DMs.
    void differentPeersCreateIndependently()
    {
        Opener o;
        o.open("@bob:x");
        o.open("@carol:x");
        QCOMPARE(o.client.posted.size(), 2);

        // Replies in the opposite order still land on the right peer.
        emit o.client.createRoomSuccess(o.client.posted[1], "!c:x");
        emit o.client.createRoomSuccess(o.client.posted[0], "!b:x");
        QCOMPARE(o.rooms.peerOf("!c:x"), QString("@carol:x"));
        QCOMPARE(o.rooms.peerOf("!b:x"), QString("@bob:x"));
    }

    // A guard that outlives a failure locks that peer out until restart.
    void failedCreateReleasesTheGuard()
    {
        Opener o;
        o.open("@bob:x");
        emit o.client.createRoomError(o.client.posted[0], "M_LIMIT_EXCEEDED");
        QCOMPARE(o.errors, 1);
        QVERIFY(!o.rooms.isCreating("@bob:x"));

        o.open("@bob:x");
        QCOMPARE(o.client.posted.size(), 2);
    }

    // Cause 2: the peer opened the DM. m.direct in /sync is the only signal.
    void dmOpenedByThePeerIsRecognisedFromMDirect()
    {
        Opener o;
        o.joined.insert("!theirs:x");
        QVERIFY(!o.rooms.contains("!theirs:x"));

        const auto changed = o.rooms.merge({{"@bob:x", {"!theirs:x"}}}, "@me:x");
        QCOMPARE(changed, QStringList{"!theirs:x"});
        QCOMPARE(o.rooms.peerOf("!theirs:x"), QString("@bob:x"));

        o.open("@bob:x");
        QCOMPARE(o.client.posted.size(), 0);
        QCOMPARE(o.activated, QStringList{"!theirs:x"});
    }

    // m.direct is restated on every initial sync; an unchanged map must not
    // look like news, or every launch rewrites settings and rebuilds the list.
    void mergeReportsOnlyWhatChanged()
    {
        DirectRooms r;
        r.record("!a:x", "@bob:x");
        const auto changed = r.merge({{"@bob:x", {"!a:x", "!b:x"}}}, "@me:x");
        QCOMPARE(changed, QStringList{"!b:x"});
        QVERIFY(r.merge({{"@bob:x", {"!a:x", "!b:x"}}}, "@me:x").isEmpty());
    }

    // Additive: an older server sends no m.direct, and ours omits rooms only
    // because it omits the whole event. Absence is not evidence.
    void mergeNeverForgetsLocallyKnownDms()
    {
        DirectRooms r;
        r.record("!local:x", "@carol:x");
        r.merge({{"@bob:x", {"!a:x"}}}, "@me:x");
        QVERIFY(r.contains("!local:x"));
        r.merge({}, "@me:x");
        QVERIFY(r.contains("!local:x"));
        QVERIFY(r.contains("!a:x"));
    }

    void mergeSkipsSelfAndEmptyIds()
    {
        DirectRooms r;
        QVERIFY(r.merge({{"@me:x", {"!self:x"}}, {"@bob:x", {""}}, {"", {"!r:x"}}}, "@me:x").isEmpty());
        QVERIFY(r.peers().isEmpty());
    }

    // After a real sync, an entry for a room we are no longer in must not be
    // jumped to — that opens an empty pane. It falls through to a create (which
    // the server answers with the live room if there is one).
    void staleEntryIsNotJumpedToOnceSyncHasSpoken()
    {
        Opener o;
        o.rooms.record("!gone:x", "@bob:x"); // not in o.joined
        o.open("@bob:x");
        QCOMPARE(o.client.posted.size(), 1);
        QVERIFY(o.activated.isEmpty());
    }

    // Before the first sync the room list is empty for everyone, so it proves
    // nothing; the persisted entry is the best information there is.
    void storedEntryIsTrustedBeforeTheFirstSync()
    {
        Opener o;
        o.firstSyncDone = false;
        o.rooms.record("!dm:x", "@bob:x");
        o.open("@bob:x");
        QCOMPARE(o.client.posted.size(), 0);
        QCOMPARE(o.activated, QStringList{"!dm:x"});
    }

    // Databases from before this fix can hold several rooms per peer. Every
    // entry point must agree on which one "the DM" is.
    void legacyDuplicatesResolveToOneStableRoom()
    {
        DirectRooms r;
        r.record("!z:x", "@bob:x");
        r.record("!a:x", "@bob:x");
        r.record("!m:x", "@bob:x");
        auto any = [](const QString&) { return true; };
        QCOMPARE(r.roomWith("@bob:x", any), QString("!a:x"));
        QCOMPARE(r.roomWith("@bob:x", [](const QString& rid) { return rid != "!a:x"; }),
                 QString("!m:x"));
        QVERIFY(r.roomWith("@nobody:x", any).isEmpty());
    }
};

QTEST_MAIN(TestDirectRooms)
#include "test_direct_rooms.moc"
