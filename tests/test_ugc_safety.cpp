// Blocking, reporting and account deletion — the three App Store gates
// (Apple guideline 1.2 user-generated content, 5.1.1(v) account deletion) —
// pinned at the four places a mistake would be invisible from the outside.
//
// All four halves below exist because the failure mode is silence. A block
// that did not take, a report that went nowhere, a rate limit surfaced without
// its wait, and a delete flow that asked for a password it then did not send
// all LOOK exactly like the working version from the UI. Nothing in here needs
// a socket, an event loop or a window: the wire-facing logic lives in
// header-only classes (net/IgnoredUsers.h, net/ReportRequest.h,
// net/MatrixFailure.h, net/DeactivateFlow.h) for that reason, and
// BlockedUsersModel reaches the network through std::function hooks a
// recording fake substitutes for — the seam SelfRoleModel and BotAdminModel
// already use.
//
// ── the four things ───────────────────────────────────────────────────────
//
//  1. THE FULL-REPLACEMENT ROUND TRIP. PUT /account_data/m.ignored_user_list
//     replaces the whole document; there is no remove verb. A client that
//     builds the next document from a stale copy silently unblocks everyone it
//     does not know about, and nothing says so — the user finds out when the
//     person they blocked speaks again. Pinned: the document is always derived
//     from the last one the SERVER gave us, unknown keys survive it, and an
//     unblock is the same document minus one key.
//
//  2. THE REPORT REQUEST SHAPE. A report is fire-and-forget by design (the
//     server answers {} and tells the reported party nothing), so a wrong path
//     or a mis-named field produces a dialog that says "Report sent" over a
//     404. Pinned against the paths and field names in
//     server/src/api/ReportHandler.h.
//
//  3. THE RATE LIMIT. /report is limited per account
//     (SendLimiter::Bucket::kReport). The server sends the wait twice — as
//     retry_after_ms in the Matrix body and as Retry-After in the header
//     (server/src/http/RateLimitResponse.h) — and the client read NEITHER
//     anywhere except one unreachable lambda in ServerConnection. A refusal
//     with no wait in it makes the user press the button again, which is
//     refused again.
//
//  4. THE DEACTIVATE HANDSHAKE. Two different things arrive as 401: "prove it
//     is you" (the UIA challenge, no errcode, carries `flows`) and "your
//     session is dead" (M_UNKNOWN_TOKEN). Confusing them either eats the
//     password prompt or tells a signed-in user they have been signed out.
//     And a wrong password is 403, which must NOT unwind the prompt — the
//     server counts that attempt against the same lockout /login uses.
//
// Source scans close the last gap: the controls have to be reachable, and on a
// phone they have to be reachable by touch. A block entry that only a
// right-click can open is not a block entry on iOS, which is where the
// guideline is enforced.
#include "model/BlockedUsersModel.h"
#include "net/DeactivateFlow.h"
#include "net/IgnoredUsers.h"
#include "net/MatrixFailure.h"
#include "net/ReportRequest.h"

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <utility>

using namespace bsfchat::net;

namespace {

// JSON written with single quotes and swapped here. Not a style choice: moc
// mis-lexes double quotes inside a R"(...)" literal in a file that also
// declares a Q_OBJECT class, decides there are "no relevant classes", and the
// binary then fails to link on a missing vtable. Same workaround as
// tests/test_server_discovery.cpp.
QByteArray json(const char* singleQuoted)
{
    return QByteArray(singleQuoted).replace('\'', '"');
}

QJsonObject obj(const char* singleQuoted)
{
    return QJsonDocument::fromJson(json(singleQuoted)).object();
}

QStringList keysOf(const QJsonObject& document)
{
    QStringList out = document.value(QStringLiteral("ignored_users")).toObject().keys();
    out.sort();
    return out;
}

QString readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

// Comments only — string literals are left alone, so a scan can miss an
// offender but cannot invent one. Same helper as test_qml_hygiene.cpp.
QString withoutComments(QString src)
{
    static const QRegularExpression block(QStringLiteral(R"(/\*.*?\*/)"),
                                          QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression line(QStringLiteral("//[^\n]*"));
    return src.remove(block).remove(line);
}

// Records what the model asked the network to do, and hands replies back.
struct Recorder {
    int fetches = 0;
    QStringList requestIds;
    QList<QJsonObject> documents;

    void install(BlockedUsersModel& m)
    {
        m.hooks.fetch = [this]() { ++fetches; };
        m.hooks.store = [this](const QString& id, const QJsonObject& doc) {
            requestIds << id;
            documents << doc;
        };
    }

    QJsonObject lastDocument() const
    {
        return documents.isEmpty() ? QJsonObject() : documents.last();
    }
    QString lastRequestId() const
    {
        return requestIds.isEmpty() ? QString() : requestIds.last();
    }
};

} // namespace

class UgcSafetyTest : public QObject {
    Q_OBJECT

private slots:

    // ───────────────── 1. the ignore-list round trip ─────────────────

    void anAbsentDocumentIsAnEmptyListAndCountsAsLoaded()
    {
        IgnoredUsers list;
        QVERIFY(!list.loaded());
        QVERIFY(list.users().isEmpty());

        list.adoptAbsent();
        QVERIFY(list.loaded());
        QVERIFY(list.users().isEmpty());
        QCOMPARE(list.count(), 0);
    }

    void aDocumentWithoutTheKeyIsAnEmptyListRatherThanAnError()
    {
        // The server reads it the same way: no `ignored_users` means nobody is
        // ignored, and it is what a client sends to clear the list.
        IgnoredUsers list;
        list.adopt(obj("{}"));
        QVERIFY(list.loaded());
        QCOMPARE(list.count(), 0);
    }

    void aNonObjectIgnoredUsersIsAnEmptyListNotAParseFailure()
    {
        // Refusing to parse would leave the pane with no list at all over a
        // malformation the user cannot fix from the client.
        IgnoredUsers list;
        list.adopt(obj("{'ignored_users': ['@a:h']}"));
        QVERIFY(list.loaded());
        QCOMPARE(list.count(), 0);
    }

    void theListIsSortedSoThePaneDoesNotReshuffleBetweenRefreshes()
    {
        IgnoredUsers list;
        list.adopt(obj("{'ignored_users':{'@zoe:h':{},'@amy:h':{},'@moe:h':{}}}"));
        QCOMPARE(list.users(),
                 (QStringList{QStringLiteral("@amy:h"), QStringLiteral("@moe:h"),
                              QStringLiteral("@zoe:h")}));
    }

