// Bot accounts: the badge path, the management view-model, and the rule that
// a bot's access token never reaches a log file or persisted state.
//
// Three things are pinned here, in that order.
//
// 1. THE BADGE. `bsfchat.bot` rides in m.room.member content, alongside
//    membership and bsfchat.nickname, on every path membership can be
//    learned from. So it arrives WITH the row that displays it: the member
//    list reads it straight off the event, and the message list stamps it at
//    append from the sender's membership, the way it already stamps the
//    sender's display name. Absence is the server's encoding for "human" —
//    it never writes `false` — which is what makes a lookup, rather than a
//    fetch, the whole of the client's job.
//
// 2. THE DIALOG'S VIEW-MODEL. BotAdminModel holds every decision the bot
//    management pane makes. It reaches the network through std::function
//    hooks (the seam VoiceSession uses for its transport), so this drives it
//    with a recording fake and hands it replies by hand, in the orders that
//    actually happen — including a list reply landing UNDERNEATH an open
//    token banner, which is the one ordering that can destroy a credential.
//
// 3. THE TOKEN. The server issues a bot's token exactly once and keeps no
//    readable copy. The client mirrors every qDebug/qWarning into a rotating
//    file in the user's home directory (util/FileLogger.h) and verbose mode
//    turns on `bsfchat.*=true` wholesale, so there is no log level at which
//    printing it would be safe. The last section scans the sources for the
//    shape of that mistake, in the spirit of test_qml_hygiene.cpp: it cannot
//    prove a token never leaks, but it fails on the commit that adds the
//    print, which is the failure that would otherwise ship unnoticed.

#include <QtTest>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>

#include "model/BotAdminModel.h"
#include "model/MemberListModel.h"
#include "model/MessageModel.h"
#include "util/PermissionMath.h"

#include <bsfchat/Constants.h>
#include <bsfchat/Permissions.h>

namespace permmath = bsfchat::permmath;

// permmath mirrors the bit VALUES but not protocol's `has()` helper — every
// call site in the client spells the mask test out. Same here, once.
static bool grants(permmath::Flags flags, permmath::Flags p)
{
    return (flags & p) == p;
}

namespace {

// A joined m.room.member event for `userId`, the shape MemberListModel reads.
// `isBot` writes `bsfchat.bot: true`; false leaves the key out entirely,
// because that is what the server does — it never writes `false`, and a test
// helper that emitted one would be testing a payload nobody sends.
bsfchat::RoomEvent memberJoin(const QString& userId, const QString& displayName,
                              bool isBot = false)
{
    bsfchat::RoomEvent ev;
    ev.type = "m.room.member";
    ev.state_key = userId.toStdString();
    ev.sender = userId.toStdString();
    ev.content.data["membership"] = "join";
    ev.content.data["displayname"] = displayName.toStdString();
    if (isBot) ev.content.data["bsfchat.bot"] = true;
    return ev;
}

bsfchat::RoomEvent textMessage(const QString& sender, const QString& body,
                               const QString& eventId)
{
    bsfchat::RoomEvent ev;
    ev.type = "m.room.message";
    ev.event_id = eventId.toStdString();
    ev.sender = sender.toStdString();
    ev.origin_server_ts = 1700000000000LL;
    ev.content.data["msgtype"] = "m.text";
    ev.content.data["body"] = body.toStdString();
    return ev;
}

QJsonObject botEntry(const QString& userId, const QString& displayName,
                     bool deactivated = false)
{
    QJsonObject o;
    o["user_id"] = userId;
    o["display_name"] = displayName;
    o["description"] = QStringLiteral("does a thing");
    o["owner_id"] = QStringLiteral("@josh:server");
    o["created_at"] = 1700000000000LL;
    o["last_seen_at"] = 1700000600000LL;
    o["deactivated"] = deactivated;
    return o;
}

// Records what BotAdminModel asked the network to do, so a test can assert on
// the calls and then feed the replies back in whatever order it likes.
struct RecordingHooks {
    QStringList calls;
    QString lastLocalpart;
    QString lastDisplayName;
    QString lastDescription;
    QString lastUserId;

    void installOn(BotAdminModel& model)
    {
        model.hooks.listBots = [this]() { calls << QStringLiteral("list"); };
        model.hooks.createBot = [this](const QString& localpart,
                                       const QString& displayName,
                                       const QString& description) {
            calls << QStringLiteral("create");
            lastLocalpart = localpart;
            lastDisplayName = displayName;
            lastDescription = description;
        };
        model.hooks.rotateToken = [this](const QString& userId) {
            calls << QStringLiteral("rotate");
            lastUserId = userId;
        };
        model.hooks.deactivateBot = [this](const QString& userId) {
            calls << QStringLiteral("deactivate");
            lastUserId = userId;
        };
    }
};

QString readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

// Comments only, never string literals — the same helper and the same caveat
// as test_qml_hygiene.cpp. Stripping can only remove text, so a scan over the
// result can miss an offender but cannot invent one.
QString withoutComments(QString src)
{
    static const QRegularExpression block(QStringLiteral(R"(/\*.*?\*/)"),
                                          QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression line(QStringLiteral("//[^\n]*"));
    return src.remove(block).remove(line);
}

QStringList filesUnder(const QString& root, const QString& glob)
{
    QStringList out;
    QDirIterator it(root, QStringList{glob}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) out << it.next();
    out.sort();
    return out;
}

} // namespace

class BotsTest : public QObject {
    Q_OBJECT

private slots:

    // ───────────── 1. the flag arrives on the member event ─────────────

    void aMemberEventCarriesTheBotFlag()
    {
        MemberListModel model;
        model.processEvent(memberJoin(QStringLiteral("@build:server"),
                                      QStringLiteral("Build Bot"), true));
        model.processEvent(memberJoin(QStringLiteral("@josh:server"),
                                      QStringLiteral("Josh")));
        QCOMPARE(model.rowCount(), 2);

        // The QML binding name. `model.isBot` is what the delegate reads; a
        // rename here is invisible in QML (undefined is falsy) and would
        // silently take every badge away.
        QCOMPARE(model.roleNames().value(MemberListModel::IsBotRole),
                 QByteArray("isBot"));

        // The badge is right on the FIRST frame of the row — there is no
        // window in which a bot renders as a human and is corrected later,
        // because the fact arrives with the row that displays it.
        QCOMPARE(model.data(model.index(0), MemberListModel::IsBotRole).toBool(), true);
        QCOMPARE(model.data(model.index(1), MemberListModel::IsBotRole).toBool(), false);
        QVERIFY(model.isBot(QStringLiteral("@build:server")));
        QVERIFY(!model.isBot(QStringLiteral("@josh:server")));
        // Nobody by that name is in the roster, so there is nothing to badge.
        QVERIFY(!model.isBot(QStringLiteral("@nobody:server")));
    }

