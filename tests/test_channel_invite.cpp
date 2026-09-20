// Adding a member — a person or a bot — to a channel from the client.
//
// Two halves, in the spirit of test_bots.cpp and test_self_roles.cpp:
//
//   1. ChannelInviteModel driven headlessly. It reaches the network through
//      std::function hooks, so every decision the dialog makes — what it
//      refuses before spending a request, what it says about each of the
//      seven different situations the server answers with an identical 403,
//      and what it claims happened on success — is exercised with a recording
//      fake and no event loop, no network and no GUI.
//
//   2. Source scans for what no headless test can reach: that the affordance
//      exists at all, that it is gated on the flag the server enforces rather
//      than on a number typed into a binding, and that the gate mutes the
//      control instead of deleting it.
//
// ───────────── the server's rules this pins against ─────────────
//
// server/src/api/RoomHandler.cpp handle_invite():
//
//   * MANAGE_CHANNELS, evaluated IN THE ROOM:
//         perms.can(*user_id, room_id, permission::kManageChannels)
//     under the comment "Inviting piggybacks on MANAGE_CHANNELS for now — we
//     don't have a separate flag."
//   * Seven distinct refusals, all 403 M_FORBIDDEN, distinguishable only by
//     their `error` text. The literals below are copied from that file and
//     are the reason explainFailure matches on substrings.
//   * The target must be a REAL ACCOUNT — user_exists(), added 2026-09-20 in
//     server b8e26ac. Ordered after MANAGE_CHANNELS and after both ban
//     checks, deliberately, so an ordinary member's refusal is byte-identical
//     for a real id and a fictional one and a pre-ban on an unregistered id
//     still reads "banned from this server". That ordering is why a caller
//     who lacks the permission never sees the new message, and it is pinned
//     server-side rather than guessed at here.
//   * A BOT target joins immediately: membership 'join', a real join event so
//     other members see it arrive, and an audit record naming the INVITER. A
//     repeat invite for a bot already in the room is a writeless 200.
//   * A HUMAN target is left at membership 'invite'.
//   * A target who is ALREADY JOINED is a writeless 200 whether they are a
//     human or a bot. Until server a19fd10 (fix/invite-no-demote) that was
//     true only of a bot: for a human it rewrote membership back to 'invite'
//     and demoted a member who was already in. The model used to refuse that
//     case locally off its cached roster, and
//     someoneAlreadyInTheChannelIsSentToTheServerWhichIsIdempotent below is
//     the test that used to assert the refusal and now asserts its absence.
//     The generic PUT .../state/m.room.member/{userId} route was fixed with
//     it, so there is no second way in.
//   * /join refuses anyone was_removed_by_moderator() recognises, and says so
//     — "cannot rejoin unless you are invited back". This endpoint is the
//     only thing that clears it.
//
// A SECOND gap closed on 2026-09-20 and this file moved with it. handle_invite
// used to skip user_exists() entirely, so an invite for a typo'd id was a 200
// and a membership row for nobody. The client could not see that, so it
// guessed at the commonest shape of the mistake — a warning when the typed
// id's homeserver was not ours. The server refuses the whole class now, so
// that guess is gone and theForeignHomeserverGuessIsGoneBecauseTheServer-
// AnswersItNow records why, including what the guess could never catch.
//
// ───────────── the drift hazard this deliberately avoids ─────────────
//
// A previous test in this suite, everyEnforcedPermissionHasASwitchInTheRoleEditor,
// compared the client's mask to the client's OWN kAllFlags. Both sides were
// client-side, so a permission the server enforces and the client had never
// heard of was structurally invisible to it. The gate test below therefore
// anchors on bsfchat::permission:: — protocol, which this binary links —
// exactly as thePermissionMirrorHasNotDriftedFromProtocol does, and the QML
// scan proves the client asks the mirror rather than restating a bit value.

#include <QtTest>
#include <QFile>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStringList>

#include "model/ChannelInviteModel.h"
#include "util/PermissionMath.h"

#include <bsfchat/Permissions.h>

namespace permmath = bsfchat::permmath;