    // THE defect this class exists for.
    void blockingBuildsTheWholeDocumentFromTheServersLastCopy()
    {
        IgnoredUsers list;
        list.adopt(obj("{'ignored_users':{'@old:h':{},'@older:h':{}}}"));

        const QJsonObject next = list.documentWith(QStringLiteral("@new:h"));
        // All three, not just the new one. A document carrying only the id
        // that was just blocked is an unblock of everybody else.
        QCOMPARE(keysOf(next),
                 (QStringList{QStringLiteral("@new:h"), QStringLiteral("@old:h"),
                              QStringLiteral("@older:h")}));
    }

    void unblockingIsTheSameDocumentMinusOneKey()
    {
        IgnoredUsers list;
        list.adopt(obj("{'ignored_users':{'@a:h':{},'@b:h':{},'@c:h':{}}}"));
        QCOMPARE(keysOf(list.documentWithout(QStringLiteral("@b:h"))),
                 (QStringList{QStringLiteral("@a:h"), QStringLiteral("@c:h")}));
    }

    void keysThisClientDoesNotUnderstandSurviveAWrite()
    {
        // Forward compatibility is not decoration here: the document is a
        // full replacement, so a field this client drops is a field it
        // DELETES from the account on the next block.
        IgnoredUsers list;
        list.adopt(obj("{'ignored_users':{'@a:h':{}},'org.example.future':{'x':1}}"));

        const QJsonObject next = list.documentWith(QStringLiteral("@b:h"));
        QVERIFY(next.contains(QStringLiteral("org.example.future")));
        QCOMPARE(next.value(QStringLiteral("org.example.future"))
                     .toObject().value(QStringLiteral("x")).toInt(), 1);
    }

    void aBlockedUsersOwnEntryValueIsPreservedRatherThanResetToEmpty()
    {
        // The spec reserves that object. Clobbering it would destroy a field
        // this client simply does not understand yet.
        IgnoredUsers list;
        list.adopt(obj("{'ignored_users':{'@a:h':{'note':'spam'}}}"));
        const QJsonObject next = list.documentWith(QStringLiteral("@b:h"));
        QCOMPARE(next.value(QStringLiteral("ignored_users")).toObject()
                     .value(QStringLiteral("@a:h")).toObject()
                     .value(QStringLiteral("note")).toString(),
                 QStringLiteral("spam"));
    }

    void adoptReportsWhetherTheMembershipActuallyMoved()
    {
        IgnoredUsers list;
        QVERIFY(list.adopt(obj("{'ignored_users':{'@a:h':{}}}")));
        // A refresh that agrees with what we had is not news to repaint on.
        QVERIFY(!list.adopt(obj("{'ignored_users':{'@a:h':{}}}")));
        QVERIFY(list.adopt(obj("{'ignored_users':{}}")));
    }

    void theRefusalsMirrorTheServersOwnValidation()
    {
        IgnoredUsers list;
        list.adopt(obj("{'ignored_users':{'@taken:h':{}}}"));

        QCOMPARE(list.canBlock(QStringLiteral("not-a-user-id"),
                               QStringLiteral("@me:h")),
                 BlockRefusal::NotAUserId);
        QCOMPARE(list.canBlock(QStringLiteral("@me:h"), QStringLiteral("@me:h")),
                 BlockRefusal::Self);
        QCOMPARE(list.canBlock(QStringLiteral("@taken:h"), QStringLiteral("@me:h")),
                 BlockRefusal::AlreadyBlocked);
        QCOMPARE(list.canBlock(QStringLiteral("@fresh:h"), QStringLiteral("@me:h")),
                 BlockRefusal::None);
        // Our own id is not always known yet; the check is skipped rather
        // than guessed, and the server still catches it.
        QCOMPARE(list.canBlock(QStringLiteral("@fresh:h"), QString()),
                 BlockRefusal::None);
        QCOMPARE(list.canUnblock(QStringLiteral("@nobody:h")),
                 BlockRefusal::NotBlocked);
        QCOMPARE(list.canUnblock(QStringLiteral("@taken:h")), BlockRefusal::None);
    }

    void theListCeilingIsRefusedBeforeTheRoundTripNotAfterIt()
    {
        QJsonObject entries;
        for (int i = 0; i < kMaxIgnoredUsers; ++i)
            entries.insert(QStringLiteral("@u%1:h").arg(i), QJsonObject());
        QJsonObject document;
        document.insert(QStringLiteral("ignored_users"), entries);

        IgnoredUsers list;
        list.adopt(document);
        QCOMPARE(list.count(), kMaxIgnoredUsers);
        QCOMPARE(list.canBlock(QStringLiteral("@one-more:h"), QStringLiteral("@me:h")),
                 BlockRefusal::TooMany);
    }

    void theAccountDataPathEncodesBothSegments()
    {
        // A user id holds '@' and ':' and a type holds '.'. An unencoded ':'
        // in a path segment is how "@a:b" became two segments and a 404
        // nobody could explain.
        const QString path = accountDataPath(QStringLiteral("@amy:bsfchat.com"),
                                             QString(kIgnoredUserListType));
        QCOMPARE(path,
                 QStringLiteral("/_matrix/client/v3/user/%40amy%3Absfchat.com"
                                "/account_data/m.ignored_user_list"));
    }

    void theTypeIsTheMatrixStandardOneNotABsfchatInvention()
    {
        // A conventional client looking for this account's block list looks
        // here. A bsfchat.* name would make every other client's block button
        // do nothing.
        QCOMPARE(QString(kIgnoredUserListType),
                 QStringLiteral("m.ignored_user_list"));
    }

    // ─────────────── 2. the model over that round trip ───────────────

    void theModelSaysNothingUntilItHasAsked()
    {
        BlockedUsersModel m;
        QVERIFY(!m.loaded());
        QCOMPARE(m.count(), 0);
        QCOMPARE(m.lastRefreshedMs(), 0);
    }