    void anAbsentFlagMeansHumanRatherThanUnknown()
    {
        // The server writes `bsfchat.bot` only for bots and never as `false`,
        // so absence is the complete encoding for "human". The client must
        // not treat it as a third "we should go and ask" state — there is
        // nothing to ask, and a client that probed on absence would issue a
        // request per human in every roster forever.
        MemberListModel model;
        bsfchat::RoomEvent ev = memberJoin(QStringLiteral("@josh:server"),
                                           QStringLiteral("Josh"));
        QVERIFY(!ev.content.data.contains("bsfchat.bot"));
        model.processEvent(ev);
        QCOMPARE(model.data(model.index(0), MemberListModel::IsBotRole).toBool(), false);

        // An explicit false — which the server does not send, but a proxy or
        // an older recording might — reads the same way.
        MemberListModel explicitFalse;
        bsfchat::RoomEvent ev2 = memberJoin(QStringLiteral("@josh:server"),
                                            QStringLiteral("Josh"));
        ev2.content.data["bsfchat.bot"] = false;
        explicitFalse.processEvent(ev2);
        QCOMPARE(explicitFalse.data(explicitFalse.index(0),
                                    MemberListModel::IsBotRole).toBool(), false);
    }

    void aLaterMemberEventUpdatesTheBadgeAndSaysSo()
    {
        MemberListModel model;
        model.processEvent(memberJoin(QStringLiteral("@build:server"),
                                      QStringLiteral("Build Bot")));
        QCOMPARE(model.data(model.index(0), MemberListModel::IsBotRole).toBool(), false);

        QSignalSpy spy(&model, &QAbstractItemModel::dataChanged);
        // A second event for a member already in the roster goes down the
        // UPDATE branch, which repaints a named list of roles. IsBotRole has
        // to be in that list or the row keeps the value it was built with —
        // the U-M15 fix means an omitted role simply never repaints.
        model.processEvent(memberJoin(QStringLiteral("@build:server"),
                                      QStringLiteral("Build Bot"), true));

        QCOMPARE(model.data(model.index(0), MemberListModel::IsBotRole).toBool(), true);
        QCOMPARE(spy.count(), 1);
        const auto roles = spy.at(0).at(2).value<QList<int>>();
        QVERIFY2(roles.contains(MemberListModel::IsBotRole),
                 "the member-update branch does not repaint the badge");
        // Still a NAMED list, not the empty "everything changed" vector that
        // re-runs every binding on every delegate.
        QVERIFY(!roles.isEmpty());
    }

    void messageRowsTakeTheFlagFromTheSendersMembership()
    {
        // A message event carries no `bsfchat.bot` of its own — the flag is
        // on m.room.member — so MessageModel reads the set ServerConnection
        // builds from member events, exactly as it reads display names.
        QSet<QString> botUsers{QStringLiteral("@build:server")};
        MessageModel model;
        model.setBotUserCache(&botUsers);
        const QString me = QStringLiteral("@josh:server");

        model.appendEvent(textMessage(QStringLiteral("@build:server"),
                                      QStringLiteral("build #41 passed"),
                                      QStringLiteral("$a")), me);
        model.appendEvent(textMessage(me, QStringLiteral("nice"),
                                      QStringLiteral("$b")), me);

        QCOMPARE(model.roleNames().value(MessageModel::SenderIsBotRole),
                 QByteArray("senderIsBot"));
        QCOMPARE(model.data(model.index(0), MessageModel::SenderIsBotRole).toBool(), true);
        QCOMPARE(model.data(model.index(1), MessageModel::SenderIsBotRole).toBool(), false);
    }

    void refreshingFlagsRepaintsOnlyTheRowsThatMoved()
    {
        QSet<QString> botUsers;
        MessageModel model;
        model.setBotUserCache(&botUsers);
        const QString me = QStringLiteral("@josh:server");

        // Two bot messages either side of a human one, so a correct
        // implementation emits two ranges rather than one span over all three.
        model.appendEvent(textMessage(QStringLiteral("@build:server"),
                                      QStringLiteral("one"), QStringLiteral("$a")), me);
        model.appendEvent(textMessage(me, QStringLiteral("two"),
                                      QStringLiteral("$b")), me);
        model.appendEvent(textMessage(QStringLiteral("@build:server"),
                                      QStringLiteral("three"), QStringLiteral("$c")), me);

        QSignalSpy spy(&model, &QAbstractItemModel::dataChanged);

        // Nothing has moved yet: a refresh driven by an unrelated member
        // event must be silent, and these run on every member event in a
        // busy room.
        model.refreshBotFlags();
        QCOMPARE(spy.count(), 0);

        botUsers.insert(QStringLiteral("@build:server"));
        model.refreshBotFlags();

        QCOMPARE(spy.count(), 2);
        for (int i = 0; i < spy.count(); ++i) {
            QCOMPARE(spy.at(i).at(2).value<QList<int>>(),
                     QList<int>{MessageModel::SenderIsBotRole});
        }
        QCOMPARE(model.data(model.index(0), MessageModel::SenderIsBotRole).toBool(), true);
        QCOMPARE(model.data(model.index(1), MessageModel::SenderIsBotRole).toBool(), false);
        QCOMPARE(model.data(model.index(2), MessageModel::SenderIsBotRole).toBool(), true);

        // Idempotent — a second refresh over settled state says nothing.
        spy.clear();
        model.refreshBotFlags();
        QCOMPARE(spy.count(), 0);
    }

    void modelsWithoutACacheNeverBadge()
    {
        // Both models are constructed before ServerConnection wires anything
        // in, and both are used in other tests with no cache at all.
        MemberListModel members;
        members.processEvent(memberJoin(QStringLiteral("@build:server"),
                                        QStringLiteral("Build Bot"), true));
        // The member list needs no cache — the event is the source.
        QCOMPARE(members.data(members.index(0), MemberListModel::IsBotRole).toBool(), true);

        MessageModel messages;
        messages.appendEvent(textMessage(QStringLiteral("@build:server"),
                                         QStringLiteral("hi"), QStringLiteral("$a")),
                             QStringLiteral("@josh:server"));
        QCOMPARE(messages.data(messages.index(0),
                               MessageModel::SenderIsBotRole).toBool(), false);
        messages.refreshBotFlags(); // must not crash
    }