namespace {

QString readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

// Comments only; string literals are left alone. Can only remove text, so a
// scan below may miss an offender but cannot invent one. Same helper and the
// same caveat as test_qml_hygiene and test_self_roles — and load-bearing
// here, because the files being scanned discuss canManageChannel at length in
// prose and a scan that counted those would pass on a file that had stopped
// calling it.
QString withoutComments(QString src)
{
    static const QRegularExpression block(QStringLiteral(R"(/\*.*?\*/)"),
                                          QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression line(QStringLiteral("//[^\n]*"));
    return src.remove(block).remove(line);
}

// The literals RoomHandler.cpp returns, verbatim. Kept in one place so the
// next person can diff them against the server rather than hunting them out
// of assertions.
namespace server_says {
constexpr const char* kNoPermission   = "Insufficient permissions to invite";
constexpr const char* kNotAMember     = "Not a member of this room";
constexpr const char* kIsADirect      = "Cannot invite someone into a direct message";
constexpr const char* kBannedRoom     = "User is banned from this room";
constexpr const char* kBannedServer   = "User is banned from this server";
constexpr const char* kBotDeactivated = "That bot is deactivated and cannot be added to a channel";
// Added 2026-09-20 with server b8e26ac. Pinned server-side too, by
// tests/e2e/mutate_phantom_membership.py, which mutates this exact literal and
// expects the mutation to be caught — so a reword upstream breaks a server test
// before it reaches this one.
constexpr const char* kNoSuchAccount   = "There is no account on this server with that id";
} // namespace server_says

// A recording fake for the model's network seam, plus a scriptable roster.
struct Fake {
    QStringList sentRooms;
    QStringList sentUsers;
    QMap<QString, QString> membership;   // "<room>|<user>" -> membership
    QStringList bots;
    QMap<QString, QString> displayNames;

    void install(ChannelInviteModel& m)
    {
        m.hooks.invite = [this](const QString& room, const QString& user) {
            sentRooms << room;
            sentUsers << user;
        };
        m.hooks.membershipOf = [this](const QString& room, const QString& user) {
            return membership.value(room + QStringLiteral("|") + user);
        };
        m.hooks.isKnownBot = [this](const QString& user) { return bots.contains(user); };
        m.hooks.displayNameOf = [this](const QString& user) {
            return displayNames.value(user);
        };
    }
};

} // namespace

class TestChannelInvite : public QObject {
    Q_OBJECT

private slots:

    // ── validation: structure only, checked before the request is spent ───
    //
    // This used to be described as the client's only defence against a server
    // that never checked the target exists. The server checks now; what is
    // left here is the part that never needed asking — see userIdError.
    // ──────────────────────────────────────────────────────────────────────

    void aMalformedIdIsRefusedWithoutSpendingARequest()
    {
        ChannelInviteModel m;
        Fake fake;
        fake.install(m);

        // Each of these has its own advice, because "invalid" is not advice.
        const QStringList bad{
            QStringLiteral("alice"),                 // no @
            QStringLiteral("@alice"),                // no server
            QStringLiteral("@:bsfchat.com"),         // no name
            QStringLiteral("@alice:"),               // no server after the colon
            QStringLiteral("@alice:a:b"),            // two colons
            QStringLiteral("@alice bsfchat.com"),    // whitespace
        };
        for (const QString& id : bad) {
            QVERIFY2(!ChannelInviteModel::userIdError(id).isEmpty(),
                     qPrintable(QStringLiteral("accepted a malformed id: ") + id));
            m.invite(QStringLiteral("!r:bsfchat.com"), id, QStringLiteral("general"));
            QVERIFY(!m.errorText().isEmpty());
        }
        // Nothing reached the wire. This matters more than the message: the
        // server would have answered 200 to most of these and written a
        // membership row for an account that does not exist.
        QCOMPARE(fake.sentUsers.size(), 0);

        // And a well-formed one is not second-guessed. The client validates
        // structure only; the server owns the grammar.
        QVERIFY(ChannelInviteModel::userIdError(
                    QStringLiteral("@a.b_c-d:bsfchat.com")).isEmpty());
        QVERIFY(ChannelInviteModel::userIdError(QString()).isEmpty());
    }