    void aBlockWritesTheFullDocumentAndOnlyTakesEffectWhenItLands()
    {
        BlockedUsersModel m;
        Recorder rec;
        rec.install(m);
        m.setSelfUserId(QStringLiteral("@me:h"));
        m.onDocument(obj("{'ignored_users':{'@old:h':{}}}"));

        m.block(QStringLiteral("@new:h"));
        QCOMPARE(rec.documents.size(), 1);
        QCOMPARE(keysOf(rec.lastDocument()),
                 (QStringList{QStringLiteral("@new:h"), QStringLiteral("@old:h")}));

        // Optimism covers what the UI draws, so the menu does not flicker and
        // the row appears in the managed list at once…
        QVERIFY(m.isBlocked(QStringLiteral("@new:h")));
        QVERIFY(m.isPending(QStringLiteral("@new:h")));
        QCOMPARE(m.users().size(), 2);
        QVERIFY(m.busy());

        // …but it is marked unconfirmed until the server says so, and only
        // then does the row settle.
        m.onWriteStored(rec.lastRequestId(), rec.lastDocument());
        QCOMPARE(m.count(), 2);
        QVERIFY(!m.isPending(QStringLiteral("@new:h")));
        QVERIFY(m.isBlocked(QStringLiteral("@new:h")));
        QVERIFY(!m.busy());
    }

    void aRefusedBlockRollsBackInsteadOfClaimingABlockThatIsNotInForce()
    {
        BlockedUsersModel m;
        Recorder rec;
        rec.install(m);
        m.onDocumentAbsent();

        m.block(QStringLiteral("@spammer:h"));
        QVERIFY(m.isBlocked(QStringLiteral("@spammer:h")));

        m.onWriteFailed(rec.lastRequestId(),
                        parseMatrixFailure(403, json("{'errcode':'M_FORBIDDEN'}")));
        QVERIFY(!m.isBlocked(QStringLiteral("@spammer:h")));
        QVERIFY(!m.isPending(QStringLiteral("@spammer:h")));
        QVERIFY(!m.errorText().isEmpty());
        QVERIFY(!m.busy());
    }

    void aSecondClickOnTheSameRowDoesNotSendASecondDocument()
    {
        // Two full replacements in flight at once is how one of them lands
        // last and undoes the other; and a repeat of a state already asked
        // for has nothing to say.
        BlockedUsersModel m;
        Recorder rec;
        rec.install(m);
        m.onDocumentAbsent();

        m.block(QStringLiteral("@a:h"));
        m.block(QStringLiteral("@a:h"));
        QCOMPARE(rec.documents.size(), 1);
    }

    void blockingSomebodyAlreadyBlockedWritesNothingAndSaysNothing()
    {
        BlockedUsersModel m;
        Recorder rec;
        rec.install(m);
        m.onDocument(obj("{'ignored_users':{'@a:h':{}}}"));

        m.block(QStringLiteral("@a:h"));
        QCOMPARE(rec.documents.size(), 0);
        // Not an error: the only way here is a stale menu, and a toast about
        // it would be noise about nothing.
        QVERIFY(m.errorText().isEmpty());
    }

    void aSelfBlockIsRefusedWithoutAskingTheServer()
    {
        BlockedUsersModel m;
        Recorder rec;
        rec.install(m);
        m.setSelfUserId(QStringLiteral("@me:h"));
        m.onDocumentAbsent();

        m.block(QStringLiteral("@me:h"));
        QCOMPARE(rec.documents.size(), 0);
        QVERIFY(!m.errorText().isEmpty());
    }

    void aWriteAdoptsWhatWasStoredSoARaceCorrectsItselfRatherThanBeingPaperedOver()
    {
        // The reply is authoritative even when it disagrees with the optimism
        // — which is what a write that raced another device looks like.
        BlockedUsersModel m;
        Recorder rec;
        rec.install(m);
        m.onDocument(obj("{'ignored_users':{}}"));
        m.block(QStringLiteral("@a:h"));

        m.onWriteStored(rec.lastRequestId(),
                        obj("{'ignored_users':{'@a:h':{},'@elsewhere:h':{}}}"));
        QCOMPARE(m.count(), 2);
        QVERIFY(m.isBlocked(QStringLiteral("@elsewhere:h")));
    }

    // THE cold-start defect, and the reason block() is not a one-liner.
    void aBlockBeforeTheListHasBeenReadFetchesFirstAndWritesNothingYet()
    {
        BlockedUsersModel m;
        Recorder rec;
        rec.install(m);

        // A menu click on a session that has not read the document. Sending
        // now would PUT a document holding one id — a full replacement, so
        // every block this account already had would be gone.
        m.block(QStringLiteral("@spammer:h"));
        QCOMPARE(rec.documents.size(), 0);
        QCOMPARE(rec.fetches, 1);

        m.onDocument(obj("{'ignored_users':{'@old:h':{},'@older:h':{}}}"));
        QCOMPARE(rec.documents.size(), 1);
        QCOMPARE(keysOf(rec.lastDocument()),
                 (QStringList{QStringLiteral("@old:h"), QStringLiteral("@older:h"),
                              QStringLiteral("@spammer:h")}));
    }

    void severalBlocksClickedBeforeTheListLandsGoOutAsOneDocument()
    {
        // Somebody working through a spam wave clicks faster than a round
        // trip. Three separate full replacements, each built from the same
        // pre-write list, would land in an order nobody controls and leave
        // ONE of the three blocked.
        BlockedUsersModel m;
        Recorder rec;
        rec.install(m);

        m.block(QStringLiteral("@a:h"));
        m.block(QStringLiteral("@b:h"));
        m.block(QStringLiteral("@c:h"));
        // One fetch, not three — refresh() drops a second call while one is
        // in flight — and nothing written yet.
        QCOMPARE(rec.fetches, 1);
        QCOMPARE(rec.documents.size(), 0);

        m.onDocumentAbsent();
        QCOMPARE(rec.documents.size(), 1);
        QCOMPARE(keysOf(rec.lastDocument()),
                 (QStringList{QStringLiteral("@a:h"), QStringLiteral("@b:h"),
                              QStringLiteral("@c:h")}));

        m.onWriteStored(rec.lastRequestId(), rec.lastDocument());
        QCOMPARE(m.count(), 3);
        QVERIFY(!m.isPending(QStringLiteral("@a:h")));
    }