    // ─────────────────────── 2. the permission bit ───────────────────────

    void manageBotsIsBitThirteenAndNotADefault()
    {
        // The value itself. The client's mirror and the server's
        // protocol/include/bsfchat/Permissions.h must agree byte for byte or
        // every role on the server means something different on each side —
        // pinned here so a change to the mirror cannot land quietly.
        QCOMPARE(permmath::kManageBots, permmath::Flags(1ULL << 13));
        QCOMPARE(permmath::kManageBots, permmath::Flags(0x2000));

        // It must not overlap anything already assigned.
        constexpr permmath::Flags others =
            permmath::kViewChannel | permmath::kSendMessages |
            permmath::kAttachFiles | permmath::kEmbedLinks |
            permmath::kManageMessages | permmath::kManageChannels |
            permmath::kManageRoles | permmath::kKickMembers |
            permmath::kBanMembers | permmath::kMentionEveryone |
            permmath::kManageServer | permmath::kChangeNickname |
            permmath::kManageNicknames | permmath::kAdministrator;
        QCOMPARE(permmath::kManageBots & others, permmath::Flags(0));

        // In the "check all" mask, so a UI toggle that grants everything
        // grants this too...
        QVERIFY(grants(permmath::kAllFlags, permmath::kManageBots));
        // ...and emphatically not in @everyone's defaults.
        QVERIFY(!grants(permmath::kEveryoneDefault, permmath::kManageBots));
    }

    void thePermissionMirrorHasNotDriftedFromProtocol()
    {
        // The role-editor test below reconstructs the editor's mask and
        // compares it to permmath::kAllFlags. Both of those are client-side,
        // so together they prove the editor covers everything the MIRROR
        // knows about — and nothing at all about whether the mirror still
        // matches the server.
        //
        // That gap is not hypothetical. ADD_REACTIONS (bit 14) was added to
        // protocol's kAllFlags while this mirror still ended at bit 13, and
        // every client-side check stayed green: the editor was missing a
        // switch for a flag the server enforces, which is the exact failure
        // the editor test was written to catch, and it could not see it.
        //
        // So compare against the authority. permmath deliberately does not
        // include protocol's header — it carries the values so the permission
        // maths can be unit-tested without the protocol library — but this
        // test already links bsfchat_protocol and can hold the two side by
        // side. A new flag on the server now fails HERE first, and the editor
        // test fails immediately after it is mirrored, which is the order
        // that makes both messages readable.
        QCOMPARE(permmath::kAllFlags,
                 permmath::Flags(bsfchat::permission::kAllFlags));
        QCOMPARE(permmath::kEveryoneDefault,
                 permmath::Flags(bsfchat::permission::kEveryoneDefault));

        // Value-by-value, so a drift report names the bit rather than
        // printing two hex masks and leaving the reader to diff them.
        QCOMPARE(permmath::kViewChannel,     permmath::Flags(bsfchat::permission::kViewChannel));
        QCOMPARE(permmath::kSendMessages,    permmath::Flags(bsfchat::permission::kSendMessages));
        QCOMPARE(permmath::kAttachFiles,     permmath::Flags(bsfchat::permission::kAttachFiles));
        QCOMPARE(permmath::kEmbedLinks,      permmath::Flags(bsfchat::permission::kEmbedLinks));
        QCOMPARE(permmath::kManageMessages,  permmath::Flags(bsfchat::permission::kManageMessages));
        QCOMPARE(permmath::kManageChannels,  permmath::Flags(bsfchat::permission::kManageChannels));
        QCOMPARE(permmath::kManageRoles,     permmath::Flags(bsfchat::permission::kManageRoles));
        QCOMPARE(permmath::kKickMembers,     permmath::Flags(bsfchat::permission::kKickMembers));
        QCOMPARE(permmath::kBanMembers,      permmath::Flags(bsfchat::permission::kBanMembers));
        QCOMPARE(permmath::kMentionEveryone, permmath::Flags(bsfchat::permission::kMentionEveryone));
        QCOMPARE(permmath::kManageServer,    permmath::Flags(bsfchat::permission::kManageServer));
        QCOMPARE(permmath::kChangeNickname,  permmath::Flags(bsfchat::permission::kChangeNickname));
        QCOMPARE(permmath::kManageNicknames, permmath::Flags(bsfchat::permission::kManageNicknames));
        QCOMPARE(permmath::kManageBots,      permmath::Flags(bsfchat::permission::kManageBots));
        QCOMPARE(permmath::kAddReactions,    permmath::Flags(bsfchat::permission::kAddReactions));
        QCOMPARE(permmath::kAdministrator,   permmath::Flags(bsfchat::permission::kAdministrator));
    }

    void manageBotsIsAServerScopeGrant()
    {
        permmath::Role everyone{QStringLiteral("everyone"), 0,
                                permmath::kEveryoneDefault};
        permmath::Role botops{QStringLiteral("botops"), 5,
                              permmath::kViewChannel | permmath::kManageBots};
        const QVector<permmath::Role> roles{everyone, botops};
        const QString me = QStringLiteral("@josh:server");

        // Granted by a role, asked at server scope (null overrides).
        const auto withRole = permmath::effectivePermissions(
            roles, {QStringLiteral("everyone"), QStringLiteral("botops")}, me, nullptr);
        QVERIFY(grants(withRole, permmath::kManageBots));

        // Without the role, nothing.
        const auto without = permmath::effectivePermissions(
            roles, {QStringLiteral("everyone")}, me, nullptr);
        QVERIFY(!grants(without, permmath::kManageBots));

        // A per-CHANNEL override must not answer this server-scope question.
        // A bot belongs to the server; letting a channel override open the
        // management pane produces a dialog whose every request 403s, and on
        // the server side that same conflation was a privilege-escalation
        // hole (see effectivePermissions' own comment).
        QVector<permmath::Override> overrides;
        overrides.append({QStringLiteral("user:") + me, permmath::kManageBots, 0});
        const auto serverScope = permmath::effectivePermissions(
            roles, {QStringLiteral("everyone")}, me, nullptr);
        QVERIFY(!grants(serverScope, permmath::kManageBots));
    }

    void administratorImpliesManageBots()
    {
        // The dialog gates on canManageBots() alone rather than asking a
        // second "or admin?" question, which is only correct because the
        // permission maths short-circuits ADMINISTRATOR.
        permmath::Role admin{QStringLiteral("admin"), 10, permmath::kAdministrator};
        const auto flags = permmath::effectivePermissions(
            {admin}, {QStringLiteral("admin")}, QStringLiteral("@josh:server"), nullptr);
        QVERIFY(grants(flags, permmath::kManageBots));
    }

