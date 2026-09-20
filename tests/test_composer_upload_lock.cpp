// TWO ways the composer's upload counter has been corrupted, and the counter
// they share.
//
// MessageInput.qml derives the composer's disabled "Uploading…" state from a
// count of the uploads IT started:
//
//     property int _inFlightUploads: 0
//     readonly property bool uploading: _inFlightUploads > 0
//
// and it keeps that count balanced from a Connections block listening to
// mediaSendCompleted and mediaSendFailed. Both are bare signals — a QString of
// error text at most — which makes the bookkeeping correct only as long as
// two things hold: every decrement is for an upload this composer started,
// and no decrement arrives before the matching increment. Each of those was
// violated once, by unrelated code, and from the user's chair the two defects
// are indistinguishable: the composer stops saying "Uploading…" when it
// should not, or never stops saying it. Both are filed under U-H6's symptom.
// They are tested together because the counter is one counter.
//
// tests/test_error_surface_guards.cpp is the third file on this surface. It
// pins what these signals SAY and the rule that a guard must never drop one.
// This file is about when they fire and whose upload they describe.
//
// ── Defect 1: ORDER. A decrement before the increment. ────────────────────
//
// All three call sites start an upload like this — the attach button, paste,
// and MessageView's drop area:
//
//     serverManager.activeServer.sendMediaMessage(url);   // 1
//     inputRoot.noteUploadStarted();                      // 2
//
// Every failure arriving through a reply handler lands long after line 2, on
// a count of 1, and the composer unlocks correctly. The two PRE-FLIGHT checks
// inside sendMediaMessage — no active room, and a file that will not open —
// used to emit mediaSendFailed synchronously, which put the decrement BETWEEN
// lines 1 and 2, on a count of zero. `_noteUploadFinished` clamped at zero, so
// that decrement was dropped; line 2 then incremented a count nothing would
// ever decrement again. The composer sat disabled reading "Uploading…" with
// nothing in flight, recoverable only by leaving the channel and coming back
// (a room-key change calls `_resetUploads`). Picking an unreadable file out of
// the attach dialog was enough to do it.
//
// Fixed in ServerConnection, not in the three QML call sites: see
// emitPreflightMediaFailure. The invariant that fix establishes, and that the
// first group of cases below exists to hold, is
//
//     sendMediaMessage NEVER emits mediaSendFailed before it returns.
//
// so that a caller which counts after calling — the natural way to write it,
// and what all three sites do — cannot lose the decrement.
//
// ── Defect 2: OWNERSHIP. A decrement for somebody else's upload. ──────────
//
// A bare signal says an upload failed and nothing about WHICH one, so the
// composer credits every mediaSendFailed it sees to itself; it has no way to
// tell a stranger's failure from its own. That was fine while the composer's
// attachment was the only thing emitting them, and it stopped being fine when
// ServerConnection::uploadAvatar and ServerConnection::uploadServerAvatar
// routed their reply handlers through emitMediaSendFailed as well. Failing to
// set an avatar or a server icon WHILE an attachment was uploading decremented
// a count those uploads had never incremented: the composer unlocked early and
// dropped "Uploading…" with the user's file still on the wire.
//
// The same clamp hid this one too, in its milder form — an avatar failure with
// no attachment in flight left no trace at all. Its damaging form, against a
// count above zero, the clamp could never have caught.
//
// Note the asymmetry that made the borrowing one-way. Neither avatar path
// emits mediaSendCompleted; success there continues into setRoomState /
// updateAvatarUrl and the user simply sees the new image. So the composer
// could only ever be handed a decrement, never a matching increment, which is
// the shape of a bug rather than of a shared lifecycle — and the reason the
// fix is to split the signal rather than to also share the completion.
//
// Fixed with a signal of the avatar uploads' own, avatarUploadFailed, carrying
// the same 401 text guard (see emitAvatarUploadFailed) and the same toast
// behind it in main.qml. The invariant the second group of cases holds is
//
//     mediaSendFailed / mediaSendCompleted are the COMPOSER's, and an upload
//     the composer did not start never arrives on either.
//
// ── ───────────────────────────────────────────────────────────────────────
//
// No GUI and no server for any of it. The pre-flight cases return before a
// byte goes on the wire; the ownership cases aim real requests at a port
// nothing is listening on, which is a real failed request and the path that
// matters there.