    void aClickDuringAWriteWaitsForItRatherThanRacingIt()
    {
        // The second document has to be built from the FIRST one's result. A
        // client that sent it immediately would build it from the pre-write
        // list and undo the write still in flight.
        BlockedUsersModel m;
        Recorder rec;
        rec.install(m);
        m.onDocument(obj("{'ignored_users':{'@old:h':{}}}"));

        m.block(QStringLiteral("@a:h"));
        QCOMPARE(rec.documents.size(), 1);
        m.block(QStringLiteral("@b:h"));
        // Still one: the line is busy.
        QCOMPARE(rec.documents.size(), 1);
        // …but the menu already reads as blocked, so it does not flicker.
        QVERIFY(m.isBlocked(QStringLiteral("@b:h")));

        m.onWriteStored(rec.lastRequestId(), rec.lastDocument());
        QCOMPARE(rec.documents.size(), 2);
        QCOMPARE(keysOf(rec.lastDocument()),
                 (QStringList{QStringLiteral("@a:h"), QStringLiteral("@b:h"),
                              QStringLiteral("@old:h")}));
    }

    void aBlockAndAnUnblockInOneBatchAreBothInTheDocument()
    {
        BlockedUsersModel m;
        Recorder rec;
        rec.install(m);
        m.onDocument(obj("{'ignored_users':{'@old:h':{}}}"));

        m.block(QStringLiteral("@new:h"));
        m.unblock(QStringLiteral("@old:h"));
        // The first went out alone; the unblock waits and then carries both
        // the state the first established and its own change.
        m.onWriteStored(rec.lastRequestId(), rec.lastDocument());
        QCOMPARE(keysOf(rec.lastDocument()),
                 QStringList{QStringLiteral("@new:h")});
    }

    void anIntentStrandedByAFailedFirstFetchIsDroppedRatherThanSentBlind()
    {
        // Sending it anyway would be the exact write this deferral exists to
        // prevent — a full replacement built from a list we could not read.
        BlockedUsersModel m;
        Recorder rec;
        rec.install(m);

        m.block(QStringLiteral("@a:h"));
        m.onFetchFailed(parseMatrixFailure(0, QByteArray(), QByteArray(),
                                           QStringLiteral("host unreachable")));
        QCOMPARE(rec.documents.size(), 0);
        QVERIFY(!m.errorText().isEmpty());

        // And it does not fire later, when an unrelated refresh succeeds.
        m.onDocument(obj("{'ignored_users':{'@old:h':{}}}"));
        QCOMPARE(rec.documents.size(), 0);
    }

    void aFailedRefreshLeavesTheListAloneRatherThanEmptyingThePane()
    {
        // "Your blocks are gone" is a much worse sentence than "could not
        // refresh", and a dropped connection says nothing about who is blocked.
        BlockedUsersModel m;
        Recorder rec;
        rec.install(m);
        m.onDocument(obj("{'ignored_users':{'@a:h':{},'@b:h':{}}}"));

        m.onFetchFailed(parseMatrixFailure(0, {}, {}, QStringLiteral("host unreachable")));
        QCOMPARE(m.count(), 2);
        QVERIFY(m.loaded());
        QVERIFY(!m.errorText().isEmpty());
    }

    void lastRefreshedIsStampedOnEveryAdoptionBecauseNothingElseWill()
    {
        // The pane renders this verbatim, and it is the whole of the currency
        // claim this client is entitled to make. /sync pushes the document now
        // (onDocumentFromSync), but only as a delta, so "when the server last
        // told us" is still the strongest true statement — and a push stamps
        // it like any other adoption, because it IS the server telling us.
        BlockedUsersModel m;
        Recorder rec;
        rec.install(m);
        qint64 now = 1000;
        m.setClock([&now] { return now; });

        m.onDocument(obj("{'ignored_users':{}}"));
        QCOMPARE(m.lastRefreshedMs(), 1000);

        now = 5000;
        m.block(QStringLiteral("@a:h"));
        m.onWriteStored(rec.lastRequestId(), rec.lastDocument());
        QCOMPARE(m.lastRefreshedMs(), 5000);

        now = 9000;
        QVERIFY(m.onDocumentFromSync(obj("{'ignored_users':{'@a:h':{},'@b:h':{}}}")));
        QCOMPARE(m.lastRefreshedMs(), 9000);
    }

    void theCeilingIsCheckedAgainstWhatTheBatchWouldLeaveNotTheCurrentCount()
    {
        // A batch can cross the limit when no member of it would on its own,
        // and there is no honest way to pick which of the user's blocks to
        // drop on their behalf — so it is refused whole.
        QJsonObject entries;
        for (int i = 0; i < kMaxIgnoredUsers - 1; ++i)
            entries.insert(QStringLiteral("@u%1:h").arg(i), QJsonObject());
        QJsonObject document;
        document.insert(QStringLiteral("ignored_users"), entries);

        BlockedUsersModel m;
        Recorder rec;
        rec.install(m);
        m.onDocument(document);

        m.block(QStringLiteral("@one:h"));
        QCOMPARE(rec.documents.size(), 1);
        m.onWriteStored(rec.lastRequestId(), rec.lastDocument());
        QCOMPARE(m.count(), kMaxIgnoredUsers);

        m.block(QStringLiteral("@two:h"));
        QCOMPARE(rec.documents.size(), 1);
        QVERIFY(!m.errorText().isEmpty());
        QVERIFY(!m.isBlocked(QStringLiteral("@two:h")));
    }

    void resetForgetsTheListBecauseItBelongsToOneAccountOnOneServer()
    {
        BlockedUsersModel m;
        Recorder rec;
        rec.install(m);
        m.onDocument(obj("{'ignored_users':{'@a:h':{}}}"));

        m.reset();
        QVERIFY(!m.loaded());
        QCOMPARE(m.count(), 0);
        QVERIFY(!m.isBlocked(QStringLiteral("@a:h")));
    }

    void aReplyToAWriteThisModelHasForgottenIsIgnored()
    {
        // A reset between request and response. Adopting it would restore one
        // account's block list onto another's session.
        BlockedUsersModel m;
        Recorder rec;
        rec.install(m);
        m.onDocumentAbsent();
        m.block(QStringLiteral("@a:h"));
        const QString id = rec.lastRequestId();
        m.reset();

        m.onWriteStored(id, obj("{'ignored_users':{'@a:h':{}}}"));
        QCOMPARE(m.count(), 0);
        QVERIFY(!m.loaded());
    }

    // ───────────────── 3. the report request shape ─────────────────