    void theForeignHomeserverGuessIsGoneBecauseTheServerAnswersItNow()
    {
        // WHAT THIS REPLACED, because the deletion is the point.
        //
        // Until server b8e26ac, handle_invite never called user_exists() and
        // answered 200 for an id nobody held. The client could not be told
        // "no such account", so it guessed the commonest shape of that
        // mistake — homeserverWarning(), which cautioned when the typed id's
        // domain was not the one we are signed in to, on the reasoning that
        // this deployment does not federate. A guess: it could not see a
        // typo'd LOCALPART at all, and it fired on a perfectly good id the
        // moment federation arrived.
        //
        // The server now refuses every one of those cases with one 403, so
        // the guess is dead code and its whole job belongs to the branch
        // below. A foreign id is not special any more — it is simply an id
        // with no account here, which is what the server says about it.
        ChannelInviteModel m;
        Fake fake;
        fake.install(m);

        const QString foreign = QStringLiteral("@alice:example.org");

        // Still not blocked client-side, for the same reason the warning
        // never blocked: refusing here would be the client deciding what
        // exists, and only the server knows. It goes, and the server answers.
        m.invite(QStringLiteral("!r:bsfchat.com"), foreign, QStringLiteral("general"));
        QCOMPARE(fake.sentUsers.size(), 1);
        QCOMPARE(fake.sentUsers.first(), foreign);

        // And THAT is the answer it gets — the same one a typo'd localpart
        // gets, which the guess could never have produced.
        m.onFailed(foreign, QStringLiteral("general"), 403,
                   QString::fromUtf8(server_says::kNoSuchAccount));
        QVERIFY(m.errorText().contains(QStringLiteral("no account")));
        QVERIFY(m.errorText().contains(foreign));

        // The dialog no longer carries the guess either. A binding left
        // calling a function nobody kept would not compile, but a binding
        // left rendering a stale warn-coloured line under the error line
        // would — so the QML is scanned, in the idiom the rest of this file
        // uses for what a headless test cannot reach.
        const QString dialog = withoutComments(
            readAll(QStringLiteral(BSFCHAT_QML_DIR "/components/AddMemberDialog.qml")));
        QVERIFY2(!dialog.isEmpty(), "AddMemberDialog.qml not readable");
        for (const char* gone : {"warnAboutHomeserver", "homeserverNote"}) {
            QVERIFY2(!dialog.contains(QLatin1String(gone)),
                     qPrintable(QStringLiteral("AddMemberDialog.qml still has ")
                                + QLatin1String(gone)));
        }
    }

    void someoneAlreadyInTheChannelIsSentToTheServerWhichIsIdempotent()
    {
        // This test used to assert the opposite, under the name
        // someoneAlreadyInTheChannelIsRefusedHereBecauseTheServerWillNot: the
        // model read the cached roster and refused a target it had seen as
        // 'join', because handle_invite would otherwise rewrite that 'join'
        // back to 'invite' — demoting a member and taking them out of
        // everyone else's roster until they joined again.
        //
        // Server a19fd10 (fix/invite-no-demote) makes an already-joined
        // target a writeless 200 for a human, the same answer the bot path
        // always gave, and does the same for the generic
        // PUT .../state/m.room.member/{userId} route. So the refusal went,
        // and the property is now the reverse: the request always goes to the
        // server.
        //
        // Do not bring the refusal back. It was never a real guard — the
        // roster is a cache, a miss fell straight through to the bug it was
        // protecting against — so all it decided was whether the same click
        // said "already in" or reported success, depending on what the client
        // had happened to see. Deleting it is safe because noticeFor()'s
        // non-bot copy names no outcome, so it reads true for a no-op too.
        ChannelInviteModel m;
        Fake fake;
        const QString room = QStringLiteral("!general:bsfchat.com");
        const QString bob  = QStringLiteral("@bob:bsfchat.com");
        fake.membership[room + QStringLiteral("|") + bob] = QStringLiteral("join");
        fake.displayNames[bob] = QStringLiteral("Bob");
        fake.install(m);

        m.invite(room, bob, QStringLiteral("general"));

        QCOMPARE(fake.sentUsers.size(), 1);
        QCOMPARE(fake.sentUsers.first(), bob);
        QCOMPARE(fake.sentRooms.first(), room);
        QVERIFY(m.errorText().isEmpty());
        QVERIFY(m.busy());

        // And the success it reports afterwards claims nothing that a no-op
        // would make untrue — no "they will see the invite" for someone who
        // is standing in the channel already.
        m.onInvited(bob, QStringLiteral("general"));
        QVERIFY(m.errorText().isEmpty());
        QVERIFY(m.noticeText().contains(QStringLiteral("Bob")));
        QVERIFY2(!m.noticeText().contains(QStringLiteral("sees the invite")),
                 qPrintable(m.noticeText()));

        // A membership the client has NOT seen behaves identically; there is
        // no longer any path where the cache changes what happens.
        m.invite(room, QStringLiteral("@carol:bsfchat.com"), QStringLiteral("general"));
        QCOMPARE(fake.sentUsers.size(), 2);
        QVERIFY(m.errorText().isEmpty());
    }

    // ── the readmit case: the one that looks like an error and is the
    //    feature ─────────────────────────────────────────────────────────────