    // ─────────────────── 4. the dialog's view-model ───────────────────

    void refreshAsksTheServerAndMarksLoaded()
    {
        BotAdminModel model;
        RecordingHooks hooks;
        hooks.installOn(model);

        // Before the first reply, "empty" must be distinguishable from
        // "haven't asked" — the pane shows a spinner for one and an empty
        // state for the other, and they look identical from `bots`.
        QVERIFY(!model.loaded());

        model.refresh();
        QCOMPARE(hooks.calls, QStringList{QStringLiteral("list")});
        QVERIFY(model.busy());

        model.onBotsListed({});
        QVERIFY(!model.busy());
        QVERIFY(model.loaded());
        QVERIFY(model.bots().isEmpty());
    }

    void listRepliesBecomeRowsWithEveryContractField()
    {
        BotAdminModel model;
        RecordingHooks hooks;
        hooks.installOn(model);

        QJsonArray bots;
        bots.append(botEntry(QStringLiteral("@build:server"), QStringLiteral("Build Bot")));
        bots.append(botEntry(QStringLiteral("@old:server"), QStringLiteral("Old Bot"), true));
        model.onBotsListed(bots);

        QCOMPARE(model.bots().size(), 2);
        const QVariantMap first = model.bots().at(0).toMap();
        QCOMPARE(first.value("userId").toString(), QStringLiteral("@build:server"));
        QCOMPARE(first.value("displayName").toString(), QStringLiteral("Build Bot"));
        QCOMPARE(first.value("description").toString(), QStringLiteral("does a thing"));
        QCOMPARE(first.value("ownerId").toString(), QStringLiteral("@josh:server"));
        QCOMPARE(first.value("createdAt").toLongLong(), 1700000000000LL);
        QCOMPARE(first.value("lastSeenAt").toLongLong(), 1700000600000LL);
        QCOMPARE(first.value("deactivated").toBool(), false);
        QCOMPARE(model.bots().at(1).toMap().value("deactivated").toBool(), true);

        // A deactivated bot is still a bot: its old messages are still in the
        // timeline and must keep their badge.
        QCOMPARE(model.listedBotUserIds(),
                 QStringList({QStringLiteral("@build:server"), QStringLiteral("@old:server")}));

        QCOMPARE(model.displayNameFor(QStringLiteral("@build:server")),
                 QStringLiteral("Build Bot"));
        // Unknown id falls back to the id, which is always meaningful copy.
        QCOMPARE(model.displayNameFor(QStringLiteral("@who:server")),
                 QStringLiteral("@who:server"));
    }

    void aBadLocalpartIsRefusedWithoutARoundTrip()
    {
        BotAdminModel model;
        RecordingHooks hooks;
        hooks.installOn(model);

        // These two used to read `build-bot` and `bot.1_2=3/4+5` and expect
        // BOTH to be accepted. Neither can be created on any server: the
        // first has no `bot_` prefix and the second uses `=`, `/` and `+`,
        // none of which BotHandler::valid_bot_localpart allows. The old
        // expectations were not describing the server, they were describing
        // this function — which is how a validator drifts a whole character
        // set away from the thing it validates for and stays green.
        QVERIFY(BotAdminModel::localpartError(QStringLiteral("bot_build")).isEmpty());
        QVERIFY(BotAdminModel::localpartError(QStringLiteral("bot_1.2_3-4")).isEmpty());
        QVERIFY(!BotAdminModel::localpartError(QStringLiteral("build-bot")).isEmpty());
        QVERIFY(!BotAdminModel::localpartError(QStringLiteral("bot_1=2")).isEmpty());
        QVERIFY(!BotAdminModel::localpartError(QStringLiteral("bot_1/2")).isEmpty());
        QVERIFY(!BotAdminModel::localpartError(QStringLiteral("bot_1+2")).isEmpty());
        QVERIFY(!BotAdminModel::localpartError(QString()).isEmpty());
        QVERIFY(!BotAdminModel::localpartError(QStringLiteral("Build Bot")).isEmpty());
        QVERIFY(!BotAdminModel::localpartError(QStringLiteral("BuildBot")).isEmpty());
        // A colon would make a second one in @localpart:server and produce an
        // id for a different server entirely — called out separately because
        // the server's generic 400 does not explain that.
        QVERIFY(BotAdminModel::localpartError(QStringLiteral("bot:server"))
                    .contains(QStringLiteral("colon")));

        model.createBot(QStringLiteral("Bad Name"), QStringLiteral("x"), QString());
        QVERIFY(hooks.calls.isEmpty());
        QVERIFY(!model.errorText().isEmpty());
        QVERIFY(!model.busy());

        model.createBot(QStringLiteral("bot_build"), QStringLiteral("Build Bot"),
                        QStringLiteral("CI"));
        QCOMPARE(hooks.calls, QStringList{QStringLiteral("create")});
        QCOMPARE(hooks.lastLocalpart, QStringLiteral("bot_build"));
        QCOMPARE(hooks.lastDisplayName, QStringLiteral("Build Bot"));
        QCOMPARE(hooks.lastDescription, QStringLiteral("CI"));
        // The stale validation message must be gone once a valid attempt
        // starts, or the pane shows an error next to a request in flight.
        QVERIFY(model.errorText().isEmpty());
    }

    // ── The 2026-09-20 discoverability incident ───────────────────────────
    //
    // One support call, one SSH session and one database query, for two
    // questions the client could have answered on screen: "which account am I
    // signed in as", and "why is there no Server Settings gear". Three of the
    // four surfaces it touched are QML and are guarded by source scans below,
    // in the spirit of test_qml_hygiene.cpp; the rule the fourth one broke is
    // ordinary C++ and is checked directly.