    void theReportPathsAreTheMatrixSpecOnes()
    {
        // server/src/api/ReportHandler.h: these are spec paths on purpose, so
        // a conventional client's report button works against this server.
        QCOMPARE(reportEventPath(QStringLiteral("!room:h"), QStringLiteral("$evt")),
                 QStringLiteral("/_matrix/client/v3/rooms/%21room%3Ah/report/%24evt"));
        QCOMPARE(reportUserPath(QStringLiteral("@spammer:h")),
                 QStringLiteral("/_matrix/client/v3/users/%40spammer%3Ah/report"));
    }

    void aReasonIsSentUnderTheNameTheServerReads()
    {
        const QJsonObject body = reportEventBody(QStringLiteral("harassment"));
        QCOMPARE(body.value(QStringLiteral("reason")).toString(),
                 QStringLiteral("harassment"));
    }

    void anEmptyReasonIsAnAbsentKeyRatherThanAnEmptyString()
    {
        // A row whose reason is "" reads, in an administrator's queue, as a
        // reason that was typed and then lost. An absent one reads as what it
        // is: a report filed with nothing said, which is still a report.
        QVERIFY(!reportEventBody(QString()).contains(QStringLiteral("reason")));
        QVERIFY(!reportEventBody(QStringLiteral("   ")).contains(QStringLiteral("reason")));
        QVERIFY(!reportUserBody(QString()).contains(QStringLiteral("reason")));
    }

    void noScoreIsSentUnlessOneIsAskedFor()
    {
        // There is no score control in the UI; the server's default is what
        // the row carries.
        QVERIFY(!reportEventBody(QStringLiteral("x")).contains(QStringLiteral("score")));
        QCOMPARE(reportEventBody(QStringLiteral("x"), -100)
                     .value(QStringLiteral("score")).toInt(), -100);
    }

    void theUserReportCarriesNoScoreAtAll()
    {
        // The server parses that body with scored=false — a score there would
        // be a field no reader could act on.
        const QJsonObject body = reportUserBody(QStringLiteral("impersonation"));
        QVERIFY(!body.contains(QStringLiteral("score")));
        QCOMPARE(body.keys(), QStringList{QStringLiteral("reason")});
    }

    void anOverlongReasonIsRefusedInBytesNotCharacters()
    {
        // The server measures the UTF-8 (input_limits::kMaxReasonBytes), so a
        // client counting characters would offer to send a reason the server
        // then refuses, after the dialog has closed.
        QVERIFY(reportReasonFits(QString(kMaxReportReasonBytes, QLatin1Char('a'))));
        QVERIFY(!reportReasonFits(QString(kMaxReportReasonBytes + 1, QLatin1Char('a'))));
        // Three bytes each, so a third of the character count is the ceiling.
        QVERIFY(!reportReasonFits(QString(kMaxReportReasonBytes / 2, QChar(0x4E16))));
    }

    // ───────────────── 4. rate limiting, read honestly ─────────────────

    void theWaitIsReadFromTheMatrixBody()
    {
        const auto f = parseMatrixFailure(
            429, json("{'errcode':'M_LIMIT_EXCEEDED','error':'Too many reports','retry_after_ms':4500}"));
        QVERIFY(f.isRateLimited());
        QCOMPARE(f.retryAfterMs, 4500);
        // Rounded UP: sending the user back a fraction early just earns them a
        // second refusal, which is also how the server rounds its header.
        QCOMPARE(f.retrySeconds(), 5);
        QCOMPARE(f.message, QStringLiteral("Too many reports"));
    }

    void theRetryAfterHeaderIsTheFallbackForARefusalWithNoMatrixBody()
    {
        // A proxy or load balancer in front of the server writes the header
        // and no Matrix object at all.
        const auto f = parseMatrixFailure(429, QByteArray(), QByteArray("30"),
                                          QStringLiteral("Too Many Requests"));
        QVERIFY(f.isRateLimited());
        QCOMPARE(f.retrySeconds(), 30);
        QCOMPARE(f.message, QStringLiteral("Too Many Requests"));
    }

    void theBodyWinsOverTheHeaderBecauseItIsThePreciseOne()
    {
        const auto f = parseMatrixFailure(
            429, json("{'errcode':'M_LIMIT_EXCEEDED','retry_after_ms':1200}"),
            QByteArray("9"));
        QCOMPARE(f.retryAfterMs, 1200);
    }

    void aRefusalThatNamedNoWaitReportsNoWaitRatherThanZeroSeconds()
    {
        const auto f = parseMatrixFailure(429, json("{'errcode':'M_LIMIT_EXCEEDED'}"));
        QVERIFY(f.isRateLimited());
        QCOMPARE(f.retrySeconds(), 0);
    }

    void anHttpDateRetryAfterIsIgnoredRatherThanMisparsedAsNoWait()
    {
        const auto f = parseMatrixFailure(429, QByteArray(),
                                          QByteArray("Wed, 21 Oct 2026 07:28:00 GMT"));
        QCOMPARE(f.retryAfterMs, -1);
        QCOMPARE(f.retrySeconds(), 0);
    }

    void aStringRetryAfterMsIsNotReadAsZero()
    {
        // toDouble() on a string yields 0, which is indistinguishable from
        // "the server named no wait" — the one answer that is definitely wrong.
        const auto f = parseMatrixFailure(
            429, json("{'errcode':'M_LIMIT_EXCEEDED','retry_after_ms':'5000'}"));
        QCOMPARE(f.retryAfterMs, -1);
    }

    void aMessageIsAlwaysProducedFromSomething()
    {
        QCOMPARE(parseMatrixFailure(403, json("{'errcode':'M_FORBIDDEN'}")).message,
                 QStringLiteral("M_FORBIDDEN"));
        QCOMPARE(parseMatrixFailure(0, QByteArray(), QByteArray(),
                                    QStringLiteral("Connection refused")).message,
                 QStringLiteral("Connection refused"));
        // An HTML error page from a proxy is not a Matrix object and must not
        // be mistaken for one.
        const auto html = parseMatrixFailure(502, QByteArray("<html>502</html>"),
                                             QByteArray(), QStringLiteral("Bad gateway"));
        QVERIFY(html.errcode.isEmpty());
        QCOMPARE(html.message, QStringLiteral("Bad gateway"));
    }

    void aNon429WithAnErrcodeOfLimitExceededStillCountsAsRateLimited()
    {
        const auto f = parseMatrixFailure(400, json("{'errcode':'M_LIMIT_EXCEEDED'}"));
        QVERIFY(f.isRateLimited());
    }

