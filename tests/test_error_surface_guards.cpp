// What each user-visible error surface is allowed to say once the homeserver
// has stopped accepting our token.
//
// Continues tests/test_session_auth.cpp (the state machine) and
// tests/test_token_rejection.cpp (the 401 detection) into the last question
// the 2026-09-19 purge left open: every subsystem kept driving the dead token
// and reported its own 401 on its own surface, so what the user saw was a
// wall of {"errcode":"M_UNKNOWN_TOKEN",...} sitting next to a banner that
// already told them the truth. sendFeedback and voiceError were fixed first.
// These are the rest, and unlike those two they are NOT one surface with one
// right answer — hence a test per decision rather than one sweeping check.
//
// Two rules are pinned here, and the second is the one that bites:
//
//   1. The text a dead session produces is the sign-in sentence, not the
//      server's error object — except where the server's words are still the
//      truth (a local pre-flight failure), and except where the sign-in
//      sentence is the wrong sentence for the place it lands (the search
//      pane, which gets its own wording).
//
//   2. The SIGNAL still fires. Every one of these surfaces is the only thing
//      that clears some in-flight state: the settings pane's pending save,
//      SearchPopup's `searching` flag, MessageInput's `_inFlightUploads`
//      count — which, while it is above zero, disables the composer and shows
//      "Uploading…". A guard that returned early instead of substituting
//      would turn a cosmetic problem into one the user cannot type their way
//      out of. Each case below therefore asserts the emission count as well
//      as the text.
//
// No GUI, no server: a ServerConnection reaches the network only when it is
// given credentials, an EMPTY access token puts it straight into the expired
// state (ServerManager blanks the stored token on expiry, so this is the
// relaunch path rather than a contrivance), and MatrixClient's reply signals
// are public, so the failure paths can be driven by hand. The one case that
// cannot be — a real upload's reply handler, whose token is minted inside
// sendMediaMessage — is driven against a refused port on localhost instead.

#include <QtTest/QtTest>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

#include "model/BotAdminModel.h"  // ServerConnection exposes a BotAdminModel*
                                   // Q_PROPERTY; moc needs the complete type.
#include "net/MatrixClient.h"
#include "net/ServerConnection.h"
#include "util/SearchParser.h"  // SearchResponse, for the error-payload case

namespace {

// Port 1 is never listening, so every stray request this file provokes is
// refused immediately rather than hanging or, worse, reaching something.
const auto kServer = QStringLiteral("http://127.0.0.1:1");
const auto kRoom = QStringLiteral("!room:example.org");
const auto kUser = QStringLiteral("@josh:example.org");

// What the homeserver actually returned during the purge, and what every one
// of these surfaces printed at the user verbatim.
const auto kRawToken = QStringLiteral(
    "{\"errcode\":\"M_UNKNOWN_TOKEN\",\"error\":\"Invalid or missing access token\"}");

// The banner's sentence. Guarded toast-like surfaces reuse it exactly.
const auto kSignIn =
    QStringLiteral("Your session has expired. Sign in again to reconnect.");

// A connection whose token the server has already rejected, without a single
// byte on the wire: setCredentials with an empty token is the restored-entry
// path and notes the rejection itself.
std::unique_ptr<ServerConnection> deadSession()
{
    auto conn = std::make_unique<ServerConnection>(kServer);
    conn->setCredentials(kUser, QString(), QStringLiteral("DEV"),
                         QStringLiteral("josh"));
    return conn;
}

// A connection nothing has complained about yet.
std::unique_ptr<ServerConnection> liveSession()
{
    return std::make_unique<ServerConnection>(kServer);
}

} // namespace