    void theLocalpartRuleIsTheServersRuleAndNotAWiderOne()
    {
        // The prefix and the length come from PROTOCOL, not from a literal
        // here and not from BotAdminModel's own copy of either — the same
        // reasoning as thePermissionMirrorHasNotDriftedFromProtocol above. A
        // test that asked the client what its prefix was and then checked the
        // client enforced it would pass on any prefix, including one the
        // server has never heard of.
        const QString prefix = QString::fromUtf8(
            bsfchat::bot::kLocalpartPrefix.data(),
            qsizetype(bsfchat::bot::kLocalpartPrefix.size()));
        QCOMPARE(BotAdminModel::localpartPrefix(), prefix);
        QCOMPARE(BotAdminModel::maxLocalpartLength(),
                 int(bsfchat::limits::kMaxUsernameLength));

        // Missing prefix: refused, and the refusal must SAY the prefix. The
        // operator in the incident was told "invalid username" by nothing at
        // all — the field let him through and the server's 400 was the first
        // mention the prefix ever got.
        const QString missing = BotAdminModel::localpartError(
            QStringLiteral("tibiaguru"));
        QVERIFY(!missing.isEmpty());
        QVERIFY2(missing.contains(prefix),
                 qPrintable(QStringLiteral("prefix not named in: ") + missing));

        // The prefix alone is not a name — the server refuses it
        // (`localpart.size() <= kLocalpartPrefix.size()`), so the button must
        // not light up for it.
        QVERIFY(!BotAdminModel::localpartError(prefix).isEmpty());
        QVERIFY(BotAdminModel::localpartError(prefix + QStringLiteral("a")).isEmpty());

        // Length, at the boundary the server uses: total, prefix included.
        const int max = int(bsfchat::limits::kMaxUsernameLength);
        const QString tail(max - prefix.size(), QLatin1Char('a'));
        QVERIFY(BotAdminModel::localpartError(prefix + tail).isEmpty());
        QVERIFY(!BotAdminModel::localpartError(prefix + tail + QStringLiteral("a"))
                     .isEmpty());

        // The character set, exactly BotHandler::valid_bot_localpart's:
        // lowercase a–z, digits, and . _ - — nothing else. `=`, `/` and `+`
        // are the three this end used to wave through, and the three the
        // field's own error message used to advertise.
        for (QChar c : {QLatin1Char('.'), QLatin1Char('_'), QLatin1Char('-')}) {
            QVERIFY2(BotAdminModel::localpartError(prefix + QStringLiteral("a") + c
                                                   + QStringLiteral("b")).isEmpty(),
                     qPrintable(QStringLiteral("rejected a legal character: ") + c));
        }
        for (QChar c : {QLatin1Char('='), QLatin1Char('/'), QLatin1Char('+'),
                        QLatin1Char('@'), QLatin1Char(' '), QLatin1Char('A')}) {
            QVERIFY2(!BotAdminModel::localpartError(prefix + QStringLiteral("a") + c
                                                    + QStringLiteral("b")).isEmpty(),
                     qPrintable(QStringLiteral("accepted an illegal character: ") + c));
        }
    }

    void theCreateFieldPrefillsThePrefixRatherThanWaitingForA400()
    {
        const QString src = withoutComments(readAll(
            QStringLiteral(BSFCHAT_QML_DIR "/components/BotManagerPane.qml")));
        QVERIFY2(!src.isEmpty(), "BotManagerPane.qml not found");

        // The field must start at the prefix, and must get it from the model
        // (which reads the protocol header) rather than from a "bot_" literal
        // in QML — a literal is how a client keeps prefilling a prefix the
        // server has stopped requiring.
        QVERIFY2(src.contains(QStringLiteral("localpartPrefix")),
                 "the create field does not prefill the bot prefix");
        static const QRegularExpression literal(QStringLiteral("\"bot_\""));
        QVERIFY2(!literal.match(src).hasMatch(),
                 "BotManagerPane.qml hard-codes \"bot_\" instead of asking the "
                 "model for protocol's bot::kLocalpartPrefix");
    }

    void serverSettingsOpensForEveryPermissionItHasAPageFor()
    {
        // The gear's gate and the modal's nav list are one fact. They had
        // drifted: the gate asked for MANAGE_ROLES / MANAGE_CHANNELS / KICK /
        // BAN, while the modal also carries an Overview page (MANAGE_SERVER)
        // and a Bots page (MANAGE_BOTS). A member holding only one of those
        // two could not reach the modal at all — including, absurdly, the
        // "you need the Manage bots permission" state BotManagerPane renders
        // for exactly that member.
        for (permmath::Flags f : {permmath::kManageServer, permmath::kManageRoles,
                                  permmath::kManageChannels, permmath::kKickMembers,
                                  permmath::kBanMembers, permmath::kManageBots,
                                  permmath::kAdministrator}) {
            QVERIFY2(permmath::opensServerSettings(f),
                     qPrintable(QStringLiteral("a page exists for 0x%1 but it does "
                                               "not open the dialog").arg(f, 0, 16)));
        }

        // Every flag in the set is one protocol knows about. Checked against
        // the authority rather than against permmath::kAllFlags, which is the
        // client's own copy — comparing the set to a mirror of itself would
        // pass for a bit protocol never assigned.
        QCOMPARE(permmath::kServerSettingsPages
                     & ~permmath::Flags(bsfchat::permission::kAllFlags),
                 permmath::Flags(0));

        // And the incident itself: an account holding nothing but @everyone.
        // It opens the dialog onto the locked panel, so this is what decides
        // whether the gear is live — never whether it exists.
        QVERIFY(!permmath::opensServerSettings(permmath::kEveryoneDefault));
        QVERIFY(!permmath::opensServerSettings(0));
    }

    void theSettingsGearSurvivesHavingNoPermissions()
    {
        const QString src = withoutComments(readAll(
            QStringLiteral(BSFCHAT_QML_DIR "/components/ChannelList.qml")));
        QVERIFY2(!src.isEmpty(), "ChannelList.qml not found");

        // Anchored at end-of-identifier: a plain indexOf("id: settingsGear")
        // also matches "id: settingsGearMouse", which is the gear's OWN
        // MouseArea and sits where the Icon used to — so the guard passed
        // happily against the very file it was written to reject.
        static const QRegularExpression gearId(
            QStringLiteral(R"(id:\s*settingsGear\s*$)"),
            QRegularExpression::MultilineOption);
        const int gear = int(gearId.match(src).capturedStart());
        QVERIFY2(gear > 0, "the settings gear is missing or was renamed");
        // From the id to the gear's own MouseArea: that span is the Icon's
        // property block and nothing else, so the `visible` found in it is
        // the gear's and not some later item's.
        const int end = src.indexOf(QStringLiteral("MouseArea"), gear);
        QVERIFY2(end > gear, "the gear has no MouseArea — did it stop being clickable?");
        const QString block = src.mid(gear, end - gear);
        static const QRegularExpression vis(QStringLiteral("visible:([^\n]*)"));
        const auto m = vis.match(block);
        QVERIFY2(m.hasMatch(), "the gear has no visible binding to check");
        // It may depend on there BEING a server. It may not depend on what
        // that server lets you do: a gear that vanishes cannot distinguish
        // "no such feature" from "not for this account", and telling those
        // two apart cost a database inspection over SSH.
        QVERIFY2(!m.captured(1).contains(QStringLiteral("can")),
                 qPrintable(QStringLiteral("the gear is permission-gated again: ")
                            + m.captured(1)));
    }