    void theModelSurfacesTheWaitWhenAWriteIsRateLimited()
    {
        BlockedUsersModel m;
        Recorder rec;
        rec.install(m);
        m.onDocumentAbsent();
        m.block(QStringLiteral("@a:h"));

        m.onWriteFailed(rec.lastRequestId(),
                        parseMatrixFailure(429, json("{'errcode':'M_LIMIT_EXCEEDED','retry_after_ms':30000}")));
        QVERIFY(m.errorText().contains(QStringLiteral("30")));
    }

    // ───────────────── 5. the deactivate handshake ─────────────────

    void anAccountWithNoPasswordIsDeletedByTheFirstRequest()
    {
        // An identity-provider account has no password on this server and is
        // admitted on the bearer token alone. The first request is therefore
        // NOT a probe, which is why the confirmation must already be complete.
        DeactivateFlow flow;
        QVERIFY(flow.begin());
        QVERIFY(flow.busy());
        QCOMPARE(flow.onReply(200, obj("{'id_server_unbind_result':'no-support'}")),
                 DeactivateOutcome::Succeeded);
        QCOMPARE(flow.phase(), DeactivatePhase::Deactivated);
    }

    void aUiaChallengeIsRecognisedByItsFlowsNotByAnErrcode()
    {
        // It carries none. The other 401 — M_UNKNOWN_TOKEN — means the exact
        // opposite thing, and telling them apart by status alone either eats
        // the password prompt or claims a signed-in user has been signed out.
        DeactivateFlow flow;
        QVERIFY(flow.begin());
        QCOMPARE(flow.onReply(401, obj("{'flows':[{'stages':['m.login.password']}],"
                                       "'params':{},'completed':[],'session':'sess-1'}")),
                 DeactivateOutcome::PasswordRequired);
        QCOMPARE(flow.phase(), DeactivatePhase::PasswordRequired);
        QVERIFY(flow.needsPassword());
        QVERIFY(!flow.busy());
        QCOMPARE(flow.session(), QStringLiteral("sess-1"));
        // Not an error — the server is doing its job, and a sentence here
        // would put "something went wrong" above an empty field.
        QVERIFY(flow.errorText().isEmpty());
    }

    void aDeadTokenIs401WithNoFlowsAndMustNotBecomeAPasswordPrompt()
    {
        DeactivateFlow flow;
        QVERIFY(flow.begin());
        QCOMPARE(flow.onReply(401, obj("{'errcode':'M_UNKNOWN_TOKEN',"
                                       "'error':'Unrecognised access token'}")),
                 DeactivateOutcome::Failed);
        QVERIFY(!flow.needsPassword());
        QCOMPARE(flow.phase(), DeactivatePhase::Idle);
    }

    void aChallengeOfferingOnlyStagesThisClientCannotCompleteIsAFailure()
    {
        // A password field that can never succeed is worse than a refusal
        // that says so.
        DeactivateFlow flow;
        QVERIFY(flow.begin());
        QCOMPARE(flow.onReply(401, obj("{'flows':[{'stages':['m.login.recaptcha']}],"
                                       "'session':'s'}")),
                 DeactivateOutcome::Failed);
        QVERIFY(!flow.needsPassword());
    }

    void theSecondRequestCarriesTheTypeAndTheServersSessionAndNoIdentifier()
    {
        // The server takes the account from the bearer token and refuses a
        // named identifier that disagrees with it, so sending one adds a way
        // to get a 403 and no way to succeed.
        DeactivateFlow flow;
        QVERIFY(flow.begin());
        flow.onReply(401, obj("{'flows':[{'stages':['m.login.password']}],'session':'sess-7'}"));
        QVERIFY(flow.submitPassword());

        const QJsonObject auth = flow.authObject(QStringLiteral("hunter2"));
        QCOMPARE(auth.value(QStringLiteral("type")).toString(),
                 QStringLiteral("m.login.password"));
        QCOMPARE(auth.value(QStringLiteral("password")).toString(),
                 QStringLiteral("hunter2"));
        QCOMPARE(auth.value(QStringLiteral("session")).toString(),
                 QStringLiteral("sess-7"));
        QVERIFY(!auth.contains(QStringLiteral("identifier")));
    }

    void aWrongPasswordKeepsTheFieldOnScreenInsteadOfUnwindingTheDialog()
    {
        // 403, not another 401. The server has already counted the attempt
        // against the same lockout /login uses, so making the user reopen the
        // dialog to retype a typo spends that budget on the UI's behalf.
        DeactivateFlow flow;
        flow.begin();
        flow.onReply(401, obj("{'flows':[{'stages':['m.login.password']}],'session':'s'}"));
        flow.submitPassword();

        QCOMPARE(flow.onReply(403, obj("{'errcode':'M_FORBIDDEN','error':'Invalid password'}")),
                 DeactivateOutcome::PasswordRejected);
        QCOMPARE(flow.phase(), DeactivatePhase::PasswordRequired);
        QVERIFY(flow.needsPassword());
        QCOMPARE(flow.errorText(), QStringLiteral("Invalid password"));
        // And it can be retried without starting over.
        QVERIFY(flow.submitPassword());
    }

    void a403BeforeAPasswordWasSentIsAPlainRefusalNotAWrongPassword()
    {
        // A bot reaching this route gets one, and "that password is not
        // correct" would be an outright wrong diagnosis.
        DeactivateFlow flow;
        flow.begin();
        QCOMPARE(flow.onReply(403, obj("{'errcode':'M_FORBIDDEN','error':"
                                       "'Bot accounts are deactivated by an administrator'}")),
                 DeactivateOutcome::Failed);
        QCOMPARE(flow.phase(), DeactivatePhase::Idle);
        QVERIFY(flow.errorText().contains(QStringLiteral("administrator")));
    }

    void aSecondBeginWhileOneIsRunningIsRefused()
    {
        DeactivateFlow flow;
        QVERIFY(flow.begin());
        QVERIFY(!flow.begin());
    }

    void aPasswordCannotBeSubmittedBeforeTheServerAsksForOne()
    {
        DeactivateFlow flow;
        QVERIFY(!flow.submitPassword());
        flow.begin();
        QVERIFY(!flow.submitPassword());
    }