class TestErrorSurfaceGuards : public QObject
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
            QStringLiteral("test_error_surface_guards"));
    }

    void deadSessionIsRecognisedWithoutANetwork()
    {
        // The premise of every case below. If this ever stops being true the
        // rest of the file is vacuously green.
        auto conn = deadSession();
        QVERIFY(conn->needsReauth());
        QVERIFY(!liveSession()->needsReauth());
    }

    // ── stateWriteFailed ──────────────────────────────────────────────────
    //
    // Guarded by replacing ONLY the server's string. `kind` is what both
    // receivers (main.qml's toast and ServerSettings.qml's inline banner)
    // switch on to say which save failed, and `status` is what they branch on
    // for the 403 phrasing. Replace the lot and the user is told a session
    // expired but not that it was their server rename that died.

    void expiredStateWriteKeepsItsKindAndStatusAndLosesTheMatrixObject()
    {
        auto conn = deadSession();
        QSignalSpy spy(conn.get(), &ServerConnection::stateWriteFailed);

        emit conn->client()->stateEventError(
            kRoom, QStringLiteral("bsfchat.server.info"), QString(), 401,
            kRawToken);

        // Fired, once. The settings pane treats this as the end of the write.
        QCOMPARE(spy.count(), 1);
        const auto args = spy.first();
        // The two fields the wording is built from survive untouched.
        QCOMPARE(args.at(0).toString(), QStringLiteral("server-name"));
        QCOMPARE(args.at(1).toInt(), 401);
        // The third does not.
        QCOMPARE(args.at(2).toString(), kSignIn);
        QVERIFY(!args.at(2).toString().contains(QStringLiteral("M_UNKNOWN_TOKEN")));
    }

    void everyStateWriteKindStillIdentifiesItselfWhenExpired()
    {
        // The per-kind wording is the whole reason this surface was not
        // swept in with a blanket substitution; prove the tag survives on
        // each of the five the handler knows about.
        auto conn = deadSession();
        QSignalSpy spy(conn.get(), &ServerConnection::stateWriteFailed);

        const QList<QPair<QString, QString>> cases = {
            {QStringLiteral("bsfchat.member.roles"), QStringLiteral("role-assign")},
            {QStringLiteral("bsfchat.server.info"), QStringLiteral("server-name")},
            {QStringLiteral("bsfchat.channel.permissions"), QStringLiteral("channel-override")},
            {QStringLiteral("bsfchat.channel.settings"), QStringLiteral("channel-settings")},
            {QStringLiteral("bsfchat.server.roles"), QStringLiteral("server-roles")},
        };
        for (const auto& c : cases) {
            emit conn->client()->stateEventError(kRoom, c.first, QString(), 401,
                                                 kRawToken);
        }

        QCOMPARE(spy.count(), cases.size());
        for (int i = 0; i < cases.size(); ++i) {
            QCOMPARE(spy.at(i).at(0).toString(), cases.at(i).second);
            QCOMPARE(spy.at(i).at(2).toString(), kSignIn);
        }
    }

    void healthyStateWriteStillCarriesTheServersOwnReason()
    {
        // The guard must not eat a 403 from an ordinary permission failure —
        // that message is the only thing telling a moderator why the save
        // bounced.
        auto conn = liveSession();
        QSignalSpy spy(conn.get(), &ServerConnection::stateWriteFailed);

        const auto reason = QStringLiteral("You don't have permission to do that");
        emit conn->client()->stateEventError(
            kRoom, QStringLiteral("bsfchat.member.roles"), QString(), 403, reason);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toString(), QStringLiteral("role-assign"));
        QCOMPARE(spy.first().at(1).toInt(), 403);
        QCOMPARE(spy.first().at(2).toString(), reason);
    }

    // ── searchErrored ─────────────────────────────────────────────────────
    //
    // Guarded, but with a different sentence. This one replaces the results
    // list inside an open popup and stays there until the next query; the
    // banner's wording reads as an answer to the search rather than to the
    // session, and repeats a line already on screen.

    void expiredSearchExplainsItselfInTheSearchPanesOwnWords()
    {
        auto conn = deadSession();
        QSignalSpy spy(conn.get(), &ServerConnection::searchErrored);

        conn->searchMessages(QStringLiteral("standup"));
        emit conn->client()->searchFailed(QStringLiteral("standup"), kRawToken);

        // Fires — SearchPopup clears `searching` here and in exactly one
        // other handler. Swallowed, the popup spins forever.
        QCOMPARE(spy.count(), 1);
        const auto msg = spy.first().at(0).toString();
        QVERIFY(!msg.isEmpty());
        QVERIFY(!msg.contains(QStringLiteral("M_UNKNOWN_TOKEN")));
        // Written for the hole where results go, not lifted from the banner.
        QVERIFY2(msg != kSignIn,
                 "the inline pane should not repeat the banner verbatim");
        QVERIFY(msg.contains(QStringLiteral("sign in"), Qt::CaseInsensitive));
    }

    void expiredSearchErrorPayloadIsAlsoGuarded()
    {
        // The other half of the surface: a 401 that arrives as an error
        // PAYLOAD on a 200-shaped reply rather than as a transport failure.
        // Both emit sites have to go through the same chokepoint, and this is
        // the one a future refactor is most likely to miss.
        auto conn = deadSession();
        QSignalSpy spy(conn.get(), &ServerConnection::searchErrored);

        conn->searchMessages(QStringLiteral("standup"));
        bsfchat::client::SearchResponse resp;
        resp.ok = false;
        resp.errorMessage = kRawToken;
        emit conn->client()->searchResult(QStringLiteral("standup"), QString(), resp);

        QCOMPARE(spy.count(), 1);
        QVERIFY(!spy.first().at(0).toString().contains(
            QStringLiteral("M_UNKNOWN_TOKEN")));
    }

    void healthySearchFailureKeepsTheParsersPlainLanguage()
    {
        // SearchParser already maps errcodes into human wording; a live
        // session must still get it rather than a session sentence.
        auto conn = liveSession();
        QSignalSpy spy(conn.get(), &ServerConnection::searchErrored);

        const auto reason = QStringLiteral("Search is taking too long — try a narrower term");
        conn->searchMessages(QStringLiteral("standup"));
        emit conn->client()->searchFailed(QStringLiteral("standup"), reason);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toString(), reason);
    }

    // ── notifyLevelFailed ─────────────────────────────────────────────────
    //
    // GUARDED, since it acquired a receiver. It had none when the other four
    // were decided — no QML handler, no C++ slot, no test — so the server's
    // text reached no screen and substituting it would have guarded nothing.
    // main.qml now toasts it, because the rollback it accompanies corrects a
    // context menu that has already closed and so tells the user nothing.
    // A toast is the same kind of hole sendFeedback lands in, so it gets the
    // banner's sentence verbatim rather than the search pane's wording.
    //
    // The rollback is still the half that matters, and both halves are
    // asserted here: a guard must not cost the user the restored setting.

    void expiredNotifyLevelWriteRollsBackAndSaysTheSessionExpired()
    {
        auto conn = deadSession();
        QSignalSpy changed(conn.get(), &ServerConnection::notifyLevelChanged);
        QSignalSpy failed(conn.get(), &ServerConnection::notifyLevelFailed);

        conn->setRoomNotifyLevel(kRoom, QStringLiteral("none"));
        QCOMPARE(changed.count(), 1);  // optimistic local write

        emit conn->client()->roomNotifyLevelError(kRoom, kRawToken);

        // The rollback is the message, and it is the part that must not be
        // lost: a menu checkmark left on a setting the server never took is a
        // lie that outlives the session.
        QCOMPARE(changed.count(), 2);
        QCOMPARE(changed.at(1).at(1).toString(), QStringLiteral("none"));

        // And the report fires with the session sentence rather than the
        // raw Matrix object main.qml would otherwise put in a toast next to
        // a banner already saying the same thing in English.
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().at(0).toString(), kRoom);
        QCOMPARE(failed.first().at(1).toString(), kSignIn);
    }

    void healthyNotifyLevelFailureKeepsTheServersOwnReason()
    {
        // The guard must not eat an ordinary refusal — a 403 on a channel a
        // moderator has locked down is the only thing explaining why the
        // choice bounced.
        auto conn = liveSession();
        QSignalSpy changed(conn.get(), &ServerConnection::notifyLevelChanged);
        QSignalSpy failed(conn.get(), &ServerConnection::notifyLevelFailed);

        const auto reason = QStringLiteral("You don't have permission to do that");
        conn->setRoomNotifyLevel(kRoom, QStringLiteral("none"));
        emit conn->client()->roomNotifyLevelError(kRoom, reason);

        QCOMPARE(changed.count(), 2);  // optimistic write, then the rollback
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().at(1).toString(), reason);
    }

    // ── mediaSendFailed ───────────────────────────────────────────────────
    //
    // Guarded on the three call sites that report a failed REQUEST, left
    // alone on the two pre-flight checks. The split matters because the
    // pre-flight text describes something a fresh sign-in would not change.

    void expiredUploadReportsTheSessionAndStillUnlocksTheComposer()
    {
        auto conn = deadSession();
        conn->setActiveRoom(kRoom);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("holiday.png"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray("\x89PNG\r\n\x1a\n", 8));
        f.close();

        QSignalSpy spy(conn.get(), &ServerConnection::mediaSendFailed);
        conn->sendMediaMessage(QUrl::fromLocalFile(path).toString());

        // The upload is attempted and refused (nothing listens on port 1),
        // which is the same shape as the 401 the purge produced: a failure
        // arriving in the reply handler rather than before the request.
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toString(), kSignIn);
    }

    void localPreflightFailureKeepsItsOwnWordsEvenWhenExpired()
    {
        // "No active room" is not a 401 wearing a local error's clothes, and
        // the session sentence would be a confidently wrong answer to it.
        auto conn = deadSession();
        QSignalSpy spy(conn.get(), &ServerConnection::mediaSendFailed);

        conn->sendMediaMessage(QStringLiteral("file:///tmp/whatever.png"));

        // Deferred to the next turn of the event loop, and the spin is not
        // incidental: emitting a pre-flight failure inside the call locked
        // the composer out. tests/test_composer_upload_lock.cpp owns that
        // invariant; here it only has to be waited for.
        QVERIFY(spy.wait(2000));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toString(), QStringLiteral("No active room"));
    }

    void unreadableFileKeepsItsOwnWordsEvenWhenExpired()
    {
        auto conn = deadSession();
        conn->setActiveRoom(kRoom);
        QSignalSpy spy(conn.get(), &ServerConnection::mediaSendFailed);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString missing = dir.filePath(QStringLiteral("gone.png"));
        conn->sendMediaMessage(QUrl::fromLocalFile(missing).toString());

        QVERIFY(spy.wait(2000));  // deferred; see the case above
        QCOMPARE(spy.count(), 1);
        const auto msg = spy.first().at(0).toString();
        QVERIFY2(msg.contains(QStringLiteral("gone.png")),
                 "a file we cannot open is still a file we cannot open");
        QVERIFY(msg != kSignIn);
    }

    void healthyUploadFailureIsNotRewrittenAsASessionProblem()
    {
        auto conn = liveSession();
        conn->setCredentials(kUser, QStringLiteral("syt_live_token"),
                             QStringLiteral("DEV"), QStringLiteral("josh"));
        QVERIFY(!conn->needsReauth());
        conn->setActiveRoom(kRoom);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("clip.mp4"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(64, 'x'));
        f.close();

        QSignalSpy spy(conn.get(), &ServerConnection::mediaSendFailed);
        conn->sendMediaMessage(QUrl::fromLocalFile(path).toString());

        QVERIFY(spy.wait(5000));
        QVERIFY2(spy.first().at(0).toString() != kSignIn,
                 "a live session's upload failure must keep its own reason");
    }
};

// No QGuiApplication: a window system on a dev Mac means permission dialogs,
// which is the thing every test in this directory avoids.
QTEST_GUILESS_MAIN(TestErrorSurfaceGuards)
#include "test_error_surface_guards.moc"