    void theLockedServerSettingsPanelNamesTheAccount()
    {
        const QString src = withoutComments(readAll(
            QStringLiteral(BSFCHAT_QML_DIR "/components/ServerSettings.qml")));
        QVERIFY2(!src.isEmpty(), "ServerSettings.qml not found");

        QVERIFY2(src.contains(QStringLiteral("canOpenServerSettings")),
                 "ServerSettings.qml has no locked state");
        // The whole incident was two accounts of one person on one homeserver
        // with near-identical display names. A panel that named the missing
        // permission but not the ACCOUNT would have left him exactly where he
        // was, so the mxid is not decoration here.
        const int locked = src.indexOf(QStringLiteral("visible: !serverSettingsPopup._mayOpen"));
        QVERIFY2(locked > 0, "the locked panel is missing or was renamed");
        const QString panel = src.mid(locked, 3000);
        QVERIFY2(panel.contains(QStringLiteral(".userId")),
                 "the locked panel does not say which account is signed in");
    }

    void theUserMenuSaysWhichAccountYouAreSignedInAs()
    {
        const QString src = withoutComments(readAll(
            QStringLiteral(BSFCHAT_QML_DIR "/components/ChannelList.qml")));
        QVERIFY2(!src.isEmpty(), "ChannelList.qml not found");

        const int menu = src.indexOf(QStringLiteral("id: userMenu"));
        QVERIFY2(menu > 0, "the user menu is missing or was renamed");
        const QString block = src.mid(menu, 4000);
        QVERIFY2(block.contains(QStringLiteral(".userId")),
                 "the user menu does not show the signed-in mxid");
        // Un-elided, and that is the point rather than a style note: the
        // sidebar footer already renders the id and elides it to "@josh.oid…"
        // in a 240px column, which is exactly no help when the two accounts
        // you are choosing between differ after the tenth character.
        const int idLine = block.indexOf(QStringLiteral(".userId"));
        const QString around = block.mid(idLine, 400);
        QVERIFY2(!around.contains(QStringLiteral("Text.Elide")),
                 "the menu elides the mxid, which is the one thing it exists "
                 "to show in full");
    }

    void aCreatedTokenIsPresentedOnceAndSurvivesTheRefreshUnderneathIt()
    {
        BotAdminModel model;
        RecordingHooks hooks;
        hooks.installOn(model);
        QSignalSpy issued(&model, &BotAdminModel::tokenIssued);

        model.createBot(QStringLiteral("bot_build"), QStringLiteral("Build Bot"), QString());
        model.onBotCreated(QStringLiteral("@build:server"), QStringLiteral("Build Bot"),
                           QStringLiteral("syt_verysecret_token"));

        QVERIFY(model.hasIssuedToken());
        QCOMPARE(model.issuedToken(), QStringLiteral("syt_verysecret_token"));
        QCOMPARE(model.issuedTokenUserId(), QStringLiteral("@build:server"));
        QVERIFY(!model.issuedTokenIsRotation());
        QCOMPARE(issued.count(), 1);
        // The token is deliberately NOT a signal argument — it would then sit
        // in every queued-connection event and every QML handler's arguments.
        QCOMPARE(issued.at(0).at(0).toString(), QStringLiteral("@build:server"));
        QCOMPARE(issued.at(0).at(1).toBool(), false);

        // The bot appears immediately, so the pane is not empty behind the
        // banner while the authoritative list flies.
        QCOMPARE(model.bots().size(), 1);
        QVERIFY(hooks.calls.contains(QStringLiteral("list")));

        // THE ORDERING THAT DESTROYS CREDENTIALS: the re-list lands while the
        // operator is still reading the banner. It must not take the token
        // away — there is no second copy anywhere.
        QJsonArray bots;
        bots.append(botEntry(QStringLiteral("@build:server"), QStringLiteral("Build Bot")));
        model.onBotsListed(bots);
        QVERIFY(model.hasIssuedToken());
        QCOMPARE(model.issuedToken(), QStringLiteral("syt_verysecret_token"));

        // Nor may a second, unrelated failure clear it.
        model.onFailed(QStringLiteral("list"), QStringLiteral("boom"));
        QVERIFY(model.hasIssuedToken());

        // Only the explicit dismissal does.
        model.dismissToken();
        QVERIFY(!model.hasIssuedToken());
        QVERIFY(model.issuedToken().isEmpty());
        QVERIFY(model.issuedTokenUserId().isEmpty());
    }

    void theTokenIsNeverInTheRowData()
    {
        BotAdminModel model;
        RecordingHooks hooks;
        hooks.installOn(model);

        model.createBot(QStringLiteral("bot_build"), QStringLiteral("Build Bot"), QString());
        model.onBotCreated(QStringLiteral("@build:server"), QStringLiteral("Build Bot"),
                           QStringLiteral("syt_verysecret_token"));
        QJsonArray bots;
        bots.append(botEntry(QStringLiteral("@build:server"), QStringLiteral("Build Bot")));
        model.onBotsListed(bots);

        // `bots` is what QML iterates and what any future "export this list"
        // would serialise. A token that had leaked into a row would follow it
        // everywhere the list goes.
        for (const QVariant& entry : model.bots()) {
            const QVariantMap row = entry.toMap();
            for (auto it = row.constBegin(); it != row.constEnd(); ++it) {
                QVERIFY2(!it.value().toString().contains(QStringLiteral("syt_verysecret")),
                         qPrintable(QStringLiteral("token leaked into row key ") + it.key()));
                QVERIFY(!it.key().contains(QStringLiteral("token"), Qt::CaseInsensitive));
            }
        }
    }