    void aLateReplyToAnAbandonedAttemptIsIgnored()
    {
        DeactivateFlow flow;
        flow.begin();
        flow.reset();
        QCOMPARE(flow.onReply(200, QJsonObject()), DeactivateOutcome::Ignored);
        QCOMPARE(flow.phase(), DeactivatePhase::Idle);
    }

    void aDeletedAccountIsTerminalAndCannotBeResetBackIntoAUsableFlow()
    {
        DeactivateFlow flow;
        flow.begin();
        flow.onReply(200, QJsonObject());
        flow.reset();
        QCOMPARE(flow.phase(), DeactivatePhase::Deactivated);
        QVERIFY(!flow.begin());
    }

    void aTransportFailureWhileAuthenticatingKeepsThePasswordField()
    {
        DeactivateFlow flow;
        flow.begin();
        flow.onReply(401, obj("{'flows':[{'stages':['m.login.password']}],'session':'s'}"));
        flow.submitPassword();

        flow.onTransportFailure(QStringLiteral("Connection refused"));
        QCOMPARE(flow.phase(), DeactivatePhase::PasswordRequired);
        QCOMPARE(flow.errorText(), QStringLiteral("Connection refused"));
    }

    void anHtmlErrorPageIsNotAUiaChallenge()
    {
        // MatrixClient hands an empty object up when the body did not parse.
        DeactivateFlow flow;
        flow.begin();
        QCOMPARE(flow.onReply(401, QJsonObject()), DeactivateOutcome::Failed);
        QVERIFY(!flow.needsPassword());
    }

    // ──────────── 6. the controls exist, and touch can reach them ────────────
    //
    // Headless QML cannot instantiate these components (the BSFChat module is
    // compiled into the app binary), so this is a source scan — the same
    // device test_qml_hygiene.cpp and test_self_roles.cpp use.

    void everyEntryPointOffersBlockingAndReporting()
    {
        struct Surface { const char* file; const char* what; };
        const Surface surfaces[] = {
            {"/components/MessageBubble.qml",   "the message context menu"},
            {"/components/MemberList.qml",      "the member list"},
            {"/components/UserProfileCard.qml", "the profile card"},
        };
        for (const auto& s : surfaces) {
            const QString src = withoutComments(
                readAll(QString::fromUtf8(BSFCHAT_QML_DIR) + QString::fromUtf8(s.file)));
            QVERIFY2(!src.isEmpty(), s.file);
            QVERIFY2(src.contains(QStringLiteral("lockUser"))
                         || src.contains(QStringLiteral("Block")),
                     qPrintable(QStringLiteral("no block control in %1 (%2)")
                                    .arg(s.file, s.what)));
            QVERIFY2(src.contains(QStringLiteral("eport")),
                     qPrintable(QStringLiteral("no report control in %1 (%2)")
                                    .arg(s.file, s.what)));
        }
    }

    void theMemberListCanBeOpenedByTouchAndNotOnlyByRightClick()
    {
        // MobileMain embeds this very component in a drawer, so a menu that
        // only a right-click opens is a menu that does not exist on a phone —
        // and these are the controls an App Store review is looking for.
        const QString src = withoutComments(
            readAll(QStringLiteral(BSFCHAT_QML_DIR "/components/MemberList.qml")));
        QVERIFY2(!src.isEmpty(), "MemberList.qml not found");
        QVERIFY2(src.contains(QStringLiteral("onPressAndHold"))
                     || src.contains(QStringLiteral("onLongPressed")),
                 "MemberList has no long-press path, so its context menu is "
                 "unreachable on touch");
    }

    void theDeleteAccountControlIsInSettingsAndSaysWhatSurvives()
    {
        // Guideline 5.1.1(v) asks for the control; honesty asks for the
        // sentence. Message history stays — it is other people's
        // conversations — and a confirmation that does not say so is a
        // promise the server does not keep.
        const QString settings = withoutComments(
            readAll(QStringLiteral(BSFCHAT_QML_DIR "/components/UserSettings.qml")));
        QVERIFY2(settings.contains(QStringLiteral("openDeleteAccount"))
                     || settings.contains(QStringLiteral("Delete account")),
                 "no delete-account control in UserSettings.qml");

        const QString dialog = withoutComments(
            readAll(QStringLiteral(BSFCHAT_QML_DIR "/components/DeleteAccountDialog.qml")));
        QVERIFY2(!dialog.isEmpty(), "DeleteAccountDialog.qml not found");
        QVERIFY2(dialog.contains(QStringLiteral("stay"))
                     || dialog.contains(QStringLiteral("remain")),
                 "the delete confirmation does not say what is kept");
    }