    void someoneWhoLeftOrWasRemovedReadsAsReadmitNotAsAnError()
    {
        ChannelInviteModel m;
        Fake fake;
        const QString room = QStringLiteral("!general:bsfchat.com");
        const QString dan  = QStringLiteral("@dan:bsfchat.com");
        fake.membership[room + QStringLiteral("|") + dan] = QStringLiteral("leave");
        fake.install(m);

        const QString hint = m.readmitHint(room, dan, QStringLiteral("general"));
        QVERIFY(!hint.isEmpty());
        // Says what adding them DOES, and says this is the way back in for
        // someone a moderator removed — which /join refuses with
        // "cannot rejoin unless you are invited back" and nothing else clears.
        QVERIFY(hint.contains(QStringLiteral("back in")));
        QVERIFY(hint.contains(QStringLiteral("moderator")));

        // It is a hint, not an error: the invite is sent and the error line
        // stays empty.
        m.invite(room, dan, QStringLiteral("general"));
        QCOMPARE(fake.sentUsers.size(), 1);
        QVERIFY(m.errorText().isEmpty());

        // Nothing to say for a member who is present, or for one we have
        // never seen, or for a banned one — a ban has its own remedy and
        // offering to add them would be offering something that cannot work.
        QVERIFY(m.readmitHint(room, QStringLiteral("@eve:bsfchat.com"),
                              QStringLiteral("general")).isEmpty());
        fake.membership[room + QStringLiteral("|@f:bsfchat.com")] = QStringLiteral("ban");
        QVERIFY(m.readmitHint(room, QStringLiteral("@f:bsfchat.com"),
                              QStringLiteral("general")).isEmpty());
    }

    // ── the bot rule, which is the surprising one ─────────────────────────

    void theBotRuleIsStatedBeforeTheClick()
    {
        ChannelInviteModel m;
        const QString advisory = m.botAdvisory();
        QVERIFY(!advisory.isEmpty());
        QVERIFY(advisory.contains(QStringLiteral("bot"), Qt::CaseInsensitive));
        // The two halves that differ from what a Discord user expects: it is
        // immediate, and there is no invite sitting there to be accepted.
        QVERIFY(advisory.contains(QStringLiteral("straight away")));
        QVERIFY(advisory.contains(QStringLiteral("no pending invite")));
    }

    void aKnownBotSuccessSaysItHasAlreadyJoined()
    {
        ChannelInviteModel m;
        Fake fake;
        const QString bot = QStringLiteral("@bot_deploy:bsfchat.com");
        fake.bots << bot;
        fake.displayNames[bot] = QStringLiteral("Deploy Bot");
        fake.install(m);

        m.invite(QStringLiteral("!general:bsfchat.com"), bot, QStringLiteral("general"));
        QVERIFY(m.busy());
        m.onInvited(bot, QStringLiteral("general"));

        QVERIFY(!m.busy());
        QVERIFY(m.errorText().isEmpty());
        QVERIFY(m.noticeText().contains(QStringLiteral("Deploy Bot")));
        QVERIFY(m.noticeText().contains(QStringLiteral("#general")));
        QVERIFY(m.noticeText().contains(QStringLiteral("joined")));
        QVERIFY(m.noticeText().contains(QStringLiteral("no invite to accept")));
    }

    void anUnknownTargetSuccessNamesNoOutcomeBecauseThreeArePossible()
    {
        // The client learns bot-ness from member events and from the bot
        // admin list. That list is gated on MANAGE_BOTS while this dialog is
        // gated on MANAGE_CHANNELS, and neither implies the other — so an
        // operator can legitimately add a bot the client has never heard of.
        // The server replies `{}` either way and cannot be asked which it did.
        //
        // This used to name both outcomes ("a person sees the invite next
        // time they connect; a bot is already in the channel"). There are
        // three now: since the local already-joined refusal went, a target
        // who is already a member reaches here too and the server writes
        // nothing for them. So the copy names none of them.
        ChannelInviteModel m;
        Fake fake;
        fake.install(m);
        const QString who = QStringLiteral("@mystery:bsfchat.com");

        m.invite(QStringLiteral("!general:bsfchat.com"), who, QStringLiteral("general"));
        m.onInvited(who, QStringLiteral("general"));

        QVERIFY(m.noticeText().contains(who));
        QVERIFY(m.noticeText().contains(QStringLiteral("#general")));
        // Nothing about what happens next, because whatever it said would be
        // false for one of the three.
        for (const char* claim : {"sees the invite", "next time they connect",
                                  "already in the channel"}) {
            QVERIFY2(!m.noticeText().contains(QLatin1String(claim)),
                     qPrintable(m.noticeText()));
        }
    }

    void aSuccessAnnouncesTheMemberSoTheRosterCanCatchUp()
    {
        ChannelInviteModel m;
        Fake fake;
        fake.install(m);
        QSignalSpy added(&m, &ChannelInviteModel::memberAdded);

        const QString room = QStringLiteral("!general:bsfchat.com");
        const QString bot  = QStringLiteral("@bot_deploy:bsfchat.com");
        m.invite(room, bot, QStringLiteral("general"));
        m.onInvited(bot, QStringLiteral("general"));

        QCOMPARE(added.size(), 1);
        QCOMPARE(added.first().at(0).toString(), room);
        QCOMPARE(added.first().at(1).toString(), bot);

        // A failure announces nothing. Badging a member who was refused
        // would put a phantom in the roster.
        m.invite(room, QStringLiteral("@x:bsfchat.com"), QStringLiteral("general"));
        m.onFailed(QStringLiteral("@x:bsfchat.com"), QStringLiteral("general"),
                   403, QString::fromUtf8(server_says::kBannedRoom));
        QCOMPARE(added.size(), 1);
    }

