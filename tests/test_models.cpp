#include <QTest>
#include <QAbstractListModel>

#include "model/MessageModel.h"
#include "model/RoomListModel.h"
#include "model/MemberListModel.h"
#include "model/ServerListModel.h"
#include "util/MarkdownParser.h"

#include <bsfchat/MatrixTypes.h>
#include <bsfchat/Constants.h>

class TestModels : public QObject {
    Q_OBJECT

private:
    bsfchat::RoomEvent makeMessageEvent(const std::string& eventId,
                                          const std::string& sender,
                                          const std::string& body,
                                          int64_t timestamp = 1000)
    {
        bsfchat::RoomEvent event;
        event.event_id = eventId;
        event.sender = sender;
        event.type = std::string(bsfchat::event_type::kRoomMessage);
        event.origin_server_ts = timestamp;
        event.content.data = {
            {"msgtype", "m.text"},
            {"body", body}
        };
        return event;
    }

    bsfchat::RoomEvent makeMemberEvent(const std::string& userId,
                                         const std::string& displayName,
                                         const std::string& membership)
    {
        bsfchat::RoomEvent event;
        event.event_id = "$member_" + userId;
        event.sender = userId;
        event.type = std::string(bsfchat::event_type::kRoomMember);
        event.state_key = userId;
        event.origin_server_ts = 1000;
        event.content.data = {
            {"membership", membership},
            {"displayname", displayName}
        };
        return event;
    }

private slots:
    // MessageModel tests
    void testMessageModelAppend()
    {
        MessageModel model;
        auto event = makeMessageEvent("$ev1", "@alice:server", "Hello world", 1000);
        model.appendEvent(event, "@bob:server");

        QCOMPARE(model.rowCount(), 1);

        auto idx = model.index(0);
        QCOMPARE(model.data(idx, MessageModel::EventIdRole).toString(), "$ev1");
        QCOMPARE(model.data(idx, MessageModel::SenderRole).toString(), "@alice:server");
        QCOMPARE(model.data(idx, MessageModel::BodyRole).toString(), "Hello world");
        QCOMPARE(model.data(idx, MessageModel::IsOwnMessageRole).toBool(), false);
    }

    void testMessageModelOwnMessage()
    {
        MessageModel model;
        auto event = makeMessageEvent("$ev1", "@alice:server", "My message");
        model.appendEvent(event, "@alice:server");

        auto idx = model.index(0);
        QCOMPARE(model.data(idx, MessageModel::IsOwnMessageRole).toBool(), true);
    }

    void testMessageModelDuplicateRejection()
    {
        MessageModel model;
        auto event = makeMessageEvent("$ev1", "@alice:server", "Hello");
        model.appendEvent(event, "@bob:server");
        model.appendEvent(event, "@bob:server"); // duplicate

        QCOMPARE(model.rowCount(), 1);
    }

    void testMessageModelClear()
    {
        MessageModel model;
        model.appendEvent(makeMessageEvent("$ev1", "@alice:server", "Hello"), "@bob:server");
        model.appendEvent(makeMessageEvent("$ev2", "@bob:server", "World"), "@bob:server");
        QCOMPARE(model.rowCount(), 2);

        model.clear();
        QCOMPARE(model.rowCount(), 0);
    }

    void testMessageModelShowSender()
    {
        MessageModel model;
        model.appendEvent(makeMessageEvent("$ev1", "@alice:server", "Hello", 1000), "@bob:server");
        model.appendEvent(makeMessageEvent("$ev2", "@alice:server", "World", 2000), "@bob:server");
        model.appendEvent(makeMessageEvent("$ev3", "@bob:server", "Hey", 3000), "@bob:server");

        // First message always shows sender
        QCOMPARE(model.data(model.index(0), MessageModel::ShowSenderRole).toBool(), true);
        // Second from same sender: don't show
        QCOMPARE(model.data(model.index(1), MessageModel::ShowSenderRole).toBool(), false);
        // Different sender: show
        QCOMPARE(model.data(model.index(2), MessageModel::ShowSenderRole).toBool(), true);
    }

    void testMessageModelIgnoresNonMessageEvents()
    {
        MessageModel model;
        bsfchat::RoomEvent event;
        event.event_id = "$state1";
        event.sender = "@alice:server";
        event.type = std::string(bsfchat::event_type::kRoomName);
        event.content.data = {{"name", "General"}};
        model.appendEvent(event, "@bob:server");

        QCOMPARE(model.rowCount(), 0);
    }

    // RoomListModel tests
    void testRoomListEnsureRoom()
    {
        RoomListModel model;
        model.ensureRoom("!room1:server");
        QCOMPARE(model.rowCount(), 1);

        // Ensure is idempotent
        model.ensureRoom("!room1:server");
        QCOMPARE(model.rowCount(), 1);
    }

