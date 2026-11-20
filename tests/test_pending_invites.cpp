// What the sidebar shows for an invite (audit 3, #6), read from the stripped
// invite_state of /sync's rooms.invite. Pure, header-only.

#include "net/PendingInvites.h"

#include <QTest>

using bsfchat::client::newlyInvited;
using bsfchat::client::PendingInvite;
using bsfchat::client::pendingInviteFrom;

namespace {

bsfchat::RoomEvent stateEvent(const char* type, const char* stateKey, const char* sender,
                              nlohmann::json content)
{
    bsfchat::RoomEvent ev;
    ev.type = type;
    ev.state_key = std::string(stateKey);
    ev.sender = sender;
    ev.content.data = std::move(content);
    return ev;
}

} // namespace

class TestPendingInvites : public QObject
{
    Q_OBJECT
private slots:
    void readsRoomNameInviterAndDirectFlag()
    {
        bsfchat::InvitedRoom room;
        room.invite_state.events.push_back(
            stateEvent("m.room.name", "", "@alice:h", {{"name", "private"}}));
        room.invite_state.events.push_back(
            stateEvent("bsfchat.room.type", "", "@alice:h", {{"type", "text"}}));
        room.invite_state.events.push_back(
            stateEvent("m.room.member", "@bob:h", "@alice:h",
                       {{"membership", "invite"}, {"is_direct", false}}));
        room.invite_state.events.push_back(
            stateEvent("m.room.member", "@alice:h", "@alice:h",
                       {{"membership", "join"}, {"displayname", "Alice"}}));

        const PendingInvite inv = pendingInviteFrom(QStringLiteral("!r:h"), room,
                                                    QStringLiteral("@bob:h"));
        QCOMPARE(inv.roomId, QStringLiteral("!r:h"));
        QCOMPARE(inv.roomName, QStringLiteral("private"));
        QCOMPARE(inv.roomType, QStringLiteral("text"));
        QCOMPARE(inv.inviterId, QStringLiteral("@alice:h"));
        QCOMPARE(inv.inviterName, QStringLiteral("Alice"));
        QVERIFY(!inv.isDirect);
    }

    void aDirectInviteIsFlagged()
    {
        bsfchat::InvitedRoom room;
        room.invite_state.events.push_back(
            stateEvent("m.room.member", "@bob:h", "@alice:h",
                       {{"membership", "invite"}, {"is_direct", true}}));
        const PendingInvite inv = pendingInviteFrom(QStringLiteral("!dm:h"), room,
                                                    QStringLiteral("@bob:h"));
        QVERIFY(inv.isDirect);
        QCOMPARE(inv.inviterId, QStringLiteral("@alice:h"));
        QVERIFY(inv.inviterName.isEmpty());
    }

    void malformedContentIsSkippedNotFatal()
    {
        bsfchat::InvitedRoom room;
        room.invite_state.events.push_back(
            stateEvent("m.room.name", "", "@alice:h", nlohmann::json::array()));
        room.invite_state.events.push_back(
            stateEvent("m.room.name", "", "@alice:h", {{"name", 5}}));
        const PendingInvite inv = pendingInviteFrom(QStringLiteral("!r:h"), room,
                                                    QStringLiteral("@bob:h"));
        QVERIFY(inv.roomName.isEmpty());
        QVERIFY(inv.inviterId.isEmpty());
    }

    void onlyUnannouncedInvitesAreNew()
    {
        PendingInvite a; a.roomId = QStringLiteral("!a:h");
        PendingInvite b; b.roomId = QStringLiteral("!b:h");
        const QStringList fresh = newlyInvited({QStringLiteral("!a:h")}, {a, b});
        QCOMPARE(fresh, QStringList{QStringLiteral("!b:h")});
        QVERIFY(newlyInvited({QStringLiteral("!a:h"), QStringLiteral("!b:h")}, {a, b}).isEmpty());
    }
};

QTEST_MAIN(TestPendingInvites)
#include "test_pending_invites.moc"