    // ── seven identical 403s, seven different things to do about them ─────

    void eachForbiddenReasonGetsItsOwnAdvice()
    {
        const QString who  = QStringLiteral("@bob:bsfchat.com");
        const QString room = QStringLiteral("general");
        const auto explain = [&](const char* msg) {
            return ChannelInviteModel::explainFailure(
                who, room, 403, QString::fromUtf8(msg));
        };

        // The permission, named, with where to grant it.
        const QString perm = explain(server_says::kNoPermission);
        QVERIFY(perm.contains(QStringLiteral("Manage Channels")));
        QVERIFY(perm.contains(QStringLiteral("#general")));

        // Not in the channel yourself.
        const QString notIn = explain(server_says::kNotAMember);
        QVERIFY(notIn.contains(QStringLiteral("not in")));
        QVERIFY(!notIn.contains(QStringLiteral("Manage Channels")));

        // A DM cannot take a third person — and the advice is the way round
        // it, not a restatement of the rule.
        const QString dm = explain(server_says::kIsADirect);
        QVERIFY(dm.contains(QStringLiteral("direct message")));
        QVERIFY(dm.contains(QStringLiteral("channel")));

        // The two bans differ in scope and therefore in remedy: one channel
        // versus every channel. Telling them apart is the point.
        const QString roomBan = explain(server_says::kBannedRoom);
        QVERIFY(roomBan.contains(QStringLiteral("banned from #general")));
        QVERIFY(roomBan.contains(QStringLiteral("Bans")));

        const QString serverBan = explain(server_says::kBannedServer);
        QVERIFY(serverBan.contains(QStringLiteral("banned from this server")));
        QVERIFY(serverBan.contains(QStringLiteral("every channel")));

        // A deactivated bot is permanent — so the advice is "make a new one",
        // not "try again".
        const QString dead = explain(server_says::kBotDeactivated);
        QVERIFY(dead.contains(QStringLiteral("deactivated")));
        QVERIFY(dead.contains(QStringLiteral("permanent")));

        // No such account — the one the server could not say until b8e26ac,
        // and the one that used to arrive as a cheerful success. The advice
        // is "you typed it wrong", because after the ban and permission
        // checks above it there is nothing else it can be.
        const QString absent = explain(server_says::kNoSuchAccount);
        QVERIFY(absent.contains(QStringLiteral("no account")));
        QVERIFY(absent.contains(who));
        QVERIFY(absent.contains(QStringLiteral("spelling")));
        // Not blamed on the channel. This refusal is about the id and would
        // be identical in every channel on the server, so naming one would
        // suggest a different channel might have worked — the same reasoning
        // the 400 branch is built on.
        QVERIFY(!absent.contains(QStringLiteral("#general")));
        // And it still parses when the id is missing. This is the one branch
        // whose sentence puts the id in a slot where explainFailure's "that
        // member" fallback would be ungrammatical, so it has its own wording
        // for that case rather than a noun phrase in a verb's place.
        const QString anon = ChannelInviteModel::explainFailure(
            QString(), room, 403, QString::fromUtf8(server_says::kNoSuchAccount));
        QVERIFY(anon.contains(QStringLiteral("with that id")));
        QVERIFY(!anon.contains(QStringLiteral("account that member")));

        // All seven say something different. A mapping that collapsed two of
        // them would be worse than no mapping, because it would look right.
        const QStringList all{perm, notIn, dm, roomBan, serverBan, dead, absent};
        QCOMPARE(QSet<QString>(all.begin(), all.end()).size(), all.size());
    }

