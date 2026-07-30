#include <QTest>
#include <QAbstractListModel>
#include <QMap>
#include <QRegularExpression>
#include <QSignalSpy>

#include "model/MessageModel.h"
#include "model/RoomListModel.h"
#include "model/MemberListModel.h"
#include "model/ServerListModel.h"
#include "util/MarkdownParser.h"
#include "util/MediaUrl.h"
#include "util/MentionBadge.h"
#include "util/MentionRenderer.h"
#include "util/PermissionMath.h"
#include "util/SearchParser.h"

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

    // A member event as the SERVER writes one once a nickname is set: `displayname`
    // already holds the EFFECTIVE name (the nickname), and `bsfchat.nickname`
    // carries the nickname separately so the UI can tell why the name is what it
    // is. Passing an empty nickname omits the key entirely, which is how "no
    // nickname" is represented on the wire.
    bsfchat::RoomEvent makeMemberEventWithNickname(const std::string& userId,
                                                   const std::string& effectiveName,
                                                   const std::string& nickname,
                                                   const std::string& membership = "join")
    {
        bsfchat::RoomEvent event;
        event.event_id = "$member_" + userId;
        event.sender = userId;
        event.type = std::string(bsfchat::event_type::kRoomMember);
        event.state_key = userId;
        event.origin_server_ts = 1000;
        event.content.data = {
            {"membership", membership},
            {"displayname", effectiveName}
        };
        if (!nickname.empty()) event.content.data["bsfchat.nickname"] = nickname;
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

    bsfchat::RoomEvent makeEditEvent(const std::string& eventId,
                                       const std::string& sender,
                                       const std::string& targetId,
                                       const std::string& newBody)
    {
        auto event = makeMessageEvent(eventId, sender, "* " + newBody, 5000);
        event.content.data["m.relates_to"] = {
            {"rel_type", "m.replace"}, {"event_id", targetId}
        };
        event.content.data["m.new_content"] = {
            {"msgtype", "m.text"}, {"body", newBody}
        };
        return event;
    }

    bsfchat::RoomEvent makeReactionEvent(const std::string& eventId,
                                          const std::string& sender,
                                          const std::string& targetId,
                                          const std::string& key)
    {
        bsfchat::RoomEvent event;
        event.event_id = eventId;
        event.sender = sender;
        event.type = "m.reaction";
        event.origin_server_ts = 6000;
        event.content.data = {
            {"m.relates_to", {{"rel_type", "m.annotation"},
                              {"event_id", targetId},
                              {"key", key}}}
        };
        return event;
    }

    bsfchat::RoomEvent makeThreadReply(const std::string& eventId,
                                         const std::string& sender,
                                         const std::string& rootId,
                                         const std::string& body,
                                         int64_t timestamp = 7000)
    {
        auto event = makeMessageEvent(eventId, sender, body, timestamp);
        event.content.data["m.relates_to"] = {
            {"rel_type", "m.thread"}, {"event_id", rootId}
        };
        return event;
    }

    // A timeline event as the SERVER now hands it over once the message has
    // been edited: `content` is already the post-edit text, the edit that
    // produced it is named in unsigned.m.relations.m.replace, and the pre-edit
    // content sits under unsigned.bsfchat.original_content. See
    // server/src/store/SqliteStore.cpp's timeline row loader.
    bsfchat::RoomEvent makeReconciledEvent(const std::string& eventId,
                                            const std::string& sender,
                                            const std::string& currentBody,
                                            const std::string& originalBody,
                                            const std::string& replaceEventId,
                                            int64_t originalTs = 1000,
                                            int64_t editTs = 5000)
    {
        auto event = makeMessageEvent(eventId, sender, currentBody, originalTs);
        nlohmann::json unsignedData;
        unsignedData["m.relations"]["m.replace"] = {
            {"event_id", replaceEventId},
            {"sender", sender},
            {"origin_server_ts", editTs}
        };
        unsignedData["bsfchat.original_content"] = {
            {"msgtype", "m.text"}, {"body", originalBody}
        };
        event.unsigned_data = bsfchat::EventContent{unsignedData};
        return event;
    }

    bsfchat::RoomEvent makeMentionEvent(const std::string& eventId,
                                         const std::string& sender,
                                         const std::string& body,
                                         const std::vector<std::string>& userIds,
                                         bool roomMention = false,
                                         int64_t timestamp = 1000)
    {
        auto event = makeMessageEvent(eventId, sender, body, timestamp);
        nlohmann::json mentions = nlohmann::json::object();
        if (!userIds.empty()) mentions["user_ids"] = userIds;
        if (roomMention) mentions["room"] = true;
        event.content.data["m.mentions"] = mentions;
        return event;
    }

    // Number of rows covered by every dataChanged emission in `spy`.
    static int rowsTouched(const QSignalSpy& spy)
    {
        int n = 0;
        for (const auto& call : spy) {
            const auto topLeft = call.at(0).value<QModelIndex>();
            const auto bottomRight = call.at(1).value<QModelIndex>();
            n += bottomRight.row() - topLeft.row() + 1;
        }
        return n;
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

    // ── per-server nicknames ────────────────────────────────────────────
    //
    // The rendered name is DisplayNameRole and nothing else: the server resolves
    // nickname-over-global-name and writes the winner into the member event's
    // `displayname`, so no client code has to choose. NicknameRole exists only so
    // admin UI can distinguish "this IS a nickname" from "this is their real name",
    // which is what makes a "Remove nickname" affordance possible.
    void testMemberListNicknamePreferredOverGlobalName()
    {
        MemberListModel model;
        QMap<QString, QString> cache;
        // The global display name is in the cache, as a profile fetch would leave it.
        cache["@alice:server"] = "Alice Anderson";
        model.setDisplayNameCache(&cache);

        model.processEvent(makeMemberEventWithNickname("@alice:server", "Ali", "Ali"));

        auto idx = model.index(0);
        // The nickname wins over the cached global name — and it wins because the
        // event carries it, not because the client re-ranked anything.
        QCOMPARE(model.data(idx, MemberListModel::DisplayNameRole).toString(), "Ali");
        QCOMPARE(model.data(idx, MemberListModel::NicknameRole).toString(), "Ali");
    }

    void testMemberListNicknameAbsentWhenNoneSet()
    {
        MemberListModel model;
        model.processEvent(makeMemberEvent("@bob:server", "Bob", "join"));

        auto idx = model.index(0);
        QCOMPARE(model.data(idx, MemberListModel::DisplayNameRole).toString(), "Bob");
        // Empty, not "Bob": a UI must be able to tell that Bob has NO nickname, or
        // it would offer to remove one that does not exist.
        QCOMPARE(model.data(idx, MemberListModel::NicknameRole).toString(), QString());
    }

    void testMemberListNicknameIsClearedByAnEventWithoutIt()
    {
        MemberListModel model;
        model.processEvent(makeMemberEventWithNickname("@alice:server", "Ali", "Ali"));
        QCOMPARE(model.data(model.index(0), MemberListModel::NicknameRole).toString(), "Ali");

        // Clearing a nickname re-emits the member event with the key ABSENT and the
        // global name restored. A stale nickname surviving that would leave the UI
        // offering "Remove nickname" forever.
        model.processEvent(makeMemberEvent("@alice:server", "Alice Anderson", "join"));
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0), MemberListModel::DisplayNameRole).toString(),
                 "Alice Anderson");
        QCOMPARE(model.data(model.index(0), MemberListModel::NicknameRole).toString(), QString());
    }

    void testMemberListNicknameLookupByUserId()
    {
        MemberListModel model;
        model.processEvent(makeMemberEventWithNickname("@alice:server", "Ali", "Ali"));
        model.processEvent(makeMemberEvent("@bob:server", "Bob", "join"));

        QCOMPARE(model.nicknameForUser("@alice:server"), "Ali");
        QCOMPARE(model.nicknameForUser("@bob:server"), QString());
        QCOMPARE(model.nicknameForUser("@nobody:server"), QString());
        // displayNameForUser is Q_INVOKABLE so QML can actually call it; it was a
        // plain member function while MessageBubble.qml already called it.
        QCOMPARE(model.displayNameForUser("@alice:server"), "Ali");
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

    // --- media download URLs -------------------------------------------
    // The server can require auth on downloads ([media] require_auth, on by
    // default). QML Image.source can't send an Authorization header, so the
    // token has to be in the query string or every image 401s.

    void testMediaUrlCarriesAccessToken()
    {
        const QString url = bsfchat::client::buildMediaDownloadUrl(
            "https://bsfchat.example", "tok_abc123", "mxc://bsfchat.example/AbCdEf");
        QCOMPARE(url, QStringLiteral("https://bsfchat.example")
                      + QString::fromUtf8(bsfchat::api_path::kMediaDownload)
                      + QStringLiteral("bsfchat.example/AbCdEf?access_token=tok_abc123"));
    }

    void testMediaUrlPercentEncodesToken()
    {
        // Tokens are server-generated, but nothing guarantees they stay
        // query-safe; an unescaped '&' or '+' would truncate or corrupt the
        // credential and read as a 401 rather than a client bug.
        const QString url = bsfchat::client::buildMediaDownloadUrl(
            "https://h", "a+b&c=d/e", "mxc://h/id");
        QVERIFY(url.endsWith(QStringLiteral("?access_token=a%2Bb%26c%3Dd%2Fe")));
        QCOMPARE(url.count(QLatin1Char('&')), 0);
    }

    void testMediaUrlRejectsNonMxcAndEmptyHomeserver()
    {
        QVERIFY(bsfchat::client::buildMediaDownloadUrl("https://h", "t", "http://evil/x").isEmpty());
        QVERIFY(bsfchat::client::buildMediaDownloadUrl("", "t", "mxc://h/id").isEmpty());
    }

    void testMediaUrlWithoutTokenHasNoQuery()
    {
        // Pre-login / no-credential case must stay a bare URL rather than
        // emitting "?access_token=".
        const QString url = bsfchat::client::buildMediaDownloadUrl("https://h", "", "mxc://h/id");
        QVERIFY(!url.isEmpty());
        QVERIFY(!url.contains(QLatin1Char('?')));
    }

    void testMessageModelResolvesMediaWithToken()
    {
        // MessageModel reads the token through a pointer to its owner's copy,
        // so a token set after construction (every login path) still lands in
        // URLs built later.
        MessageModel model;
        model.setHomeserver("https://bsfchat.example");
        QString token;
        model.setAccessTokenSource(&token);

        QVERIFY(!model.resolveMediaUrl("mxc://bsfchat.example/id").contains("access_token"));
        token = "later_token";
        QVERIFY(model.resolveMediaUrl("mxc://bsfchat.example/id")
                    .endsWith("?access_token=later_token"));
    }

    void testMessageModelMediaUrlOnEventMatchesMatrixClient()
    {
        // The inline-image path (baked into the row at insert time) must
        // produce the same shape as the avatar path (built on demand in QML).
        MessageModel model;
        model.setHomeserver("https://bsfchat.example");
        const QString token = QStringLiteral("tok");
        model.setAccessTokenSource(&token);

        bsfchat::RoomEvent event = makeMessageEvent("$img", "@a:h", "pic.png");
        event.content.data["msgtype"] = "m.image";
        event.content.data["url"] = "mxc://bsfchat.example/PicId";
        model.appendEvent(event, "@me:h");

        QCOMPARE(model.rowCount(), 1);
        const QString baked = model.data(model.index(0, 0),
                                         MessageModel::MediaUrlRole).toString();
        QCOMPARE(baked, model.resolveMediaUrl("mxc://bsfchat.example/PicId"));
        QVERIFY(baked.endsWith("?access_token=tok"));
    }

    // --- event-id indexing / targeted repaints ---------------------------
    // MessageModel used to answer "which row is event X?" with a linear scan
    // in six separate places, and refreshDisplayNames() repainted the whole
    // model for a single rename. These tests pin the row-level behaviour so
    // the index can't silently drift out of step with m_messages.

    void testMessageModelIndexForEventId()
    {
        MessageModel model;
        for (int i = 0; i < 5; ++i) {
            model.appendEvent(makeMessageEvent("$ev" + std::to_string(i),
                                               "@alice:server", "m", 1000 + i * 1000),
                              "@bob:server");
        }
        QCOMPARE(model.indexForEventId("$ev0"), 0);
        QCOMPARE(model.indexForEventId("$ev4"), 4);
        QCOMPARE(model.indexForEventId("$nope"), -1);
        QCOMPARE(model.indexForEventId(QString()), -1);
    }

    void testMessageModelPrependRebuildsIndex()
    {
        MessageModel model;
        model.appendEvent(makeMessageEvent("$new1", "@alice:server", "new", 3000),
                          "@bob:server");
        model.appendEvent(makeMessageEvent("$new2", "@alice:server", "newer", 4000),
                          "@bob:server");

        QVector<bsfchat::RoomEvent> older{
            makeMessageEvent("$old1", "@alice:server", "old", 1000),
            makeMessageEvent("$old2", "@alice:server", "older", 2000),
        };
        model.prependEvents(older, "@bob:server");
        QCOMPARE(model.rowCount(), 4);

        // Every existing row shifted by two; the index has to follow, or an
        // edit lands on the wrong message.
        QCOMPARE(model.indexForEventId("$old1"), 0);
        QCOMPARE(model.indexForEventId("$new1"), 2);
        QCOMPARE(model.indexForEventId("$new2"), 3);

        model.appendEvent(makeEditEvent("$edit", "@alice:server", "$new1", "edited"),
                          "@bob:server");
        QCOMPARE(model.data(model.index(2), MessageModel::BodyRole).toString(), "edited");
        QCOMPARE(model.data(model.index(3), MessageModel::BodyRole).toString(), "newer");
    }

    void testMessageModelPrependIgnoresDuplicatesWithinBatch()
    {
        MessageModel model;
        QVector<bsfchat::RoomEvent> page{
            makeMessageEvent("$a", "@alice:server", "one", 1000),
            makeMessageEvent("$a", "@alice:server", "one", 1000),
            makeMessageEvent("$b", "@alice:server", "two", 2000),
        };
        model.prependEvents(page, "@bob:server");
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.indexForEventId("$b"), 1);
    }

    void testMessageModelEditRepaintsOnlyTargetRow()
    {
        MessageModel model;
        for (int i = 0; i < 6; ++i) {
            model.appendEvent(makeMessageEvent("$ev" + std::to_string(i),
                                               "@alice:server", "m", 1000 + i * 1000),
                              "@bob:server");
        }

        QSignalSpy spy(&model, &QAbstractItemModel::dataChanged);
        model.appendEvent(makeEditEvent("$edit", "@alice:server", "$ev2", "fixed"),
                          "@bob:server");

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).value<QModelIndex>().row(), 2);
        QCOMPARE(spy.at(0).at(1).value<QModelIndex>().row(), 2);
        const auto roles = spy.at(0).at(2).value<QList<int>>();
        QVERIFY(roles.contains(MessageModel::BodyRole));
        QCOMPARE(model.data(model.index(2), MessageModel::BodyRole).toString(), "fixed");
        QCOMPARE(model.data(model.index(2), MessageModel::EditedRole).toBool(), true);
    }

    void testMessageModelReactionRepaintsOnlyTargetRow()
    {
        MessageModel model;
        for (int i = 0; i < 4; ++i) {
            model.appendEvent(makeMessageEvent("$ev" + std::to_string(i),
                                               "@alice:server", "m", 1000 + i * 1000),
                              "@bob:server");
        }

        QSignalSpy spy(&model, &QAbstractItemModel::dataChanged);
        model.appendEvent(makeReactionEvent("$r1", "@carol:server", "$ev1", "👍"),
                          "@bob:server");
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).value<QModelIndex>().row(), 1);
        QCOMPARE(spy.at(0).at(1).value<QModelIndex>().row(), 1);
        QCOMPARE(spy.at(0).at(2).value<QList<int>>(),
                 QList<int>{MessageModel::ReactionsRole});

        QCOMPARE(model.ownReactionEventId("$ev1", "👍", "@carol:server"), "$r1");
        QVERIFY(model.ownReactionEventId("$ev1", "👍", "@dave:server").isEmpty());
        QVERIFY(model.ownReactionEventId("$missing", "👍", "@carol:server").isEmpty());

        // Redacting the reaction must land on the same row, not rescan.
        spy.clear();
        bsfchat::RoomEvent redaction;
        redaction.event_id = "$red1";
        redaction.sender = "@carol:server";
        redaction.type = std::string(bsfchat::event_type::kRoomRedaction);
        redaction.origin_server_ts = 9000;
        redaction.content.data = {{"redacts", "$r1"}};
        model.appendEvent(redaction, "@bob:server");

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).value<QModelIndex>().row(), 1);
        QVERIFY(model.ownReactionEventId("$ev1", "👍", "@carol:server").isEmpty());
    }

    void testMessageModelThreadReplyCountIsIndexed()
    {
        MessageModel model;
        model.appendEvent(makeMessageEvent("$root", "@alice:server", "topic", 1000),
                          "@bob:server");
        model.appendEvent(makeMessageEvent("$other", "@alice:server", "unrelated", 1500),
                          "@bob:server");
        QCOMPARE(model.threadReplyCount("$root"), 0);

        QSignalSpy spy(&model, &QAbstractItemModel::dataChanged);
        model.appendEvent(makeThreadReply("$t1", "@carol:server", "$root", "re", 2000),
                          "@bob:server");
        model.appendEvent(makeThreadReply("$t2", "@dave:server", "$root", "re2", 3000),
                          "@bob:server");

        QCOMPARE(model.threadReplyCount("$root"), 2);
        QCOMPARE(model.threadReplyCount("$other"), 0);
        QCOMPARE(model.threadReplyCount(QString()), 0);
        QCOMPARE(model.data(model.index(0), MessageModel::ThreadReplyCountRole).toInt(), 2);
        QCOMPARE(model.threadReplies("$root").size(), 2);

        // Each reply refreshes the root's badge, and nothing else.
        QCOMPARE(spy.count(), 2);
        for (const auto& call : spy) {
            QCOMPARE(call.at(0).value<QModelIndex>().row(), 0);
            QCOMPARE(call.at(1).value<QModelIndex>().row(), 0);
            QCOMPARE(call.at(2).value<QList<int>>(),
                     QList<int>{MessageModel::ThreadReplyCountRole});
        }

        model.clear();
        QCOMPARE(model.threadReplyCount("$root"), 0);
    }

    void testMessageModelDisplayNameRefreshIsBounded()
    {
        MessageModel model;
        QMap<QString, QString> cache;
        model.setDisplayNameCache(&cache);
        // alice, bob, alice, bob, alice, bob
        for (int i = 0; i < 6; ++i) {
            model.appendEvent(makeMessageEvent(
                "$ev" + std::to_string(i),
                (i % 2) ? "@bob:server" : "@alice:server", "m", 1000 + i * 1000),
                "@me:server");
        }
        QCOMPARE(model.data(model.index(1), MessageModel::SenderDisplayNameRole).toString(),
                 "bob");

        // One user renames. The whole-model dataChanged this replaced meant a
        // single rename rebuilt every delegate in the timeline.
        cache["@bob:server"] = "Bobby";
        QSignalSpy spy(&model, &QAbstractItemModel::dataChanged);
        model.refreshDisplayNames();

        QCOMPARE(model.data(model.index(1), MessageModel::SenderDisplayNameRole).toString(),
                 "Bobby");
        QCOMPARE(model.data(model.index(0), MessageModel::SenderDisplayNameRole).toString(),
                 "alice");
        // Three of six rows changed, and none of the emissions may reach
        // beyond them.
        QCOMPARE(rowsTouched(spy), 3);
        for (const auto& call : spy) {
            QCOMPARE(call.at(2).value<QList<int>>(),
                     QList<int>{MessageModel::SenderDisplayNameRole});
            QVERIFY(call.at(0).value<QModelIndex>().row() % 2 == 1);
        }

        // A no-op refresh must stay silent rather than repaint.
        spy.clear();
        model.refreshDisplayNames();
        QCOMPARE(spy.count(), 0);
    }

    void testMessageModelDisplayNameRefreshCollapsesManyRanges()
    {
        // Pathological case: every row belongs to a different user and they
        // all resolve at once (an initial member-state batch). The emission
        // count is capped, but must still be bounded by the changed rows.
        MessageModel model;
        QMap<QString, QString> cache;
        model.setDisplayNameCache(&cache);
        constexpr int kRows = 60;
        for (int i = 0; i < kRows; ++i) {
            const std::string user = "@u" + std::to_string(i) + ":server";
            model.appendEvent(makeMessageEvent("$ev" + std::to_string(i), user,
                                               "m", 1000 + i * 1000),
                              "@me:server");
        }
        // Leave row 0 alone so the collapsed span cannot be the whole model.
        for (int i = 1; i < kRows; ++i) {
            cache[QStringLiteral("@u%1:server").arg(i)] = QStringLiteral("User %1").arg(i);
        }

        QSignalSpy spy(&model, &QAbstractItemModel::dataChanged);
        model.refreshDisplayNames();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).value<QModelIndex>().row(), 1);
        QCOMPARE(spy.at(0).at(1).value<QModelIndex>().row(), kRows - 1);
    }

    // --- server-reconciled edits -----------------------------------------
    // The server applies edits before sending, so a timeline event arrives
    // with the post-edit body plus a bundle naming the edit and carrying the
    // original. The replacement is ALSO still an ordinary timeline event, so
    // the same edit reaches the client twice and the model has to recognise
    // the second copy as old news.

    void testEditHistorySeededFromServerBundle()
    {
        MessageModel model;
        model.appendEvent(makeReconciledEvent("$msg", "@alice:server",
                                              "second draft", "first draft",
                                              "$edit", 1000, 5000),
                          "@bob:server");

        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0), MessageModel::BodyRole).toString(),
                 "second draft");
        // The bundle alone is enough to mark the message edited — no sibling
        // event required, which is what makes this correct on a cold load.
        QCOMPARE(model.data(model.index(0), MessageModel::EditedRole).toBool(), true);

        const auto history = model.editHistory("$msg");
        QCOMPARE(history.size(), 2);
        QCOMPARE(history.at(0).toMap().value("body").toString(), "first draft");
        QCOMPARE(history.at(0).toMap().value("timestamp").toLongLong(), 1000);
        QCOMPARE(history.at(1).toMap().value("body").toString(), "second draft");
        // editedAt comes from the bundle, not from the original's ts.
        QCOMPARE(history.at(1).toMap().value("timestamp").toLongLong(), 5000);
        QCOMPARE(history.at(1).toMap().value("isCurrent").toBool(), true);
    }

    void testReconciledEditSiblingDoesNotDuplicateHistory()
    {
        // The regression: the sibling used to push the ALREADY-current body
        // into history and re-set it as the body, so "Show edit history"
        // listed "second draft" twice and never showed "first draft".
        MessageModel model;
        model.appendEvent(makeReconciledEvent("$msg", "@alice:server",
                                              "second draft", "first draft",
                                              "$edit"),
                          "@bob:server");
        QSignalSpy spy(&model, &QAbstractItemModel::dataChanged);
        model.appendEvent(makeEditEvent("$edit", "@alice:server", "$msg",
                                        "second draft"),
                          "@bob:server");

        // Not a row of its own, and not a repaint either — nothing changed.
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(spy.count(), 0);

        const auto history = model.editHistory("$msg");
        QCOMPARE(history.size(), 2);
        QCOMPARE(history.at(0).toMap().value("body").toString(), "first draft");
        QCOMPARE(history.at(1).toMap().value("body").toString(), "second draft");
    }

    void testLiveEditStillAppliesAfterReconciledLoad()
    {
        // Skipping the bundled edit must not turn into skipping *all* edits:
        // a later edit arriving over /sync while the user watches the room has
        // a different event id and still has to land.
        MessageModel model;
        model.appendEvent(makeReconciledEvent("$msg", "@alice:server",
                                              "v2", "v1", "$edit1"),
                          "@bob:server");
        model.appendEvent(makeEditEvent("$edit1", "@alice:server", "$msg", "v2"),
                          "@bob:server");

        QSignalSpy spy(&model, &QAbstractItemModel::dataChanged);
        model.appendEvent(makeEditEvent("$edit2", "@alice:server", "$msg", "v3"),
                          "@bob:server");

        QCOMPARE(model.data(model.index(0), MessageModel::BodyRole).toString(), "v3");
        QCOMPARE(model.data(model.index(0), MessageModel::EditedRole).toBool(), true);
        // Exactly one row repainted — the "(edited)" marker updates in place.
        QCOMPARE(rowsTouched(spy), 1);
        QCOMPARE(spy.at(0).at(0).value<QModelIndex>().row(), 0);

        const auto history = model.editHistory("$msg");
        QCOMPARE(history.size(), 3);
        QCOMPARE(history.at(0).toMap().value("body").toString(), "v1");
        QCOMPARE(history.at(1).toMap().value("body").toString(), "v2");
        QCOMPARE(history.at(2).toMap().value("body").toString(), "v3");
    }

    void testReplayedEditSiblingIsIdempotent()
    {
        // Same edit event twice with no server bundle involved (an ordinary
        // sync replay). The old code appended to history on every copy.
        MessageModel model;
        model.appendEvent(makeMessageEvent("$msg", "@alice:server", "v1", 1000),
                          "@bob:server");
        model.appendEvent(makeEditEvent("$edit", "@alice:server", "$msg", "v2"),
                          "@bob:server");
        model.appendEvent(makeEditEvent("$edit", "@alice:server", "$msg", "v2"),
                          "@bob:server");

        const auto history = model.editHistory("$msg");
        QCOMPARE(history.size(), 2);
        QCOMPARE(history.at(0).toMap().value("body").toString(), "v1");
        QCOMPARE(history.at(1).toMap().value("body").toString(), "v2");
    }

    void testPrependDropsReplacementEvents()
    {
        // Back-pagination used to insert every edit in the fetched page as its
        // own row rendering the "* new text" fallback body.
        MessageModel model;
        QVector<bsfchat::RoomEvent> page{
            makeReconciledEvent("$old1", "@alice:server", "fixed typo",
                                "fxied typo", "$edit1", 1000, 1500),
            makeEditEvent("$edit1", "@alice:server", "$old1", "fixed typo"),
            makeMessageEvent("$old2", "@alice:server", "next", 2000),
        };
        model.prependEvents(page, "@bob:server");

        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.indexForEventId("$edit1"), -1);
        QCOMPARE(model.data(model.index(0), MessageModel::BodyRole).toString(),
                 "fixed typo");
        QCOMPARE(model.data(model.index(1), MessageModel::BodyRole).toString(), "next");
        // The prepended original still carries its history from the bundle.
        QCOMPARE(model.data(model.index(0), MessageModel::EditedRole).toBool(), true);
        const auto history = model.editHistory("$old1");
        QCOMPARE(history.size(), 2);
        QCOMPARE(history.at(0).toMap().value("body").toString(), "fxied typo");
    }

    // --- @mentions --------------------------------------------------------

    void testMentionTokenMatchesComposerForm()
    {
        // MessageInput.qml's _insertMention strips whitespace so the token is
        // a single word. Renderer and composer must agree or nothing matches.
        QCOMPARE(bsfchat::client::mentionToken("Josh Smith"), "@JoshSmith");
        QCOMPARE(bsfchat::client::mentionToken("Josh"), "@Josh");
    }

    void testMentionParsedAndRendered()
    {
        MessageModel model;
        QMap<QString, QString> cache;
        cache["@josh:server"] = "Josh Smith";
        cache["@carol:server"] = "Carol";
        model.setDisplayNameCache(&cache);

        model.appendEvent(makeMentionEvent("$m1", "@alice:server",
                                           "hey @JoshSmith and @Carol look",
                                           {"@josh:server", "@carol:server"}),
                          "@josh:server");

        auto idx = model.index(0);
        QCOMPARE(model.data(idx, MessageModel::MentionsMeRole).toBool(), true);
        QCOMPARE(model.data(idx, MessageModel::MentionsRoomRole).toBool(), false);

        const QString html = model.data(idx, MessageModel::FormattedBodyRole).toString();
        // Both mentions become profile links...
        QVERIFY(html.contains("bsfchat://user/%40josh%3Aserver"));
        QVERIFY(html.contains("bsfchat://user/%40carol%3Aserver"));
        QVERIFY(html.contains(">@JoshSmith</a>"));
        QVERIFY(html.contains(">@Carol</a>"));
        // ...and the local user's own mention is styled differently from
        // everybody else's, which is the whole point of the highlight.
        const int selfAt = html.indexOf("%40josh%3Aserver");
        const int otherAt = html.indexOf("%40carol%3Aserver");
        QVERIFY(selfAt >= 0 && otherAt >= 0);
        QVERIFY(html.mid(selfAt, otherAt - selfAt) != html.mid(otherAt));
        QVERIFY(html.contains("#ffd166")); // self
        QVERIFY(html.contains("#7aa2ff")); // other
        // Plain text around the mentions survives.
        QVERIFY(html.contains("hey "));
        QVERIFY(html.contains(" look"));
    }

    void testMentionOfSomeoneElseIsNotMineButStillRenders()
    {
        MessageModel model;
        QMap<QString, QString> cache;
        cache["@carol:server"] = "Carol";
        model.setDisplayNameCache(&cache);
        model.appendEvent(makeMentionEvent("$m1", "@alice:server", "ping @Carol",
                                           {"@carol:server"}),
                          "@josh:server");

        auto idx = model.index(0);
        QCOMPARE(model.data(idx, MessageModel::MentionsMeRole).toBool(), false);
        const QString html = model.data(idx, MessageModel::FormattedBodyRole).toString();
        QVERIFY(html.contains(">@Carol</a>"));
        QVERIFY(!html.contains("#ffd166")); // never the self style
    }

    void testRoomMentionRendersDistinctly()
    {
        MessageModel model;
        model.appendEvent(makeMentionEvent("$m1", "@alice:server",
                                           "@room server restart in 5", {}, true),
                          "@josh:server");

        auto idx = model.index(0);
        QCOMPARE(model.data(idx, MessageModel::MentionsRoomRole).toBool(), true);
        const QString html = model.data(idx, MessageModel::FormattedBodyRole).toString();
        QVERIFY(html.contains("#ff9f6e"));
        QVERIFY(html.contains(">@room</span>"));
        // @room is not a person: it must not become a profile link.
        QVERIFY(!html.contains("bsfchat://user/"));
    }

    void testMentionRenderingEscapesHostileDisplayName()
    {
        // A display name is set by its owner. It reaches the RichText engine,
        // so if the renderer interpolated it raw this would be stored XSS
        // against every reader of the channel.
        MessageModel model;
        QMap<QString, QString> cache;
        cache["@evil:server"] = "<img src=x onerror=alert(1)>";
        cache["@evil2:server"] = "Bob\" onmouseover=\"steal()";
        model.setDisplayNameCache(&cache);

        // The composer strips whitespace, so these are the tokens that would
        // actually appear in a body naming those users.
        const QString t1 = bsfchat::client::mentionToken("<img src=x onerror=alert(1)>");
        const QString t2 = bsfchat::client::mentionToken("Bob\" onmouseover=\"steal()");
        model.appendEvent(makeMentionEvent(
            "$m1", "@alice:server",
            ("look " + t1 + " " + t2).toStdString(),
            {"@evil:server", "@evil2:server"}),
            "@josh:server");

        const QString html = model.data(model.index(0),
                                        MessageModel::FormattedBodyRole).toString();
        // No tag from the display name survives as markup — the payload is
        // present, but only as escaped text.
        QVERIFY(!html.contains("<img"));
        QVERIFY(html.contains("&lt;img"));
        // ...and the quote in the second name can't break out of an
        // attribute, so no event handler is ever parsed.
        QVERIFY(!html.contains("onmouseover=\""));
        QVERIFY(html.contains("&quot;"));
        QCOMPARE(html.count("<a href=\""), html.count("</a>"));

        // The strong form of the claim: strip the markup this renderer is
        // allowed to emit, and NOTHING that looks like a tag may remain. Any
        // future change that lets a display name reach markup fails here even
        // if it uses a payload this test never thought of.
        QString residue = html;
        residue.remove(QRegularExpression(
            "<a href=\"bsfchat://user/[^\"]*\" style=\"[^\"]*\">"));
        residue.remove(QRegularExpression("<span style=\"[^\"]*\">"));
        residue.remove("</a>");
        residue.remove("</span>");
        residue.remove("<br>");
        QVERIFY2(!residue.contains(QLatin1Char('<')),
                 qPrintable("unexpected markup survived: " + residue));
    }

    void testSenderSuppliedMentionAnchorCannotImpersonate()
    {
        // formatted_body is attacker-controlled. A sender can anchor
        // bsfchat://user/<victim> and label it with somebody else's name, or
        // hand-roll attributes. The renderer re-emits the anchor from the
        // locally-known identity instead of trusting either.
        QVector<bsfchat::client::MentionTarget> targets{
            {"@carol:server", "Carol", false}
        };
        const QString hostile =
            "<a href=\"bsfchat://user/%40carol%3Aserver\" "
            "onclick=\"evil()\" style=\"color:red\">@Administrator</a>";
        const QString out = bsfchat::client::renderMentions(hostile, targets, false);

        QVERIFY(!out.contains("onclick"));
        QVERIFY(!out.contains("@Administrator"));
        QVERIFY(out.contains(">@Carol</a>"));
        QVERIFY(out.contains("bsfchat://user/%40carol%3Aserver"));
        // And no nesting: exactly one anchor came out.
        QCOMPARE(out.count("<a "), 1);
    }

    void testMentionRenderingLeavesCodeSpansAndAnchorsAlone()
    {
        QVector<bsfchat::client::MentionTarget> targets{
            {"@carol:server", "Carol", false}
        };
        // Inside <code> a mention is being quoted, not addressed.
        const QString code = "<code>@Carol</code> and @Carol";
        const QString out = bsfchat::client::renderMentions(code, targets, false);
        QCOMPARE(out.count("<a "), 1);
        QVERIFY(out.contains("<code>@Carol</code>"));

        // A token inside a longer word is not a mention.
        const QString email = "mail me at josh@Carol.example";
        QCOMPARE(bsfchat::client::renderMentions(email, targets, false), email);

        // ...nor is a prefix of a longer name.
        QVector<bsfchat::client::MentionTarget> two{
            {"@c:server", "Carol", false}, {"@c2:server", "CarolAnne", false}
        };
        const QString both = bsfchat::client::renderMentions("@CarolAnne", two, false);
        QVERIFY(both.contains(">@CarolAnne</a>"));
        QCOMPARE(both.count("<a "), 1);
    }

    void testMentionMarkupSurvivesAnEditWithoutWideningRepaint()
    {
        // Editing a message rebuilds its body, which throws away the baked
        // anchors — they have to be re-applied, and doing so must not turn a
        // one-row repaint into a model-wide one.
        MessageModel model;
        QMap<QString, QString> cache;
        cache["@josh:server"] = "Josh";
        model.setDisplayNameCache(&cache);
        for (int i = 0; i < 6; ++i) {
            model.appendEvent(makeMessageEvent("$ev" + std::to_string(i),
                                               "@alice:server", "m", 1000 + i * 1000),
                              "@josh:server");
        }

        QSignalSpy insertSpy(&model, &QAbstractItemModel::dataChanged);
        model.appendEvent(makeMentionEvent("$m1", "@alice:server", "yo @Josh",
                                           {"@josh:server"}, false, 9000),
                          "@josh:server");
        // Ingesting a mention is a plain insert: it must not repaint any
        // existing row.
        QCOMPARE(rowsTouched(insertSpy), 0);
        QCOMPARE(model.data(model.index(6), MessageModel::MentionsMeRole).toBool(), true);

        QSignalSpy spy(&model, &QAbstractItemModel::dataChanged);
        model.appendEvent(makeEditEvent("$edit", "@alice:server", "$m1",
                                        "yo @Josh again"),
                          "@josh:server");

        QCOMPARE(rowsTouched(spy), 1);
        QCOMPARE(spy.at(0).at(0).value<QModelIndex>().row(), 6);
        const QString html = model.data(model.index(6),
                                        MessageModel::FormattedBodyRole).toString();
        QVERIFY(html.contains(">@Josh</a>"));
        QVERIFY(html.contains("again"));
    }

    void testMentionsAreNotRenderedWithoutTheMentionsBlock()
    {
        // No m.mentions ⇒ nothing to trust, so a bare "@Josh" stays plain
        // text. Highlighting on text alone would let anyone fake a ping.
        MessageModel model;
        QMap<QString, QString> cache;
        cache["@josh:server"] = "Josh";
        model.setDisplayNameCache(&cache);
        model.appendEvent(makeMessageEvent("$m1", "@alice:server", "yo @Josh", 1000),
                          "@josh:server");

        auto idx = model.index(0);
        QCOMPARE(model.data(idx, MessageModel::MentionsMeRole).toBool(), false);
        QVERIFY(!model.data(idx, MessageModel::FormattedBodyRole)
                     .toString().contains("bsfchat://user/"));
    }

    void testThreadRepliesCarryMentionMarkupAndFlags()
    {
        // The thread drawer renders the same prose the timeline does, off
        // threadReplies() rather than the model roles. If that map drops the
        // rendered markup, a reply that pings you shows a bare "@JoshSmith"
        // in the drawer and a highlighted anchor in the channel behind it.
        MessageModel model;
        QMap<QString, QString> cache;
        cache["@josh:server"] = "Josh Smith";
        model.setDisplayNameCache(&cache);

        model.appendEvent(makeMessageEvent("$root", "@alice:server", "topic", 1000),
                          "@josh:server");

        auto pingsMe = makeMentionEvent("$t1", "@alice:server",
                                         "@JoshSmith take a look",
                                         {"@josh:server"}, false, 2000);
        pingsMe.content.data["m.relates_to"] = {
            {"rel_type", "m.thread"}, {"event_id", "$root"}
        };
        model.appendEvent(pingsMe, "@josh:server");

        auto pingsRoom = makeMentionEvent("$t2", "@carol:server",
                                           "@room heads up", {}, true, 3000);
        pingsRoom.content.data["m.relates_to"] = {
            {"rel_type", "m.thread"}, {"event_id", "$root"}
        };
        model.appendEvent(pingsRoom, "@josh:server");

        model.appendEvent(makeThreadReply("$t3", "@dave:server", "$root",
                                          "no ping here", 4000),
                          "@josh:server");

        const QVariantList replies = model.threadReplies("$root");
        QCOMPARE(replies.size(), 3);

        const QVariantMap mine = replies.at(0).toMap();
        QCOMPARE(mine.value("eventId").toString(), QStringLiteral("$t1"));
        QCOMPARE(mine.value("mentionsMe").toBool(), true);
        QCOMPARE(mine.value("mentionsRoom").toBool(), false);
        // Same rendered markup the timeline row shows, not the plain body.
        const QString mineHtml = mine.value("formattedBody").toString();
        QVERIFY(mineHtml.contains("bsfchat://user/%40josh%3Aserver"));
        QVERIFY(mineHtml.contains(">@JoshSmith</a>"));
        QVERIFY(mineHtml.contains("#ffd166")); // self-mention style
        QCOMPARE(mineHtml,
                 model.data(model.index(1), MessageModel::FormattedBodyRole)
                     .toString());
        // The plain body stays available for anything that wants it.
        QCOMPARE(mine.value("body").toString(),
                 QStringLiteral("@JoshSmith take a look"));

        const QVariantMap room = replies.at(1).toMap();
        QCOMPARE(room.value("mentionsMe").toBool(), false);
        QCOMPARE(room.value("mentionsRoom").toBool(), true);
        QVERIFY(room.value("formattedBody").toString().contains(">@room</span>"));

        // An ordinary reply is neither flagged nor markup-free — it still
        // carries its markdown-rendered body.
        const QVariantMap plain = replies.at(2).toMap();
        QCOMPARE(plain.value("mentionsMe").toBool(), false);
        QCOMPARE(plain.value("mentionsRoom").toBool(), false);
        QVERIFY(!plain.value("formattedBody").toString().contains("bsfchat://user/"));
        QVERIFY(plain.value("formattedBody").toString().contains("no ping here"));
    }

    // --- mention badges on the channel list -------------------------------

    void testRoomListMentionCountIsSeparateFromUnread()
    {
        RoomListModel model;
        model.ensureRoom("!room1:server");
        QCOMPARE(model.mentionCountFor("!room1:server"), 0);

        model.incrementUnreadCount("!room1:server", 3);
        model.incrementMentionCount("!room1:server", 1);
        model.incrementMentionCount("!room1:server", 1);
        QCOMPARE(model.mentionCountFor("!room1:server"), 2);
        QCOMPARE(model.totalMentionCount(), 2);
        QCOMPARE(model.data(model.index(0),
                            RoomListModel::MentionCountRole).toInt(), 2);

        // A server-reported unread total says nothing about whether the
        // mention has been seen, so it must not clear the badge.
        model.setUnreadCount("!room1:server", 7);
        QCOMPARE(model.mentionCountFor("!room1:server"), 2);

        // Opening the room does.
        model.resetUnreadCount("!room1:server");
        QCOMPARE(model.mentionCountFor("!room1:server"), 0);
        QCOMPARE(model.data(model.index(0),
                            RoomListModel::UnreadCountRole).toInt(), 0);

        QCOMPARE(model.mentionCountFor("!nope:server"), 0);
    }

    // ── unread_notifications.highlight_count ─────────────────────────────
    //
    // The server reports the mention count per joined room, computed against the
    // SERVER-side read marker. Before this the badge was incremented purely from
    // witnessed arrivals, which under-reports for any session that resumed from
    // a persisted sync token: it has no timeline events for anything older than
    // the token, so mentions from while it was offline were invisible.

    // Applies one sync batch's badge decision to the model, exactly as
    // ServerConnection::processSyncResponse does.
    static void applyBadge(RoomListModel& model, const QString& roomId,
                           bool isActiveRoom, std::optional<int> highlightCount,
                           int witnessedMentions)
    {
        using namespace bsfchat::client;
        const auto update = mentionBadgeUpdate(isActiveRoom, highlightCount,
                                               witnessedMentions);
        switch (update.action) {
        case MentionBadgeUpdate::Action::SetAbsolute:
            model.setMentionCount(roomId, update.value);
            break;
        case MentionBadgeUpdate::Action::Increment:
            model.incrementMentionCount(roomId, update.value);
            break;
        case MentionBadgeUpdate::Action::None:
            break;
        }
    }

    void testHighlightCountDrivesMentionBadge()
    {
        RoomListModel model;
        model.ensureRoom("!room1:server");

        // A server-reported count lands on the badge whole — including mentions
        // this client never witnessed arriving, which is the entire point.
        applyBadge(model, "!room1:server", false, 3, 0);
        QCOMPARE(model.mentionCountFor("!room1:server"), 3);
        QCOMPARE(model.data(model.index(0),
                            RoomListModel::MentionCountRole).toInt(), 3);

        // Absolute, so it can go DOWN — reading the mention on another device
        // advances the server's read marker and the count drops.
        applyBadge(model, "!room1:server", false, 1, 0);
        QCOMPARE(model.mentionCountFor("!room1:server"), 1);
        applyBadge(model, "!room1:server", false, 0, 0);
        QCOMPARE(model.mentionCountFor("!room1:server"), 0);

        // The active room is read by definition — forced to zero regardless of
        // what the server says, because the read marker we send alongside is
        // what makes the server agree on the next poll.
        applyBadge(model, "!room1:server", false, 5, 0);
        QCOMPARE(model.mentionCountFor("!room1:server"), 5);
        applyBadge(model, "!room1:server", /*isActiveRoom=*/true, 5, 0);
        QCOMPARE(model.mentionCountFor("!room1:server"), 0);

        // A negative count from a broken server must not produce a negative
        // badge.
        applyBadge(model, "!room1:server", false, -4, 0);
        QCOMPARE(model.mentionCountFor("!room1:server"), 0);
    }

    void testHighlightCountIsIdempotentAcrossResume()
    {
        // The launch sequence for a resumed session: hydrate from the local
        // snapshot (which does NOT persist highlight_count), then the catch-up
        // sync, then steady-state polls. The count must be right, not doubled.
        RoomListModel model;
        model.ensureRoom("!room1:server");

        // 1. Hydration replay. No highlight_count in the snapshot, and the
        //    witnessed-arrival counter is pinned at 0 by the
        //    m_hydratingFromCache gate — so hydration changes nothing.
        applyBadge(model, "!room1:server", false, std::nullopt, 0);
        QCOMPARE(model.mentionCountFor("!room1:server"), 0);

        // 2. Catch-up sync. Two mentions are visible in the batch, but the
        //    server knows about four — two predate the resume token, so this
        //    client can never witness them. The absolute count wins, and
        //    crucially is NOT added to the witnessed pair.
        applyBadge(model, "!room1:server", false, 4, 2);
        QCOMPARE(model.mentionCountFor("!room1:server"), 4);

        // 3. Steady state: the server keeps re-reporting the same count on
        //    every poll for as long as the mentions stay unread. Re-applying it
        //    must be a no-op, which is exactly what an increment would have got
        //    wrong (4 → 8 → 12 …).
        for (int i = 0; i < 5; ++i) {
            applyBadge(model, "!room1:server", false, 4, 0);
        }
        QCOMPARE(model.mentionCountFor("!room1:server"), 4);
    }

    void testHighlightCountAbsentFallsBackToWitnessedArrivals()
    {
        // A server that reports no highlight_count at all (pre-highlight_count
        // build) must still get a badge, via the legacy witnessed-arrival path —
        // dropping the badge entirely would be a regression for those servers.
        RoomListModel model;
        model.ensureRoom("!room1:server");

        applyBadge(model, "!room1:server", false, std::nullopt, 2);
        QCOMPARE(model.mentionCountFor("!room1:server"), 2);
        // Increments accumulate across batches, as they must when nothing
        // absolute is available.
        applyBadge(model, "!room1:server", false, std::nullopt, 1);
        QCOMPARE(model.mentionCountFor("!room1:server"), 3);

        // Nothing witnessed and nothing reported: leave the badge alone rather
        // than assuming zero. Assuming zero would let an ordinary sync for an
        // unrelated event silently clear a mention on an old server.
        applyBadge(model, "!room1:server", false, std::nullopt, 0);
        QCOMPARE(model.mentionCountFor("!room1:server"), 3);
    }

    void testHighlightCountRepaintScopeIsOneRowAndOnlyOnChange()
    {
        // Repaint scope. The server re-reports highlight_count on EVERY poll, so
        // a setter that emitted unconditionally would make sidebar repaint
        // traffic proportional to the sync rate rather than to actual changes.
        RoomListModel model;
        for (int i = 0; i < 5; ++i) {
            model.ensureRoom(QStringLiteral("!room%1:server").arg(i));
        }
        QSignalSpy spy(&model, &QAbstractItemModel::dataChanged);

        applyBadge(model, "!room2:server", false, 3, 0);
        QCOMPARE(spy.count(), 1);
        // Exactly the one row that changed — not a range, not the whole model.
        QCOMPARE(rowsTouched(spy), 1);
        QCOMPARE(spy.at(0).at(0).value<QModelIndex>().row(), 2);
        QCOMPARE(spy.at(0).at(2).value<QList<int>>(),
                 QList<int>{RoomListModel::MentionCountRole});

        // Re-applying the same count must be completely silent.
        spy.clear();
        for (int i = 0; i < 10; ++i) {
            applyBadge(model, "!room2:server", false, 3, 0);
        }
        QCOMPARE(spy.count(), 0);
        QCOMPARE(rowsTouched(spy), 0);

        // A change repaints one row again, and does not touch its neighbours.
        applyBadge(model, "!room2:server", false, 1, 0);
        QCOMPARE(rowsTouched(spy), 1);
        QCOMPARE(model.mentionCountFor("!room1:server"), 0);
        QCOMPARE(model.mentionCountFor("!room3:server"), 0);
    }

    void testMentionBadgeDecisionTable()
    {
        // The decision itself, independent of the model. Documents the
        // precedence: active room beats everything, an absolute server count
        // beats a witnessed guess, and a guess is used only when there is no
        // server count.
        using namespace bsfchat::client;
        using Action = MentionBadgeUpdate::Action;

        QCOMPARE(mentionBadgeUpdate(true, 7, 3).action, Action::SetAbsolute);
        QCOMPARE(mentionBadgeUpdate(true, 7, 3).value, 0);
        QCOMPARE(mentionBadgeUpdate(true, std::nullopt, 3).value, 0);

        QCOMPARE(mentionBadgeUpdate(false, 7, 3).action, Action::SetAbsolute);
        QCOMPARE(mentionBadgeUpdate(false, 7, 3).value, 7);
        QCOMPARE(mentionBadgeUpdate(false, 0, 3).action, Action::SetAbsolute);
        QCOMPARE(mentionBadgeUpdate(false, 0, 3).value, 0);

        QCOMPARE(mentionBadgeUpdate(false, std::nullopt, 3).action, Action::Increment);
        QCOMPARE(mentionBadgeUpdate(false, std::nullopt, 3).value, 3);
        QCOMPARE(mentionBadgeUpdate(false, std::nullopt, 0).action, Action::None);
    }

    // ── mention set preserved across an edit ─────────────────────────────
    void testMentionSetSurvivesForEditPayload()
    {
        // ServerConnection::editMessage unions this into the edit's m.mentions,
        // because the server folds an edit's m.new_content in as the event's
        // content — an edit that omitted m.mentions blanked the highlight for
        // anyone loading the room afterwards.
        MessageModel model;

        auto plain = makeMessageEvent("$plain", "@alice:server", "no pings", 1000);
        model.appendEvent(plain, "@josh:server");
        QVERIFY(model.mentionSetFor("$plain").isEmpty());

        auto pinged = makeMessageEvent("$pinged", "@alice:server",
                                       "@josh look at this", 2000);
        pinged.content.data["m.mentions"] = {
            {"user_ids", nlohmann::json::array({"@josh:server", "@carol:server"})}};
        model.appendEvent(pinged, "@josh:server");
        QCOMPARE(model.mentionSetFor("$pinged"),
                 QStringList({"@josh:server", "@carol:server"}));

        // An @room broadcast is a boolean on the wire; the send/edit paths speak
        // the "@room" sentinel, so it has to be flattened back into the list.
        auto broadcast = makeMessageEvent("$all", "@alice:server", "@room heads up",
                                          3000);
        broadcast.content.data["m.mentions"] = {{"room", true}};
        model.appendEvent(broadcast, "@josh:server");
        QCOMPARE(model.mentionSetFor("$all"), QStringList({"@room"}));

        auto both = makeMessageEvent("$both", "@alice:server", "@room and @josh",
                                      4000);
        both.content.data["m.mentions"] = {
            {"user_ids", nlohmann::json::array({"@josh:server"})}, {"room", true}};
        model.appendEvent(both, "@josh:server");
        QCOMPARE(model.mentionSetFor("$both"),
                 QStringList({"@josh:server", "@room"}));

        // Unknown event — no crash, no guess.
        QVERIFY(model.mentionSetFor("$nope").isEmpty());
        QVERIFY(model.mentionSetFor("").isEmpty());
    }

    // ── POST /_matrix/client/v3/search response parsing ──────────────────
    void testSearchResponseParsing()
    {
        using bsfchat::client::parseSearchResponse;

        const QByteArray body = R"({
          "search_categories": {
            "room_events": {
              "count": 2,
              "highlights": ["deploy", "friday"],
              "results": [
                {"rank": 1.5, "result": {
                  "event_id": "$one", "room_id": "!eng:server",
                  "sender": "@alice:server", "origin_server_ts": 1700000000000,
                  "content": {"msgtype": "m.text", "body": "deploy on friday"}}},
                {"rank": 0.5, "result": {
                  "event_id": "$two", "room_id": "!ops:server",
                  "sender": "@bob:server", "origin_server_ts": 1700000001000,
                  "content": {"msgtype": "m.text", "body": "no deploy"}}}
              ]
            }
          }
        })";

        auto r = parseSearchResponse(200, body);
        QVERIFY(r.ok);
        QVERIFY(r.errorMessage.isEmpty());
        QCOMPARE(r.count, 2);
        QCOMPARE(r.highlights, QStringList({"deploy", "friday"}));
        QCOMPARE(r.hits.size(), 2);
        QCOMPARE(r.hits[0].eventId, QStringLiteral("$one"));
        // roomId is load-bearing: a hit is usually not in the room on screen,
        // so the UI needs it to switch channel before scrolling.
        QCOMPARE(r.hits[0].roomId, QStringLiteral("!eng:server"));
        QCOMPARE(r.hits[0].sender, QStringLiteral("@alice:server"));
        QCOMPARE(r.hits[0].body, QStringLiteral("deploy on friday"));
        QCOMPARE(r.hits[0].timestamp, 1700000000000LL);
        QVERIFY(r.hits[0].rank > r.hits[1].rank);
        // No next_batch key means this was the last page.
        QVERIFY(r.nextBatch.isEmpty());
    }

    void testSearchResponsePagination()
    {
        using bsfchat::client::parseSearchResponse;
        const QByteArray body = R"({"search_categories":{"room_events":{
            "count": 99, "next_batch": "30", "highlights": [],
            "results": [{"rank": 1.0, "result": {"event_id":"$a",
              "room_id":"!r:s","sender":"@a:s","origin_server_ts":1,
              "content":{"body":"x"}}}]}}})";
        auto r = parseSearchResponse(200, body);
        QVERIFY(r.ok);
        QCOMPARE(r.nextBatch, QStringLiteral("30"));
        // The server's total exceeds what this page carries — the UI shows
        // "1 of 99" rather than claiming 1 match.
        QCOMPARE(r.count, 99);
        QCOMPARE(r.hits.size(), 1);
    }

    void testSearchResponseEmptyIsNotAnError()
    {
        using bsfchat::client::parseSearchResponse;
        // Punctuation-only input: the server tokenises FTS5 metacharacters away,
        // so typing `"` or `*` yields zero matches rather than a syntax error.
        // That must read as "no matches", never as a failure.
        const QByteArray body = R"({"search_categories":{"room_events":{
            "count":0,"results":[],"highlights":[]}}})";
        auto r = parseSearchResponse(200, body);
        QVERIFY(r.ok);
        QVERIFY(r.errorMessage.isEmpty());
        QCOMPARE(r.count, 0);
        QCOMPARE(r.hits.size(), 0);
    }

    void testSearchResponseErrorsAreReportedNotSwallowed()
    {
        using bsfchat::client::parseSearchResponse;

        // 400 with a Matrix error: prefer the server's own wording, which is
        // more specific than anything we could guess.
        auto bad = parseSearchResponse(400,
            R"({"errcode":"M_INVALID_PARAM","error":"Search term is not usable"})");
        QVERIFY(!bad.ok);
        QCOMPARE(bad.errorCode, QStringLiteral("M_INVALID_PARAM"));
        QCOMPARE(bad.errorMessage, QStringLiteral("Search term is not usable"));

        // 501: the server's SQLite has no FTS5 module. Distinct from "no
        // matches" and the user has to be told, or search looks broken-but-quiet.
        auto unsupported = parseSearchResponse(501,
            R"({"errcode":"M_UNRECOGNIZED","error":""})");
        QVERIFY(!unsupported.ok);
        QVERIFY(unsupported.errorMessage.contains("doesn't support"));

        // Error status with no usable body at all still yields a message.
        auto bodyless = parseSearchResponse(500, QByteArray());
        QVERIFY(!bodyless.ok);
        QVERIFY(!bodyless.errorMessage.isEmpty());

        // A 200 carrying an error object (misbehaving proxy) is still an error —
        // recognised by errcode, not by status.
        auto sneaky = parseSearchResponse(200,
            R"({"errcode":"M_FORBIDDEN","error":"nope"})");
        QVERIFY(!sneaky.ok);
        QCOMPARE(sneaky.errorMessage, QStringLiteral("nope"));

        // Garbage with a 200: the one thing we must not do is report "no
        // matches", which would be indistinguishable from a successful search.
        auto garbage = parseSearchResponse(200, QByteArray("<html>nope</html>"));
        QVERIFY(!garbage.ok);
        QVERIFY(!garbage.errorMessage.isEmpty());

        // 200, valid JSON, no room_events category: a well-formed answer about
        // nothing we asked for. Empty result, not an error.
        auto otherCategory = parseSearchResponse(200,
            R"({"search_categories":{"other":{}}})");
        QVERIFY(otherCategory.ok);
        QCOMPARE(otherCategory.hits.size(), 0);
    }

    void testSearchResponseDropsUnusableHits()
    {
        using bsfchat::client::parseSearchResponse;
        // A result with no event_id cannot be jumped to, which is the only thing
        // a search result is for — drop it rather than render a dead row. Same
        // for an entry with no `result` object at all.
        const QByteArray body = R"({"search_categories":{"room_events":{
            "count":3,"highlights":[],"results":[
              {"rank":1.0,"result":{"room_id":"!r:s","content":{"body":"no id"}}},
              {"rank":1.0},
              {"rank":1.0,"result":{"event_id":"$ok","room_id":"!r:s",
                "sender":"@a:s","origin_server_ts":5,"content":{"body":"keep"}}}
            ]}}})";
        auto r = parseSearchResponse(200, body);
        QVERIFY(r.ok);
        QCOMPARE(r.hits.size(), 1);
        QCOMPARE(r.hits[0].eventId, QStringLiteral("$ok"));
        QCOMPARE(r.hits[0].body, QStringLiteral("keep"));
        // `count` is the server's, and is deliberately NOT recomputed from the
        // rows we kept — it is the total match count, not the page size.
        QCOMPARE(r.count, 3);
    }

    void testRoomListEnsureRoomDoesNotShiftFields()
    {
        // ensureRoom used to positionally brace-init RoomEntry, so inserting a
        // field mid-struct silently slid every later field one slot over.
        RoomListModel model;
        model.ensureRoom("!room1:server");
        auto idx = model.index(0);
        QCOMPARE(model.data(idx, RoomListModel::RoomIdRole).toString(), "!room1:server");
        QCOMPARE(model.data(idx, RoomListModel::UnreadCountRole).toInt(), 0);
        QCOMPARE(model.data(idx, RoomListModel::MentionCountRole).toInt(), 0);
        QCOMPARE(model.data(idx, RoomListModel::IsVoiceRole).toBool(), false);
        QCOMPARE(model.data(idx, RoomListModel::VoiceMemberCountRole).toInt(), 0);
        QCOMPARE(model.data(idx, RoomListModel::SortOrderRole).toInt(), 0);
        QVERIFY(model.data(idx, RoomListModel::TopicRole).toString().isEmpty());
    }

    // ── Permission mirror (util/PermissionMath) ─────────────────────────────
    // These guard the client's copy of the server's permission algorithm. When
    // the two disagree the UI either offers actions the server refuses or, worse
    // for the owner, hides actions a role is genuinely entitled to.

    void testPermissionsRoleGrantConfersServerScopeCapability()
    {
        using namespace bsfchat::permmath;
        QVector<Role> roles{
            {QStringLiteral("everyone"), 0, kEveryoneDefault},
            {QStringLiteral("builder"),  5, kEveryoneDefault | kManageChannels},
        };
        const QStringList mine{QStringLiteral("everyone"), QStringLiteral("builder")};

        // Server scope (no overrides): the create-channel question.
        const Flags server = effectivePermissions(roles, mine, QStringLiteral("@bob:t"), nullptr);
        QVERIFY((server & kManageChannels) != 0);
        QVERIFY((server & kAdministrator) == 0);
        QVERIFY((server & kBanMembers) == 0);
    }

    void testPermissionsWithoutTheRoleHaveNoChannelManagement()
    {
        using namespace bsfchat::permmath;
        QVector<Role> roles{{QStringLiteral("everyone"), 0, kEveryoneDefault}};
        const Flags server = effectivePermissions(
            roles, {QStringLiteral("everyone")}, QStringLiteral("@mallory:t"), nullptr);
        QVERIFY((server & kSendMessages) != 0);
        QVERIFY((server & kManageChannels) == 0);
    }

    void testPermissionsChannelOverrideDoesNotLeakToServerScope()
    {
        using namespace bsfchat::permmath;
        QVector<Role> roles{{QStringLiteral("everyone"), 0, kEveryoneDefault}};
        const QStringList mine{QStringLiteral("everyone")};
        QVector<Override> overrides{
            {QStringLiteral("user:@mallory:t"), kManageChannels, 0}
        };

        // In that channel the override applies...
        const Flags inChannel = effectivePermissions(
            roles, mine, QStringLiteral("@mallory:t"), &overrides);
        QVERIFY((inChannel & kManageChannels) != 0);

        // ...but the server-scope answer must ignore it, or the client lights up
        // a "create channel" affordance that handle_create_room then 403s.
        const Flags serverScope = effectivePermissions(
            roles, mine, QStringLiteral("@mallory:t"), nullptr);
        QVERIFY((serverScope & kManageChannels) == 0);
    }

    // The client must agree with the server about the SCOPE of the moderation and
    // nickname flags, or it renders affordances the server then refuses. Kick, ban
    // and both nickname flags are server-wide only: a per-channel override that
    // allows them confers nothing.
    void testPermissionsModerationAndNicknameFlagsAreServerScopeOnly()
    {
        using namespace bsfchat::permmath;
        QVector<Role> roles{{QStringLiteral("everyone"), 0, kEveryoneDefault}};
        const QStringList mine{QStringLiteral("everyone")};
        const Flags serverOnly =
            kKickMembers | kBanMembers | kManageNicknames;
        QVector<Override> overrides{
            {QStringLiteral("user:@mallory:t"), serverOnly, 0}
        };

        const Flags inChannel = effectivePermissions(
            roles, mine, QStringLiteral("@mallory:t"), &overrides);
        QVERIFY((inChannel & serverOnly) == serverOnly);

        // The scope the UI must actually ask about for these four capabilities.
        const Flags serverScope = effectivePermissions(
            roles, mine, QStringLiteral("@mallory:t"), nullptr);
        QVERIFY((serverScope & kKickMembers) == 0);
        QVERIFY((serverScope & kBanMembers) == 0);
        QVERIFY((serverScope & kManageNicknames) == 0);
        // CHANGE_NICKNAME is in the @everyone default, so it IS present at server
        // scope — and that is the point: it is granted by the role, never by a
        // channel override.
        QVERIFY((serverScope & kChangeNickname) != 0);
    }

    void testPermissionsAdministratorShortCircuitsAndIgnoresDenies()
    {
        using namespace bsfchat::permmath;
        QVector<Role> roles{
            {QStringLiteral("everyone"), 0, kEveryoneDefault},
            {QStringLiteral("admin"), 100, kAllFlags},
        };
        QVector<Override> overrides{
            {QStringLiteral("role:everyone"), 0, kViewChannel | kManageChannels}
        };
        const Flags p = effectivePermissions(
            roles, {QStringLiteral("everyone"), QStringLiteral("admin")},
            QStringLiteral("@owner:t"), &overrides);
        QCOMPARE(p, kAllFlags);
        QVERIFY((p & kManageChannels) != 0);
    }

    void testPermissionsOverridePrecedenceIsPositionThenUser()
    {
        using namespace bsfchat::permmath;
        QVector<Role> roles{
            {QStringLiteral("everyone"), 0, kEveryoneDefault},
            {QStringLiteral("low"),      1, kEveryoneDefault},
            {QStringLiteral("high"),     9, kEveryoneDefault},
        };
        const QStringList mine{QStringLiteral("everyone"), QStringLiteral("high"),
                               QStringLiteral("low")};

        // Deliberately listed out of position order to prove the sort happens:
        // the lower-positioned role denies, the higher one allows, so allow wins.
        QVector<Override> overrides{
            {QStringLiteral("role:high"), kManageChannels, 0},
            {QStringLiteral("role:low"),  0, kManageChannels},
        };
        Flags p = effectivePermissions(roles, mine, QStringLiteral("@bob:t"), &overrides);
        QVERIFY((p & kManageChannels) != 0);

        // A user-specific override is applied last and beats every role.
        overrides.append({QStringLiteral("user:@bob:t"), 0, kManageChannels});
        p = effectivePermissions(roles, mine, QStringLiteral("@bob:t"), &overrides);
        QVERIFY((p & kManageChannels) == 0);
    }

    void testPermissionsUnsyncedClientFallsBackToEveryoneDefaults()
    {
        using namespace bsfchat::permmath;
        // Roles arrive over /sync, so there is a window where the client knows
        // none. The server answers this case with the @everyone defaults; if the
        // client answered 0 instead it would grey out the composer and claim the
        // user has no permission to speak.
        const Flags p = effectivePermissions({}, {}, QStringLiteral("@bob:t"), nullptr);
        QCOMPARE(p, kEveryoneDefault);
        QVERIFY((p & kSendMessages) != 0);
        QVERIFY((p & kManageChannels) == 0);
    }

    void testPermissionsEveryoneAppliesEvenWhenNotListed()
    {
        using namespace bsfchat::permmath;
        QVector<Role> roles{
            {QStringLiteral("everyone"), 0, kEveryoneDefault},
            {QStringLiteral("builder"),  5, kManageChannels},
        };
        // Assignment lists only "builder" — @everyone is implicit server-side.
        const Flags p = effectivePermissions(
            roles, {QStringLiteral("builder")}, QStringLiteral("@bob:t"), nullptr);
        QVERIFY((p & kManageChannels) != 0);
        QVERIFY((p & kSendMessages) != 0);
    }
};

QTEST_MAIN(TestModels)
#include "test_models.moc"