#include <QtTest/QtTest>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

#include "model/BotAdminModel.h"  // ServerConnection exposes a BotAdminModel*
                                   // Q_PROPERTY; moc needs the complete type.
#include "net/MatrixClient.h"
#include "net/ServerConnection.h"

namespace {

// Never listening, so nothing here can reach anything.
const auto kServer = QStringLiteral("http://127.0.0.1:1");
const auto kRoom = QStringLiteral("!room:example.org");

// MessageInput.qml's upload bookkeeping, transcribed. Deliberately a
// transcription and not an approximation: the clamp is the part that made the
// bug silent, so it is reproduced exactly, and the composer's `enabled` is
// derived from the count exactly as the QML derives it.
//
// It records unmatched decrements rather than swallowing them, which is the
// one thing the QML cannot do silently — see the console.warn this mirrors.
class ComposerBookkeeping : public QObject
{
    Q_OBJECT
public:
    int inFlight = 0;
    int unmatchedDecrements = 0;

    // readonly property bool uploading: _inFlightUploads > 0
    bool composerEnabled() const { return inFlight == 0; }

    void noteUploadStarted() { ++inFlight; }

public slots:
    void noteUploadFinished()
    {
        if (inFlight > 0) { --inFlight; return; }
        ++unmatchedDecrements;
    }
};

// Wire a bookkeeper to a connection the way MessageInput's Connections block
// is wired: one decrement per mediaSendCompleted, one per mediaSendFailed,
// and nothing else. In particular it does NOT listen to avatarUploadFailed,
// and the composer in the real QML does not either — that is defect 2's fix.
void attachComposer(ComposerBookkeeping* composer, ServerConnection* conn)
{
    QObject::connect(conn, &ServerConnection::mediaSendCompleted, composer,
                     &ComposerBookkeeping::noteUploadFinished);
    QObject::connect(conn, &ServerConnection::mediaSendFailed, composer,
                     [composer](const QString&) { composer->noteUploadFinished(); });
}

// A signed-in connection. uploadAvatar returns early on an empty user id, so
// without this the ownership cases would pass by never uploading anything.
void signIn(ServerConnection* conn)
{
    conn->setCredentials(QStringLiteral("@josh:example.org"),
                         QStringLiteral("syt_live_token"),
                         QStringLiteral("DEV"), QStringLiteral("josh"));
}

QString writeImage(const QTemporaryDir& dir, const QString& name)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    f.write(QByteArray(64, 'x'));
    f.close();
    return path;
}

} // namespace