    void noRefusalIsCapturedByAnotherRefusalsSubstring()
    {
        // explainFailure tells seven identical-looking 403s apart by matching
        // SUBSTRINGS of the server's `error` text, first match wins. That is
        // a mapping which can silently go wrong in exactly one way: a new
        // fragment matching an older message, or an older fragment matching a
        // newer message, so one refusal is answered with another's advice.
        // The reader would see a confident, wrong sentence.
        //
        // Checked BEHAVIOURALLY rather than by re-listing the fragments here.
        // Restating them would be the everyEnforcedPermissionHasASwitchInThe-
        // RoleEditor mistake in miniature: both sides of the comparison drawn
        // from the client, so a fragment that stopped matching would still
        // "agree" with its copy. Feeding the SERVER's strings in and looking
        // at what comes out tests the mapping itself.
        const QString who  = QStringLiteral("@bob:bsfchat.com");
        const QString room = QStringLiteral("general");
        const QList<const char*> refusals{
            server_says::kNoPermission,   server_says::kNotAMember,
            server_says::kIsADirect,      server_says::kBannedRoom,
            server_says::kBannedServer,   server_says::kBotDeactivated,
            server_says::kNoSuchAccount,
        };

        QStringList advice;
        for (const char* raw : refusals) {
            const QString text = QString::fromUtf8(raw);
            const QString msg  = ChannelInviteModel::explainFailure(who, room, 403, text);
            // RECOGNISED. The fallback branch appends the server's sentence
            // verbatim, so echoing it back is exactly the signature of a
            // refusal that matched nothing — which is how this test sees a
            // fragment that has drifted out of agreement with the server.
            QVERIFY2(!msg.contains(text),
                     qPrintable(QStringLiteral("fell through to the server's words: ") + text));
            advice << msg;
        }
        // DISTINCT. `who` and `room` are held constant, so two refusals
        // landing in the same branch produce byte-identical advice — which
        // covers both collision directions at once.
        QCOMPARE(QSet<QString>(advice.begin(), advice.end()).size(), refusals.size());
    }

    void aTransportFailureDoesNotBlameTheId()
    {
        // status 0 is "no HTTP answer" — DNS, TLS, a dropped connection.
        // Nothing about the invite is wrong and the message must not imply it.
        const QString msg = ChannelInviteModel::explainFailure(
            QStringLiteral("@bob:bsfchat.com"), QStringLiteral("general"), 0,
            QStringLiteral("Connection refused"));
        QVERIFY(msg.contains(QStringLiteral("reach the server")));
        QVERIFY(msg.contains(QStringLiteral("nothing was added")));
        QVERIFY(!msg.contains(QStringLiteral("permission")));
        QVERIFY(!msg.contains(QStringLiteral("banned")));
    }

    void anUnrecognisedFailureKeepsTheServersOwnWords()
    {
        // The six branches match on substrings of the server's text, because
        // the errcode is M_FORBIDDEN for all of them. Reword one upstream and
        // this is where it lands: worse copy, never a wrong claim, and never
        // a swallowed error.
        const QString odd = ChannelInviteModel::explainFailure(
            QStringLiteral("@bob:bsfchat.com"), QStringLiteral("general"), 403,
            QStringLiteral("Some future refusal nobody has written yet"));
        QVERIFY(odd.contains(QStringLiteral("Some future refusal nobody has written yet")));

        // A NEAR MISS to the newest fragment, which is the loosest of the
        // seven: "no account" is two common words, so a 403 that merely talks
        // about accounts must not be mis-attributed to it. This one shares
        // "account" and "server" with kNoSuchAccount and matches neither.
        const QString near = ChannelInviteModel::explainFailure(
            QStringLiteral("@bob:bsfchat.com"), QStringLiteral("general"), 403,
            QStringLiteral("That account is administered by another server"));
        QVERIFY(near.contains(QStringLiteral("administered by another server")));
        QVERIFY(!near.contains(QStringLiteral("spelling")));

        // 400 and 404 have their own shapes and neither is a 403.
        const QString bad = ChannelInviteModel::explainFailure(
            QStringLiteral("@nonsense"), QStringLiteral("general"), 400,
            QStringLiteral("Missing user_id"));
        QVERIFY(bad.contains(QStringLiteral("@alice:bsfchat.com")));
        const QString gone = ChannelInviteModel::explainFailure(
            QStringLiteral("@bob:bsfchat.com"), QStringLiteral("general"), 404,
            QStringLiteral("Not found"));
        QVERIFY(gone.contains(QStringLiteral("no longer exists")));
    }

    void aMissingChannelNameNeverProducesAHashWithNothingAfterIt()
    {
        // A reply can land after a channel switch, so the name can be gone by
        // the time the message is built. "#" with nothing after it would read
        // as a rendering bug — which is what the reader would then report
        // instead of the actual failure.
        for (int status : {0, 400, 403, 404, 500}) {
            const QString msg = ChannelInviteModel::explainFailure(
                QStringLiteral("@bob:bsfchat.com"), QString(), status,
                QStringLiteral("whatever"));
            QVERIFY2(!msg.contains(QStringLiteral("#")), qPrintable(msg));
        }
        // The ones that DO refer to a place say "this channel" rather than
        // trailing off. 400 and 0 are not among them on purpose: "that is not
        // a member id" is true of every channel, and "we never reached the
        // server" is not about a channel at all — naming one in either would
        // suggest a different channel might have worked.
        for (int status : {403, 404, 500}) {
            const QString msg = ChannelInviteModel::explainFailure(
                QStringLiteral("@bob:bsfchat.com"), QString(), status,
                QStringLiteral("whatever"));
            QVERIFY2(msg.contains(QStringLiteral("this channel")), qPrintable(msg));
        }
    }

