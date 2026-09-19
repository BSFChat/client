// WHEN the upload failure signals fire, as opposed to what they say.
//
// tests/test_error_surface_guards.cpp pins the wording of every error surface
// and the rule that a guard must never drop a signal, because MessageInput's
// `_inFlightUploads` count is cleared by that signal and nothing else. This
// file is the other half of the same counter: a signal that fires is no use
// if it fires at a moment when nobody is counting yet.
//
// The defect, in the composer's own terms. MessageInput.qml starts an upload
// like this, at the attach button, at paste, and in MessageView's drop area:
//
//     serverManager.activeServer.sendMediaMessage(url);   // 1
//     inputRoot.noteUploadStarted();                      // 2
//
// Every failure that reaches the user through a reply handler arrives long
// after line 2, so the decrement lands on a count of 1 and the composer
// unlocks. The two PRE-FLIGHT checks inside sendMediaMessage — no active
// room, and a file that will not open — used to emit mediaSendFailed
// synchronously, which put the decrement BETWEEN lines 1 and 2, on a count of
// zero. `_noteUploadFinished` clamps at zero, so that decrement was dropped
// on the floor; line 2 then incremented a count nothing would ever decrement
// again. The composer stayed disabled, reading "Uploading…", with nothing in
// flight, until the user switched channel and back (a room-key change calls
// `_resetUploads`). Picking an unreadable file out of the attach dialog was
// enough to do it.
//
// Fixed in ServerConnection, not in the three QML call sites: see
// emitPreflightMediaFailure. The invariant that fix establishes, and that
// this file exists to hold, is
//
//     sendMediaMessage NEVER emits mediaSendFailed before it returns.
//
// so that a caller which counts after calling — the natural way to write it,
// and what all three sites do — cannot lose the decrement. The two cases
// below assert the invariant directly; the two after them assert the thing
// the user feels, by driving the real connection through a stand-in for
// MessageInput's bookkeeping and checking the composer comes back.
//
// No GUI and no server, for test_error_surface_guards' reasons: both
// pre-flight paths return before a single byte goes on the wire.

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
// is wired: one decrement per completion, one per failure, and nothing else.
void attachComposer(ComposerBookkeeping* composer, ServerConnection* conn)
{
    QObject::connect(conn, &ServerConnection::mediaSendCompleted, composer,
                     &ComposerBookkeeping::noteUploadFinished);
    QObject::connect(conn, &ServerConnection::mediaSendFailed, composer,
                     [composer](const QString&) { composer->noteUploadFinished(); });
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

    // ── The invariant ─────────────────────────────────────────────────────

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

    // ── What the user feels ───────────────────────────────────────────────

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
};

// No QGuiApplication: a window system on a dev Mac means permission dialogs,
// which is the thing every test in this directory avoids.
QTEST_GUILESS_MAIN(TestComposerUploadLock)
#include "test_composer_upload_lock.moc"