    void bothShellsCanReachEveryNewDialog()
    {
        // U-C2: a Window.window.openX() that exists in main.qml and not in
        // MobileMain.qml is a TypeError on the platform the store gates are
        // for. Every helper these controls call must be in both.
        //
        // The required set is DERIVED from the call sites rather than listed
        // here, because a hand-kept list only fails for the helpers someone
        // remembered to add to it. openSelfRoles() was live in ChannelList's
        // user menu — ungated, unlike the Keyboard Shortcuts entry next to it
        // — with no forwarder in MobileMain at all, and the three-name list
        // this replaces said nothing. Adding a Window.window.newThing() call
        // to a shared component now fails the build until BOTH shells
        // implement it.
        const QString desktop = withoutComments(
            readAll(QStringLiteral(BSFCHAT_QML_DIR "/main.qml")));
        const QString mobile = withoutComments(
            readAll(QStringLiteral(BSFCHAT_QML_DIR "/mobile/MobileMain.qml")));
        QVERIFY2(!desktop.isEmpty() && !mobile.isEmpty(), "shells not found");

        // Only CALLS — the trailing "(" is what separates a helper the shells
        // owe us from a property read like Window.window.showMemberList or
        // the built-in Window.window.visibility. Any receiver is allowed
        // before it: VoiceDock goes through `dock.Window.window` on purpose
        // (a Connections block is not an Item, so the attached property is
        // null there).
        static const QRegularExpression call(
            QStringLiteral(R"(\bWindow\.window\.([A-Za-z_][A-Za-z0-9_]*)\s*\()"));

        // Provided by QQuickWindow/ApplicationWindow itself, so neither shell
        // declares them and neither should have to.
        static const QSet<QString> builtIns{
            QStringLiteral("close"),       QStringLiteral("show"),
            QStringLiteral("hide"),        QStringLiteral("raise"),
            QStringLiteral("lower"),       QStringLiteral("alert"),
            QStringLiteral("requestActivate"),
            QStringLiteral("showFullScreen"), QStringLiteral("showNormal"),
            QStringLiteral("showMaximized"),  QStringLiteral("showMinimized"),
        };

        // Reached through an alias rather than the literal
        // `Window.window.` spelling the scan matches — ReportDialog and
        // DeleteAccountDialog resolve the window once into `hostWindow` and
        // call the toast helpers off that — so the scan cannot see them.
        // Pinned by hand for exactly that reason.
        QSet<QString> required{
            QStringLiteral("toast"),        QStringLiteral("toastInfo"),
            QStringLiteral("toastSuccess"), QStringLiteral("toastWarn"),
            QStringLiteral("toastError"),
        };

        const QDir dir(QStringLiteral(BSFCHAT_QML_DIR "/components"));
        const QStringList files = dir.entryList({QStringLiteral("*.qml")}, QDir::Files);
        QVERIFY2(files.size() > 20,
                 qPrintable(QStringLiteral("only %1 component(s) under %2 — the scan is "
                                           "not looking where it thinks it is")
                                .arg(files.size()).arg(dir.path())));

        for (const QString& name : files) {
            const QString src = withoutComments(readAll(dir.filePath(name)));
            auto it = call.globalMatch(src);
            while (it.hasNext()) {
                const QString fn = it.next().captured(1);
                if (!builtIns.contains(fn))
                    required.insert(fn);
            }
        }

        // A regex that silently stopped matching would turn this test into a
        // no-op that still passes. These two are live call sites in
        // ChannelList.qml and MessageBubble.qml.
        QVERIFY2(required.contains(QStringLiteral("openSelfRoles")),
                 "the call-site scan found no openSelfRoles() — the scan is broken");
        QVERIFY2(required.contains(QStringLiteral("openReportDialog")),
                 "the call-site scan found no openReportDialog() — the scan is broken");

        QStringList sorted(required.cbegin(), required.cend());
        sorted.sort();
        for (const QString& fn : std::as_const(sorted)) {
            const QString decl = QStringLiteral("function ") + fn;
            QVERIFY2(desktop.contains(decl),
                     qPrintable(QStringLiteral("shared components call Window.window.%1() "
                                               "but main.qml does not define it").arg(fn)));
            QVERIFY2(mobile.contains(decl),
                     qPrintable(QStringLiteral("shared components call Window.window.%1() "
                                               "but MobileMain.qml does not define it — "
                                               "that is a TypeError on iOS/Android (U-C2)").arg(fn)));
        }
    }

    void theReportDialogDoesNotPromiseSomethingWillHappen()
    {
        // Server-side a report is one row for an administrator to read later.
        // It does not redact, mute, hide or notify, and the dialog may not
        // imply that it does — the control that stops it NOW is the block.
        const QString src = withoutComments(
            readAll(QStringLiteral(BSFCHAT_QML_DIR "/components/ReportDialog.qml")));
        QVERIFY2(!src.isEmpty(), "ReportDialog.qml not found");
        QVERIFY2(src.contains(QStringLiteral("dmin")),
                 "the report dialog does not say who receives the report");

        static const QRegularExpression overclaim(
            QStringLiteral("(will be (removed|deleted|banned|muted))"
                           "|(we will (remove|delete|ban))"),
            QRegularExpression::CaseInsensitiveOption);
        QVERIFY2(!overclaim.match(src).hasMatch(),
                 "the report dialog promises an action the server does not take");
    }

    // ── The block list arriving from /sync ────────────────────────────────

    // The server carries account data in /sync now, so a block made on the
    // phone reaches the desktop without anybody pressing Refresh.
    void aDocumentPushedBySyncIsAdopted()
    {
        BlockedUsersModel m;
        Recorder rec;
        rec.install(m);
        m.onDocument(obj("{'ignored_users':{'@old:h':{}}}"));

        QVERIFY(m.onDocumentFromSync(obj("{'ignored_users':{'@old:h':{},'@new:h':{}}}")));
        QVERIFY2(m.isBlocked(QStringLiteral("@new:h")),
                 "a block made on another device did not arrive through sync");
        QCOMPARE(m.count(), 2);
        QCOMPARE(rec.fetches, 0);  // no round trip of our own was needed
    }

    // …but not while we have a write outstanding. The document /sync built
    // may predate our PUT, and adopting it would put a user we just unblocked
    // back in the list — and then build the NEXT full replacement from it,
    // which is how a full-replacement document silently un-blocks people.
    void aPushedDocumentNeverOverwritesAWriteInFlight()
    {
        BlockedUsersModel m;
        Recorder rec;
        rec.install(m);
        m.setSelfUserId(QStringLiteral("@me:h"));
        m.onDocument(obj("{'ignored_users':{'@old:h':{},'@spammer:h':{}}}"));

        m.unblock(QStringLiteral("@spammer:h"));
        QCOMPARE(rec.documents.size(), 1);

        // The server's pre-unblock copy arrives while the PUT is in flight.
        QVERIFY2(!m.onDocumentFromSync(obj("{'ignored_users':{'@old:h':{},'@spammer:h':{}}}")),
                 "a pushed document was adopted over a write in flight");
        QVERIFY2(!m.isBlocked(QStringLiteral("@spammer:h")),
                 "the unblock the user just made was undone by a sync push");

        // Our own reply is the newer truth, and it settles the row.
        m.onWriteStored(rec.lastRequestId(), rec.lastDocument());
        QVERIFY(!m.isBlocked(QStringLiteral("@spammer:h")));
        QCOMPARE(m.count(), 1);
    }

    // An intent recorded before the list had ever been read must survive a
    // push too: pump() is waiting for a document to build a replacement from,
    // and adopting one here would race the write it is about to make.
    void aPushedDocumentIsDeclinedWhileAnIntentIsWaiting()
    {
        BlockedUsersModel m;
        Recorder rec;
        rec.install(m);
        m.block(QStringLiteral("@spammer:h"));
        QCOMPARE(rec.documents.size(), 0);  // nothing to build from yet

        QVERIFY(!m.onDocumentFromSync(obj("{'ignored_users':{}}")));
        QVERIFY(m.isBlocked(QStringLiteral("@spammer:h")));
    }
};

QTEST_MAIN(UgcSafetyTest)
#include "test_ugc_safety.moc"