    void testRoomListUpdateName()
    {
        RoomListModel model;
        model.updateRoomName("!room1:server", "General");
        QCOMPARE(model.rowCount(), 1);

        auto idx = model.index(0);
        QCOMPARE(model.data(idx, RoomListModel::DisplayNameRole).toString(), "General");
        QCOMPARE(model.data(idx, RoomListModel::RoomIdRole).toString(), "!room1:server");
    }

    void testRoomListUpdateTopic()
    {
        RoomListModel model;
        model.updateRoomTopic("!room1:server", "Welcome to general");
        auto idx = model.index(0);
        QCOMPARE(model.data(idx, RoomListModel::TopicRole).toString(), "Welcome to general");
    }

    void testRoomListLastMessage()
    {
        RoomListModel model;
        model.updateLastMessage("!room1:server", "Hello!", 12345);
        auto idx = model.index(0);
        QCOMPARE(model.data(idx, RoomListModel::LastMessageRole).toString(), "Hello!");
        QCOMPARE(model.data(idx, RoomListModel::LastMessageTimeRole).toLongLong(), 12345);
    }

    void testRoomDisplayName()
    {
        RoomListModel model;
        model.ensureRoom("!room1:server");
        // Without name set, should return roomId
        QCOMPARE(model.roomDisplayName("!room1:server"), "!room1:server");

        model.updateRoomName("!room1:server", "General");
        QCOMPARE(model.roomDisplayName("!room1:server"), "General");
    }

    void testRoomListClear()
    {
        RoomListModel model;
        model.ensureRoom("!room1:server");
        model.ensureRoom("!room2:server");
        QCOMPARE(model.rowCount(), 2);

        model.clear();
        QCOMPARE(model.rowCount(), 0);
    }

    // ServerListModel tests
    void testServerListAdd()
    {
        ServerListModel model;
        model.addServer("My Server", "http://localhost:8008");
        QCOMPARE(model.rowCount(), 1);

        auto idx = model.index(0);
        QCOMPARE(model.data(idx, ServerListModel::DisplayNameRole).toString(), "My Server");
        QCOMPARE(model.data(idx, ServerListModel::ServerUrlRole).toString(), "http://localhost:8008");
    }

    void testServerListRemove()
    {
        ServerListModel model;
        model.addServer("Server 1", "http://s1");
        model.addServer("Server 2", "http://s2");
        QCOMPARE(model.rowCount(), 2);

        model.removeServer(0);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0), ServerListModel::DisplayNameRole).toString(), "Server 2");
    }

    // MemberListModel tests
    void testMemberListJoin()
    {
        MemberListModel model;
        auto event = makeMemberEvent("@alice:server", "Alice", "join");
        model.processEvent(event);
        QCOMPARE(model.rowCount(), 1);

        auto idx = model.index(0);
        QCOMPARE(model.data(idx, MemberListModel::UserIdRole).toString(), "@alice:server");
        QCOMPARE(model.data(idx, MemberListModel::DisplayNameRole).toString(), "Alice");
    }

    void testMemberListLeave()
    {
        MemberListModel model;
        model.processEvent(makeMemberEvent("@alice:server", "Alice", "join"));
        QCOMPARE(model.rowCount(), 1);

        model.processEvent(makeMemberEvent("@alice:server", "Alice", "leave"));
        QCOMPARE(model.rowCount(), 0);
    }

    // MarkdownParser tests
    void testMarkdownBold()
    {
        QString result = MarkdownParser::toHtml("This is **bold** text");
        QVERIFY(result.contains("<b>bold</b>"));
    }

    void testMarkdownItalic()
    {
        QString result = MarkdownParser::toHtml("This is *italic* text");
        QVERIFY(result.contains("<i>italic</i>"));
    }

    void testMarkdownInlineCode()
    {
        QString result = MarkdownParser::toHtml("Use `code` here");
        QVERIFY(result.contains("<code"));
        QVERIFY(result.contains("code"));
    }

    void testMarkdownLinks()
    {
        QString result = MarkdownParser::toHtml("Visit [Google](https://google.com)");
        QVERIFY(result.contains("<a href"));
        QVERIFY(result.contains("Google"));
        QVERIFY(result.contains("https://google.com"));
    }

    void testMarkdownPlainText()
    {
        QString result = MarkdownParser::toHtml("Just plain text");
        QVERIFY(result.contains("Just plain text"));
        QVERIFY(!result.contains("<b>"));
        QVERIFY(!result.contains("<i>"));
    }

    void testMarkdownHtmlEscaping()
    {
        QString result = MarkdownParser::toHtml("<script>alert('xss')</script>");
        QVERIFY(!result.contains("<script>"));
        QVERIFY(result.contains("&lt;script&gt;"));
    }
};

QTEST_MAIN(TestModels)
#include "test_models.moc"