    void theBusyFlagAndTheTwoLinesNeverContradictEachOther()
    {
        ChannelInviteModel m;
        Fake fake;
        fake.install(m);
        const QString room = QStringLiteral("!general:bsfchat.com");
        const QString bob  = QStringLiteral("@bob:bsfchat.com");

        QVERIFY(!m.busy());
        m.invite(room, bob, QStringLiteral("general"));
        QVERIFY(m.busy());
        // Nothing is claimed while it is in flight.
        QVERIFY(m.noticeText().isEmpty());
        QVERIFY(m.errorText().isEmpty());

        m.onFailed(bob, QStringLiteral("general"), 403,
                   QString::fromUtf8(server_says::kNoPermission));
        QVERIFY(!m.busy());
        QVERIFY(!m.errorText().isEmpty());
        QVERIFY(m.noticeText().isEmpty());

        // A success clears the failure, and vice versa — the two lines are
        // shown together and a stale one beside a fresh one is a lie.
        m.invite(room, bob, QStringLiteral("general"));
        m.onInvited(bob, QStringLiteral("general"));
        QVERIFY(m.errorText().isEmpty());
        QVERIFY(!m.noticeText().isEmpty());

        m.reset();
        QVERIFY(m.errorText().isEmpty());
        QVERIFY(m.noticeText().isEmpty());
    }

    void aDisconnectedServerSaysSoRatherThanSilentlyDoingNothing()
    {
        // Hooks unset is what a dialog opened against a dead connection sees.
        ChannelInviteModel m;
        m.invite(QStringLiteral("!general:bsfchat.com"),
                 QStringLiteral("@bob:bsfchat.com"), QStringLiteral("general"));
        QVERIFY(m.errorText().contains(QStringLiteral("Not connected")));

        // And no channel is no channel, not a request to an empty room id.
        ChannelInviteModel m2;
        Fake fake;
        fake.install(m2);
        m2.invite(QString(), QStringLiteral("@bob:bsfchat.com"), QString());
        QCOMPARE(fake.sentUsers.size(), 0);
        QVERIFY(!m2.errorText().isEmpty());
    }

    // ── the gate ──────────────────────────────────────────────────────────

    void theGateIsTheFlagProtocolDefinesNotOneTypedIntoTheClient()
    {
        // Anchored on protocol, NOT on the client's own kAllFlags. That is
        // the whole lesson of everyEnforcedPermissionHasASwitchInTheRoleEditor,
        // which compared the client to itself and could not see drift by
        // construction.
        //
        // handle_invite asks for permission::kManageChannels. If protocol
        // renumbers it, this fails here rather than in a 403 an operator has
        // to report.
        QCOMPARE(permmath::kManageChannels,
                 permmath::Flags(bsfchat::permission::kManageChannels));

        // The gate has to mean something: a flag @everyone holds by default
        // would gate nothing at all, and a control that is always unlocked
        // would look identical in every screenshot.
        QVERIFY((permmath::Flags(bsfchat::permission::kEveryoneDefault)
                 & permmath::Flags(bsfchat::permission::kManageChannels)) == 0);

        // And it is a real bit in the set the server enforces, rather than a
        // client-side extra.
        QVERIFY((permmath::Flags(bsfchat::permission::kAllFlags)
                 & permmath::Flags(bsfchat::permission::kManageChannels)) != 0);
    }

    void theDialogAsksTheMirrorRatherThanRestatingABitValue()
    {
        const QString qml = withoutComments(
            readAll(QStringLiteral(BSFCHAT_QML_DIR "/components/AddMemberDialog.qml")));
        QVERIFY2(!qml.isEmpty(), "AddMemberDialog.qml not found");

        // The mirror, through the accessor that resolves overrides — and with
        // the ROOM, because a per-channel override grants MANAGE_CHANNELS in
        // one channel and not the next.
        QVERIFY(qml.contains(QStringLiteral("canManageChannel(addMemberDialog.roomId)")));

        // No hardcoded flag arithmetic anywhere in the dialog. This is the
        // shape the hazard takes in QML: a `0x0020` in a binding is a number
        // no test can compare against protocol.
        static const QRegularExpression hexBit(QStringLiteral("0x[0-9a-fA-F]{2,}"));
        QVERIFY2(!hexBit.match(qml).hasMatch(),
                 "AddMemberDialog.qml contains a hardcoded permission bit");
        QVERIFY(!qml.contains(QStringLiteral("1 <<")));

        // Re-evaluated on the permission tick, so a role edit unlocks it
        // without a reconnect.
        QVERIFY(qml.contains(QStringLiteral("permissionsGeneration")));
    }