class TestComposerUploadLock : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // ServerConnection's constructor hydrates from QSettings and opens a
        // LocalCache. Keep both out of the developer's real profile.
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("BSFChatTest"));
        QCoreApplication::setApplicationName(
            QStringLiteral("test_composer_upload_lock"));
    }

    // ── Defect 1: the ordering invariant ─────────────────────────────────

    void noActiveRoomFailsAfterTheCallReturns()
    {
        ServerConnection conn(kServer);
        QSignalSpy spy(&conn, &ServerConnection::mediaSendFailed);

        conn.sendMediaMessage(QStringLiteral("file:///tmp/whatever.png"));

        // The whole fix, in one assertion: the caller gets control back
        // before the failure is reported, so whatever it does on the line
        // after the call has already happened when the signal lands.
        QCOMPARE(spy.count(), 0);
        QVERIFY(spy.wait(2000));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toString(), QStringLiteral("No active room"));
    }

    void unreadableFileFailsAfterTheCallReturns()
    {
        ServerConnection conn(kServer);
        conn.setActiveRoom(kRoom);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString missing = dir.filePath(QStringLiteral("gone.png"));

        QSignalSpy spy(&conn, &ServerConnection::mediaSendFailed);
        conn.sendMediaMessage(QUrl::fromLocalFile(missing).toString());

        QCOMPARE(spy.count(), 0);
        QVERIFY(spy.wait(2000));
        QCOMPARE(spy.count(), 1);
        QVERIFY2(spy.first().at(0).toString().contains(QStringLiteral("gone.png")),
                 "deferring the emit must not change what it says");
    }

    // ── Defect 1: what the user feels ────────────────────────────────────

    void composerIsTypeableAgainAfterPickingAnUnreadableFile()
    {
        // The reported reproduction: attach button, choose a file that will
        // not open. Before the fix the composer never came back in that
        // channel.
        ServerConnection conn(kServer);
        conn.setActiveRoom(kRoom);
        ComposerBookkeeping composer;
        attachComposer(&composer, &conn);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString missing = dir.filePath(QStringLiteral("gone.png"));

        // MessageInput.qml's fileDialog.onAccepted, in order.
        conn.sendMediaMessage(QUrl::fromLocalFile(missing).toString());
        composer.noteUploadStarted();

        QTRY_VERIFY_WITH_TIMEOUT(composer.composerEnabled(), 2000);
        QCOMPARE(composer.unmatchedDecrements, 0);
    }

    void composerIsTypeableAgainWithNoActiveRoom()
    {
        ServerConnection conn(kServer);
        ComposerBookkeeping composer;
        attachComposer(&composer, &conn);

        conn.sendMediaMessage(QStringLiteral("file:///tmp/whatever.png"));
        composer.noteUploadStarted();

        QTRY_VERIFY_WITH_TIMEOUT(composer.composerEnabled(), 2000);
        QCOMPARE(composer.unmatchedDecrements, 0);
    }

    void aMultiFileDropUnlocksOnlyOnceEveryFileHasFailed()
    {
        // MessageView.qml's drop handler is the call site that gets the
        // ordering most wrong: it sends every file first and only then
        // increments once per file. Three unreadable files therefore emit
        // three failures with the count still at zero. It must still end up
        // unlocked and balanced — and must NOT unlock early, which is what
        // U-H6 was about.
        ServerConnection conn(kServer);
        conn.setActiveRoom(kRoom);
        ComposerBookkeeping composer;
        attachComposer(&composer, &conn);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QStringList names{QStringLiteral("a.png"), QStringLiteral("b.png"),
                                QStringLiteral("c.png")};
        for (const auto& n : names)
            conn.sendMediaMessage(QUrl::fromLocalFile(dir.filePath(n)).toString());
        for (int i = 0; i < names.size(); ++i) composer.noteUploadStarted();

        // All three increments land before any failure is delivered.
        QCOMPARE(composer.inFlight, names.size());
        QVERIFY(!composer.composerEnabled());

        QTRY_VERIFY_WITH_TIMEOUT(composer.composerEnabled(), 2000);
        QCOMPARE(composer.unmatchedDecrements, 0);
    }

    void aMixedDropCountsTheFilesItSendsRatherThanTheOnesThatWork()
    {
        // The drop area's worst case, and the one that says whether it is
        // genuinely covered or merely lucky. It increments once per file it
        // SENT, not per file that will succeed, so a drop mixing a readable
        // and an unreadable file produces two increments and two terminal
        // signals of different kinds arriving at different times — one
        // deferred pre-flight failure and one real request against a dead
        // port. Both must land after both increments, and the pair must
        // balance exactly.
        //
        // The all-unreadable case above passes even if the deferral only
        // happens to hold, because nothing else is in flight to be corrupted.
        // Here the pre-flight failure lands with a real upload's count beside
        // it, so a regression would show up as the composer unlocking while
        // the readable file is still going.
        ServerConnection conn(kServer);
        signIn(&conn);
        conn.setActiveRoom(kRoom);
        ComposerBookkeeping composer;
        attachComposer(&composer, &conn);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString readable = writeImage(dir, QStringLiteral("holiday.png"));
        QVERIFY(!readable.isEmpty());
        const QString unreadable = dir.filePath(QStringLiteral("gone.png"));

        // MessageView.qml's onDropped, verbatim in shape: every send first,
        // then one increment per send.
        conn.sendMediaMessage(QUrl::fromLocalFile(unreadable).toString());
        conn.sendMediaMessage(QUrl::fromLocalFile(readable).toString());
        composer.noteUploadStarted();
        composer.noteUploadStarted();

        QCOMPARE(composer.inFlight, 2);
        QVERIFY2(!composer.composerEnabled(),
                 "nothing may have been reported while the handler was running");

        // The pre-flight failure arrives first, on a count of 2. If the
        // composer unlocks here the readable file is still on the wire.
        QTRY_VERIFY_WITH_TIMEOUT(composer.inFlight < 2, 2000);
        QVERIFY2(!composer.composerEnabled(),
                 "one file failing pre-flight must not unlock the composer "
                 "while the other is still uploading");
        QCOMPARE(composer.unmatchedDecrements, 0);

        QTRY_VERIFY_WITH_TIMEOUT(composer.composerEnabled(), 8000);
        QCOMPARE(composer.inFlight, 0);
        QCOMPARE(composer.unmatchedDecrements, 0);
    }

    void aSucceedingSendStillLocksTheComposerWhileItRuns()
    {
        // The guard against "fix the lockout by never locking". A send that
        // passes pre-flight must leave the composer disabled until its reply
        // comes back — here, a connection refused by port 1.
        ServerConnection conn(kServer);
        conn.setCredentials(QStringLiteral("@josh:example.org"),
                            QStringLiteral("syt_live_token"),
                            QStringLiteral("DEV"), QStringLiteral("josh"));
        conn.setActiveRoom(kRoom);
        ComposerBookkeeping composer;
        attachComposer(&composer, &conn);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("clip.mp4"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(64, 'x'));
        f.close();

        conn.sendMediaMessage(QUrl::fromLocalFile(path).toString());
        composer.noteUploadStarted();
        QVERIFY2(!composer.composerEnabled(),
                 "a real upload must hold the composer while it is in flight");

        QTRY_VERIFY_WITH_TIMEOUT(composer.composerEnabled(), 5000);
        QCOMPARE(composer.unmatchedDecrements, 0);
    }
    // ── Defect 2: the signal is the avatar's own ─────────────────────────

    void aFailedAvatarUploadDoesNotEmitMediaSendFailed()
    {
        ServerConnection conn(kServer);
        signIn(&conn);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeImage(dir, QStringLiteral("face.png"));
        QVERIFY(!path.isEmpty());

        QSignalSpy avatarSpy(&conn, &ServerConnection::avatarUploadFailed);
        QSignalSpy composerSpy(&conn, &ServerConnection::mediaSendFailed);

        conn.uploadAvatar(QUrl::fromLocalFile(path).toString());

        QVERIFY2(avatarSpy.wait(5000), "the avatar failure must still be reported");
        QCOMPARE(avatarSpy.count(), 1);
        // The whole fix, in one assertion.
        QCOMPARE(composerSpy.count(), 0);

        // The split is about routing, not wording: the same failure on the
        // composer's own path must produce the same text, because main.qml
        // toasts both with the same prefix. (Against a dead port that text is
        // the empty response body for both — MatrixClient passes the body
        // through verbatim, see its mediaUploadError emit. Asserting equality
        // rather than non-emptiness keeps this test honest about that and
        // still fails the day one path starts rewording.)
        ServerConnection composerConn(kServer);
        signIn(&composerConn);
        composerConn.setActiveRoom(kRoom);
        QSignalSpy sameFailure(&composerConn, &ServerConnection::mediaSendFailed);
        composerConn.sendMediaMessage(QUrl::fromLocalFile(path).toString());
        QVERIFY(sameFailure.wait(5000));
        QCOMPARE(avatarSpy.first().at(0).toString(),
                 sameFailure.first().at(0).toString());
    }

    void aFailedServerIconUploadDoesNotEmitMediaSendFailed()
    {
        ServerConnection conn(kServer);
        signIn(&conn);
        conn.setActiveRoom(kRoom);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeImage(dir, QStringLiteral("icon.png"));
        QVERIFY(!path.isEmpty());

        QSignalSpy avatarSpy(&conn, &ServerConnection::avatarUploadFailed);
        QSignalSpy composerSpy(&conn, &ServerConnection::mediaSendFailed);

        conn.uploadServerAvatar(QUrl::fromLocalFile(path).toString());

        QVERIFY(avatarSpy.wait(5000));
        QCOMPARE(avatarSpy.count(), 1);
        QCOMPARE(composerSpy.count(), 0);
    }

    // ── Defect 2: what the user feels ────────────────────────────────────

    void aFailedAvatarUploadLeavesTheComposerCountAlone()
    {
        // The reported shape: nothing in the composer, a failed avatar. Before
        // the fix this was a decrement on a count of zero — dropped by the
        // clamp, so it corrupted nothing yet, but it is the same unmatched
        // decrement that corrupts a non-zero count in the case below. It is
        // also what the console.warn added on fix/composer-upload-lockout now
        // fires on.
        ServerConnection conn(kServer);
        signIn(&conn);
        ComposerBookkeeping composer;
        attachComposer(&composer, &conn);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeImage(dir, QStringLiteral("face.png"));
        QVERIFY(!path.isEmpty());

        QSignalSpy avatarSpy(&conn, &ServerConnection::avatarUploadFailed);
        conn.uploadAvatar(QUrl::fromLocalFile(path).toString());
        QVERIFY(avatarSpy.wait(5000));

        QCOMPARE(composer.inFlight, 0);
        QCOMPARE(composer.unmatchedDecrements, 0);
        QVERIFY(composer.composerEnabled());
    }

    void anAvatarFailureMidAttachmentDoesNotUnlockTheComposer()
    {
        // The damaging shape, and the one the clamp could never have caught:
        // the count is above zero, so the stray decrement lands on the
        // attachment's own tally. The composer unlocked and stopped reading
        // "Uploading…" while the attachment was still going.
        ServerConnection conn(kServer);
        signIn(&conn);
        conn.setActiveRoom(kRoom);
        ComposerBookkeeping composer;
        attachComposer(&composer, &conn);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString attachment = writeImage(dir, QStringLiteral("clip.mp4"));
        const QString avatar = writeImage(dir, QStringLiteral("face.png"));
        QVERIFY(!attachment.isEmpty() && !avatar.isEmpty());

        // MessageInput.qml's attach path: send, then count.
        conn.sendMediaMessage(QUrl::fromLocalFile(attachment).toString());
        composer.noteUploadStarted();
        QCOMPARE(composer.inFlight, 1);
        QVERIFY(!composer.composerEnabled());

        // UserSettings.qml, while that is in flight.
        QSignalSpy avatarSpy(&conn, &ServerConnection::avatarUploadFailed);
        conn.uploadAvatar(QUrl::fromLocalFile(avatar).toString());
        QVERIFY(avatarSpy.wait(5000));

        // The attachment is somebody's file and it is still the composer's
        // business. Whether its own reply has landed yet is a race against the
        // avatar's, so assert the part that is not racy: the avatar's failure
        // cannot have been the thing that emptied the count.
        QCOMPARE(composer.unmatchedDecrements, 0);
        QVERIFY2(composer.inFlight + composer.unmatchedDecrements <= 1,
                 "the avatar failure must not decrement the composer's count");

        // And once the attachment's own failure lands, the count balances
        // exactly — no leftover, no double-decrement.
        QTRY_VERIFY_WITH_TIMEOUT(composer.composerEnabled(), 5000);
        QCOMPARE(composer.inFlight, 0);
        QCOMPARE(composer.unmatchedDecrements, 0);
    }

    void severalAvatarFailuresCannotDrainAMultiFileUpload()
    {
        // The count only has to be driven below the number actually in flight
        // for the composer to unlock early, so the worst case is several
        // strangers against one attachment batch. Three avatar failures used
        // to take a three-file drop from 3 to 0.
        ServerConnection conn(kServer);
        signIn(&conn);
        conn.setActiveRoom(kRoom);
        ComposerBookkeeping composer;
        attachComposer(&composer, &conn);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        // Three attachments in flight, counted the way MessageView's drop area
        // counts them: every send first, then one increment per file.
        const QStringList names{QStringLiteral("a.png"), QStringLiteral("b.png"),
                                QStringLiteral("c.png")};
        for (const auto& n : names) {
            const QString p = writeImage(dir, n);
            QVERIFY(!p.isEmpty());
            conn.sendMediaMessage(QUrl::fromLocalFile(p).toString());
        }
        for (int i = 0; i < names.size(); ++i) composer.noteUploadStarted();
        QCOMPARE(composer.inFlight, names.size());

        QSignalSpy avatarSpy(&conn, &ServerConnection::avatarUploadFailed);
        for (int i = 0; i < 3; ++i) {
            const QString p = writeImage(dir, QStringLiteral("face%1.png").arg(i));
            QVERIFY(!p.isEmpty());
            conn.uploadAvatar(QUrl::fromLocalFile(p).toString());
        }
        QTRY_COMPARE_WITH_TIMEOUT(avatarSpy.count(), 3, 8000);

        // Three strangers failed and took nothing with them.
        QCOMPARE(composer.unmatchedDecrements, 0);
        QTRY_VERIFY_WITH_TIMEOUT(composer.composerEnabled(), 8000);
        QCOMPARE(composer.inFlight, 0);
        QCOMPARE(composer.unmatchedDecrements, 0);
    }

    // ── Defect 2: the guard against fixing it by breaking it ───────────────

    void theComposersOwnFailureStillReachesTheComposer()
    {
        // Scoping the avatars out must not scope the attachment out with them.
        // mediaSendFailed is still what clears the composer, and a swallowed
        // one is the lockout emitMediaSendFailed's comment is about.
        ServerConnection conn(kServer);
        signIn(&conn);
        conn.setActiveRoom(kRoom);
        ComposerBookkeeping composer;
        attachComposer(&composer, &conn);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeImage(dir, QStringLiteral("clip.mp4"));
        QVERIFY(!path.isEmpty());

        QSignalSpy avatarSpy(&conn, &ServerConnection::avatarUploadFailed);
        conn.sendMediaMessage(QUrl::fromLocalFile(path).toString());
        composer.noteUploadStarted();
        QVERIFY2(!composer.composerEnabled(),
                 "a real upload must hold the composer while it is in flight");

        QTRY_VERIFY_WITH_TIMEOUT(composer.composerEnabled(), 8000);
        QCOMPARE(composer.unmatchedDecrements, 0);
        QVERIFY2(avatarSpy.count() == 0,
                 "an attachment must not report itself on the avatar signal");
    }
};

// No QGuiApplication: a window system on a dev Mac means permission dialogs,
// which is the thing every test in this directory avoids.
QTEST_GUILESS_MAIN(TestComposerUploadLock)
#include "test_composer_upload_lock.moc"