    void aRotationSaysSoAndReplacesAnOpenBanner()
    {
        BotAdminModel model;
        RecordingHooks hooks;
        hooks.installOn(model);

        QJsonArray bots;
        bots.append(botEntry(QStringLiteral("@build:server"), QStringLiteral("Build Bot")));
        model.onBotsListed(bots);

        model.rotateToken(QStringLiteral("@build:server"));
        QVERIFY(hooks.calls.contains(QStringLiteral("rotate")));
        QCOMPARE(hooks.lastUserId, QStringLiteral("@build:server"));

        model.onTokenRotated(QStringLiteral("@build:server"), QStringLiteral("syt_rotated"));
        QVERIFY(model.hasIssuedToken());
        QCOMPARE(model.issuedToken(), QStringLiteral("syt_rotated"));
        // The copy differs between the two cases and an operator acts on the
        // difference: after a rotation something deployed is already broken.
        QVERIFY(model.issuedTokenIsRotation());
        QVERIFY(!model.busy());

        // A rotation changes nothing the list shows, so it must not kick off
        // a refresh that could race the banner.
        const int listsBefore = hooks.calls.count(QStringLiteral("list"));
        model.onTokenRotated(QStringLiteral("@build:server"), QStringLiteral("syt_again"));
        QCOMPARE(hooks.calls.count(QStringLiteral("list")), listsBefore);
        QCOMPARE(model.issuedToken(), QStringLiteral("syt_again"));
    }

    void deactivationMarksTheRowRatherThanRemovingIt()
    {
        BotAdminModel model;
        RecordingHooks hooks;
        hooks.installOn(model);

        QJsonArray bots;
        bots.append(botEntry(QStringLiteral("@build:server"), QStringLiteral("Build Bot")));
        bots.append(botEntry(QStringLiteral("@alerts:server"), QStringLiteral("Alerts")));
        model.onBotsListed(bots);

        model.deactivateBot(QStringLiteral("@build:server"));
        QCOMPARE(hooks.lastUserId, QStringLiteral("@build:server"));
        model.onBotDeactivated(QStringLiteral("@build:server"));

        // Still two rows. The server DEACTIVATES — the bot keeps its id, its
        // messages and its badge — so a row that vanished would promise a
        // deletion that did not happen.
        QCOMPARE(model.bots().size(), 2);
        QCOMPARE(model.bots().at(0).toMap().value("deactivated").toBool(), true);
        QCOMPARE(model.bots().at(1).toMap().value("deactivated").toBool(), false);
        QCOMPARE(model.listedBotUserIds().size(), 2);
    }

    void failuresAreNamedAndClearBusy()
    {
        BotAdminModel model;
        RecordingHooks hooks;
        hooks.installOn(model);

        model.refresh();
        QVERIFY(model.busy());
        model.onFailed(QStringLiteral("list"), QStringLiteral("You do not have permission"));

        QVERIFY(!model.busy());
        // The message must say which control failed — the pane shows one
        // error line for four different buttons.
        QVERIFY(model.errorText().contains(QStringLiteral("load bots")));
        QVERIFY(model.errorText().contains(QStringLiteral("You do not have permission")));
        // A failed list still counts as "asked", or the pane sits on a
        // spinner next to an error, which reads as still trying.
        QVERIFY(model.loaded());

        model.onFailed(QStringLiteral("rotate"), QString());
        QVERIFY(model.errorText().contains(QStringLiteral("rotate")));

        // A success wipes it.
        model.onBotsListed({});
        model.refresh();
        QVERIFY(model.errorText().isEmpty());
    }

    void resetDropsTheTokenWithEverythingElse()
    {
        BotAdminModel model;
        RecordingHooks hooks;
        hooks.installOn(model);

        model.onBotCreated(QStringLiteral("@build:server"), QStringLiteral("Build Bot"),
                           QStringLiteral("syt_secret"));
        QVERIFY(model.hasIssuedToken());

        // Closing the pane, or disconnecting. Reopening must not resurrect a
        // token the operator walked away from.
        model.reset();
        QVERIFY(!model.hasIssuedToken());
        QVERIFY(model.bots().isEmpty());
        QVERIFY(!model.loaded());
        QVERIFY(model.errorText().isEmpty());
        QVERIFY(!model.busy());
    }

    void actionsWithoutHooksAreNoOpsRatherThanCrashes()
    {
        // A pane opened against a disconnected server has no hooks installed.
        BotAdminModel model;
        model.refresh();
        model.createBot(QStringLiteral("bot_build"), QString(), QString());
        model.rotateToken(QStringLiteral("@build:server"));
        model.deactivateBot(QStringLiteral("@build:server"));
        // Nothing was attempted, so nothing is in flight.
        QVERIFY(!model.busy());
    }

    // ───────────────── 5. the token never reaches disk ─────────────────

    void nothingOnTheBotPathLogsOrPersistsTheToken()
    {
        // The identifiers that HOLD a bot token, anywhere in the client. If
        // one of these ever appears inside a logging call, the token is in
        // ~/Library/Logs/BSFChat/bsfchat.log — util/FileLogger.h mirrors
        // every qDebug/qInfo/qWarning/qCritical to a file, so there is no
        // level at which this is safe, and Settings > Advanced > verbose
        // logging turns the bsfchat.* categories on wholesale.
        const QStringList tokenBearers{
            QStringLiteral("issuedToken"), QStringLiteral("m_issuedToken"),
            QStringLiteral("botCreated"), QStringLiteral("botTokenRotated"),
            QStringLiteral("onBotCreated"), QStringLiteral("onTokenRotated"),
            QStringLiteral("rotateBotToken"), QStringLiteral("presentToken")
        };

        // One logging statement, up to its terminating semicolon. Matches the
        // Qt spellings and QML's console.*; deliberately greedy about which
        // call it considers a log and conservative about nothing else.
        static const QRegularExpression logCall(
            QStringLiteral(R"((qDebug|qInfo|qWarning|qCritical|qFatal|qCDebug|qCInfo|qCWarning|qCCritical|console\.(log|info|warn|error|debug))\s*\([^;]*;)"),
            QRegularExpression::DotMatchesEverythingOption);

        QStringList offenders;
        QStringList scanned;
        for (const QString& path : filesUnder(QStringLiteral(BSFCHAT_SRC_DIR), QStringLiteral("*.cpp"))
                                       + filesUnder(QStringLiteral(BSFCHAT_SRC_DIR), QStringLiteral("*.h"))
                                       + filesUnder(QStringLiteral(BSFCHAT_QML_DIR), QStringLiteral("*.qml"))) {
            const QString src = withoutComments(readAll(path));
            if (src.isEmpty()) continue;
            scanned << path;
            auto it = logCall.globalMatch(src);
            while (it.hasNext()) {
                const QString stmt = it.next().captured(0);
                for (const QString& bearer : tokenBearers) {
                    if (stmt.contains(bearer))
                        offenders << (path + QStringLiteral(" — ") + bearer);
                }
            }
        }
        // A scan that found nothing to scan is a scan that passes for the
        // wrong reason.
        QVERIFY2(scanned.size() > 40, "source scan found almost no files — "
                                      "check BSFCHAT_SRC_DIR / BSFCHAT_QML_DIR");
        QVERIFY2(offenders.isEmpty(), qPrintable(offenders.join(QStringLiteral("\n"))));
    }