    void theControlStaysPresentAndExplainsInsteadOfVanishing()
    {
        // The precedent BotManagerPane.qml set and that
        // fix/identity-permission-discoverability applied to the Server
        // Settings gear. A control that disappears says "this product cannot
        // do that"; the person in front of it needs "this ACCOUNT may not".
        const QString dialog = withoutComments(
            readAll(QStringLiteral(BSFCHAT_QML_DIR "/components/AddMemberDialog.qml")));
        QVERIFY(dialog.contains(QStringLiteral("InfoBanner")));
        QVERIFY(dialog.contains(QStringLiteral("Manage Channels")));
        // Names the signed-in account, which is the thing the 2026-09-20
        // incident turned on.
        QVERIFY(dialog.contains(QStringLiteral("userId")));

        const QString memberList = withoutComments(
            readAll(QStringLiteral(BSFCHAT_QML_DIR "/components/MemberList.qml")));
        QVERIFY2(!memberList.isEmpty(), "MemberList.qml not found");

        // The + exists, is gated, and — the point of this test — its
        // VISIBILITY does not depend on the gate. Scoped to the icon's own
        // block so this cannot be satisfied by some unrelated `visible:`
        // elsewhere in a 500-line file.
        const int start = memberList.indexOf(QStringLiteral("id: addMemberIcon"));
        QVERIFY2(start >= 0, "the member-list + is gone");
        const int end = memberList.indexOf(QStringLiteral("MouseArea"), start);
        QVERIFY(end > start);
        const QString block = memberList.mid(start, end - start);

        QVERIFY(block.contains(QStringLiteral("canManageChannel")));
        // The gate drives the COLOUR...
        QVERIFY(block.contains(QStringLiteral("unlocked")));
        // ...and `visible` is about whether there is a channel at all.
        static const QRegularExpression visibleLine(QStringLiteral("visible:[^\n]*"));
        const auto vis = visibleLine.match(block);
        QVERIFY2(vis.hasMatch(), "the + has no visible binding to check");
        QVERIFY2(!vis.captured(0).contains(QStringLiteral("unlocked")),
                 "the member-list + hides itself when the permission is missing");
        QVERIFY2(!vis.captured(0).contains(QStringLiteral("canManage")),
                 "the member-list + hides itself when the permission is missing");
    }

    void theAffordanceIsReachableFromBothPlacesAPersonWouldLook()
    {
        // The feature this replaces was "SSH into the host and read the room
        // id out of SQLite". Whether it is reachable is not a detail.
        const QString memberList = withoutComments(
            readAll(QStringLiteral(BSFCHAT_QML_DIR "/components/MemberList.qml")));
        QVERIFY(memberList.contains(QStringLiteral("AddMemberDialog")));
        QVERIFY(memberList.contains(QStringLiteral("addMemberDialog.open()")));

        // And from the channel's own right-click menu, where Discord puts
        // "Invite People" — on the channel that was clicked, not on whichever
        // one happens to be active.
        const QString channelList = withoutComments(
            readAll(QStringLiteral(BSFCHAT_QML_DIR "/components/ChannelList.qml")));
        QVERIFY(channelList.contains(QStringLiteral("AddMemberDialog")));
        QVERIFY(channelList.contains(QStringLiteral("Add member")));
        QVERIFY(channelList.contains(
            QStringLiteral("addMemberFromMenu.roomId = roomContextMenu.roomId")));
        QVERIFY(channelList.contains(
            QStringLiteral("canManageChannel(roomContextMenu.roomId)")));

        // The dialog says the bot rule, by asking the model for it rather
        // than by restating it in a binding where nothing can read it back.
        const QString dialog = withoutComments(
            readAll(QStringLiteral(BSFCHAT_QML_DIR "/components/AddMemberDialog.qml")));
        QVERIFY(dialog.contains(QStringLiteral("botAdvisory()")));
    }

    void noTelemetryRidesInOnTheNewSurface()
    {
        // House rule, checked where a new network-touching surface lands.
        const QStringList files{
            QStringLiteral(BSFCHAT_SRC_DIR "/model/ChannelInviteModel.cpp"),
            QStringLiteral(BSFCHAT_SRC_DIR "/model/ChannelInviteModel.h"),
            QStringLiteral(BSFCHAT_QML_DIR "/components/AddMemberDialog.qml"),
        };
        for (const QString& path : files) {
            const QString src = withoutComments(readAll(path));
            QVERIFY2(!src.isEmpty(), qPrintable(path));
            for (const char* word : {"analytics", "telemetry", "trackEvent", "gtag"}) {
                QVERIFY2(!src.contains(QLatin1String(word), Qt::CaseInsensitive),
                         qPrintable(path + QStringLiteral(": ") + QLatin1String(word)));
            }
        }
    }
};

QTEST_APPLESS_MAIN(TestChannelInvite)
#include "test_channel_invite.moc"