    void theViewModelHasNoLoggingAndNoPersistenceAtAll()
    {
        // Two files carry the token: the view-model that holds it and the
        // pane that shows it. Rather than hunting for a bad log line in
        // them, the rule is simply that they contain no logging and no
        // settings writes whatsoever — a blanket ban is enforceable by
        // reading, which "log carefully" never is.
        const QStringList carriers{
            QStringLiteral(BSFCHAT_SRC_DIR "/model/BotAdminModel.cpp"),
            QStringLiteral(BSFCHAT_SRC_DIR "/model/BotAdminModel.h"),
            QStringLiteral(BSFCHAT_QML_DIR "/components/BotManagerPane.qml")
        };
        static const QRegularExpression anyLog(
            QStringLiteral(R"(\b(qDebug|qInfo|qWarning|qCritical|qFatal|qC(Debug|Info|Warning|Critical))\s*\(|console\s*\.\s*(log|info|warn|error|debug)\s*\()"));
        // QSettings is plain text on every platform we ship; LocalCache is an
        // on-disk SQLite database. Neither may ever see a bot token, so
        // neither belongs in these files at all.
        static const QRegularExpression anyPersist(
            QStringLiteral(R"(\bQSettings\b|\bsetValue\s*\(|\bLocalCache\b|\bwriteSettings\b)"));

        for (const QString& path : carriers) {
            const QString src = readAll(path);
            QVERIFY2(!src.isEmpty(), qPrintable(QStringLiteral("missing: ") + path));
            const QString code = withoutComments(src);
            QVERIFY2(!anyLog.match(code).hasMatch(),
                     qPrintable(QStringLiteral("logging call in token carrier: ") + path));
            QVERIFY2(!anyPersist.match(code).hasMatch(),
                     qPrintable(QStringLiteral("persistence in token carrier: ") + path));
        }
    }

    // ─────────── 6. the flag is actually reachable in the UI ───────────

    void everyEnforcedPermissionHasASwitchInTheRoleEditor()
    {
        // BSFChat has already shipped a permission that the server enforced
        // and no UI could grant — which is not "a missing checkbox", it is a
        // capability nobody on the server can ever be given, because the only
        // account that has it is one with ADMINISTRATOR.
        //
        // So this does not check for MANAGE_BOTS specifically. It reconstructs
        // the mask the role editor can produce from its own source text and
        // requires it to equal permmath::kAllFlags — the client's mirror of
        // every bit the server enforces. Adding a flag to the mirror without
        // adding a row to the editor fails here, for this flag and for the
        // next one.
        const QString src = readAll(
            QStringLiteral(BSFCHAT_QML_DIR "/components/ServerSettings.qml"));
        QVERIFY2(!src.isEmpty(), "ServerSettings.qml not found");

        const int start = src.indexOf(QStringLiteral("readonly property var permissionFlags:"));
        QVERIFY2(start > 0, "permissionFlags list not found — did it move or get renamed?");
        const int end = src.indexOf(QStringLiteral("\n    ]"), start);
        QVERIFY(end > start);
        const QString block = src.mid(start, end - start);

        static const QRegularExpression flagEntry(QStringLiteral(R"(flag:\s*0x([0-9a-fA-F]+))"));
        permmath::Flags uiMask = 0;
        int entries = 0;
        auto it = flagEntry.globalMatch(block);
        while (it.hasNext()) {
            bool ok = false;
            const auto value = it.next().captured(1).toULongLong(&ok, 16);
            QVERIFY(ok);
            uiMask |= value;
            ++entries;
        }
        QVERIFY2(entries >= 14, "suspiciously few permission rows parsed");

        // The specific flag this change adds, named so a failure says which.
        QVERIFY2(grants(uiMask, permmath::kManageBots),
                 "MANAGE_BOTS has no switch in the role editor — an admin "
                 "cannot grant it, so nobody but an Administrator can ever "
                 "manage bots");
        // And the general rule.
        QCOMPARE(uiMask, permmath::kAllFlags);
    }

    void theBotsPageIsWiredIntoServerSettings()
    {
        // The nav list and the StackLayout children are ONE ordering written
        // twice: the nav Repeater's `index` is the StackLayout index. A page
        // appended at the end while its nav row sits in the middle shows the
        // wrong tab for every entry after it, silently. Pin that the label
        // exists and that the pane it selects is instantiated.
        const QString src = withoutComments(readAll(
            QStringLiteral(BSFCHAT_QML_DIR "/components/ServerSettings.qml")));
        QVERIFY(!src.isEmpty());

        const int navAt = src.indexOf(QStringLiteral("\"Overview\", \"Roles\", \"Members\", \"Bots\""));
        QVERIFY2(navAt > 0, "the Bots nav row is missing or reordered");
        QVERIFY2(src.contains(QStringLiteral("BotManagerPane {")),
                 "the Bots nav row selects a page that does not instantiate "
                 "BotManagerPane");
    }

    void theBotEndpointsDoNotEchoSuccessBodies()
    {
        // MatrixClient's other endpoints hand a raw response body straight to
        // an error signal (see login / getLoginFlows: `QString::fromUtf8(data)`).
        // That is fine there and would be a credential leak on the two bot
        // calls whose SUCCESS body carries a token, because the same habit
        // applied one line up puts it in a toast and, from there, wherever
        // toasts are recorded. Pin that those two functions decode named
        // fields only.
        const QString src = withoutComments(
            readAll(QStringLiteral(BSFCHAT_SRC_DIR "/net/MatrixClient.cpp")));
        QVERIFY(!src.isEmpty());

        const int start = src.indexOf(QStringLiteral("void MatrixClient::createBot"));
        QVERIFY(start > 0);
        const int end = src.indexOf(QStringLiteral("void MatrixClient::deactivateBot"));
        QVERIFY(end > start);
        const QString botSection = src.mid(start, end - start);

        QVERIFY2(!botSection.contains(QStringLiteral("QString::fromUtf8(data)")),
                 "a bot endpoint echoes a raw response body");
        // The token must be read as a named field and nothing else.
        QVERIFY(botSection.contains(QStringLiteral("value(\"token\").toString()")));
    }
};

QTEST_MAIN(BotsTest)
#include "test_bots.moc"
