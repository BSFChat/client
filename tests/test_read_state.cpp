// The channel-list unread dot (src/core/ReadState.h).
//
// Two defects lived here. A room with no stored read marker could never show
// a dot — and the marker was only written by opening the room, so a channel
// the user had never visited stayed dark no matter what arrived in it. And
// "Mark as Read" stamped Date.now() into a value that is compared against
// origin_server_ts, so client clock skew hid dots or pinned them on.
//
// The scenarios below replay what ServerConnection::processSyncResponse does
// per room — seed once on first sight, never again — against a plain map
// standing in for Settings.

#include <QtTest/QtTest>
#include <QFile>
#include <QHash>
#include <QJsonObject>
#include <QRegularExpression>

#include "core/ReadState.h"

using namespace bsfchat::client;

namespace {
// Settings::seedLastReadTs / lastReadTs, minus the QSettings file.
struct Markers {
    QHash<QString, qint64> ts;
    qint64 get(const QString& room) const { return ts.value(room, 0); }
    void seed(const QString& room, qint64 v) {
        if (v > 0 && get(room) <= 0) ts[room] = v;
    }
};

QString readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

// Source scans below read intent, so a commented-out or merely-described
// version of the thing must not count as the thing. Same reasoning (and the
// same shape) as test_qml_hygiene's helper.
QString withoutComments(QString src)
{
    src.remove(QRegularExpression(QStringLiteral("/\\*.*?\\*/"),
                                  QRegularExpression::DotMatchesEverythingOption));
    src.remove(QRegularExpression(QStringLiteral("//[^\n]*")));
    return src;
}
} // namespace

class TestReadState : public QObject {
    Q_OBJECT
private slots:
    void unseededRoomIsNeverUnread() {
        // No marker = /sync hasn't introduced the room. No dot, but this is
        // no longer a state a synced room can stay in.
        QVERIFY(!isUnread(1'700'000'000'000, 0));
        QVERIFY(!isUnread(0, 0));
        QVERIFY(!isUnread(0, 1'700'000'000'000));
    }

    void firstLoginDoesNotLightEveryChannel() {
        // Initial full sync: every channel arrives with history.
        Markers m;
        const qint64 newest = 1'700'000'000'000;
        m.seed("!a", readSeedFor(/*baselineBatch=*/true, newest));
        QCOMPARE(m.get("!a"), newest);
        QVERIFY(!isUnread(newest, m.get("!a")));
    }

    void neverOpenedChannelLightsOnTheNextMessage() {
        // THE bug: seeded at first login, never opened, then a message.
        Markers m;
        const qint64 newest = 1'700'000'000'000;
        m.seed("!a", readSeedFor(true, newest));
        // Later batches re-run the seed; it must not move.
        m.seed("!a", readSeedFor(false, newest + 5000));
        QCOMPARE(m.get("!a"), newest);
        QVERIFY(isUnread(newest + 5000, m.get("!a")));
    }

    void emptyChannelAtFirstLoginLightsOnItsFirstMessage() {
        // No history to seed from — the seed still has to be a real marker,
        // or the room is back to "never comparable".
        Markers m;
        m.seed("!empty", readSeedFor(true, 0));
        QCOMPARE(m.get("!empty"), kReadSeedNothingRead);
        QVERIFY(!isUnread(0, m.get("!empty")));
        QVERIFY(isUnread(1'700'000'000'000, m.get("!empty")));
    }

    void roomFirstSeenLiveIsUnread() {
        // A channel created (or joined) mid-session whose introducing batch
        // already carries a message: that message is unread, not "history".
        Markers m;
        const qint64 ts = 1'700'000'000'000;
        m.seed("!new", readSeedFor(/*baselineBatch=*/false, ts));
        QVERIFY(isUnread(ts, m.get("!new")));
    }

    void seedNeverOverwritesARealMarker() {
        Markers m;
        m.ts["!a"] = 1'700'000'009'000;   // user read up to here
        m.seed("!a", readSeedFor(true, 1'700'000'000'000));
        m.seed("!a", readSeedFor(false, 0));
        QCOMPARE(m.get("!a"), qint64(1'700'000'009'000));
    }

    void markAsReadInServerClockSurvivesSkew() {
        // Server clock domain throughout: marking read at the newest
        // message's own timestamp clears the dot and the very next message
        // relights it, whatever the client's wall clock says.
        const qint64 newest = 1'700'000'000'000;
        QVERIFY(!isUnread(newest, /*lastRead=*/newest));
        QVERIFY(isUnread(newest + 1, newest));

        // What Date.now() did. Client 10 min fast: the next message is
        // swallowed. Client 10 min slow: the dot cannot be cleared.
        const qint64 skew = 10 * 60 * 1000;
        QVERIFY(!isUnread(newest + 1, newest + skew));
        QVERIFY(isUnread(newest, newest - skew));
    }

    void aMarkerOnlyEverGoesForward()
    {
        // The server resolves its own upsert with MAX (SqliteStore::
        // set_read_marker). The client did not, and several of its writers
        // pass "the newest thing I can see" rather than "the newest thing the
        // user read" — a sync batch's newest message, the newest LOADED row
        // at room-switch time, the newest row in a model still filling. Any
        // of those landing after a further-on marker re-lights a dot the user
        // has already cleared.
        QVERIFY(readMarkerAdvances(1'700'000'000'000, 1'700'000'000'001));
        QVERIFY(!readMarkerAdvances(1'700'000'000'000, 1'700'000'000'000));
        QVERIFY(!readMarkerAdvances(1'700'000'000'000, 1'699'000'000'000));
        // An absent marker is 0, so a first real write always takes.
        QVERIFY(readMarkerAdvances(0, kReadSeedNothingRead));
    }

    void markerSurvivesAnOutOfOrderSyncBatch()
    {
        // The concrete shape: the user reads to the end of a busy channel,
        // and the next sync batch the client processes carries an older
        // message (a backfill, a redelivery, a room whose events arrive
        // behind the one we just marked). The handler passes that batch's
        // newest message down as the marker.
        Markers m;
        const qint64 readTo = 1'700'000'009'000;
        m.ts["!a"] = readTo;
        const qint64 batchNewest = 1'700'000'005'000;
        if (readMarkerAdvances(m.get("!a"), batchNewest)) m.ts["!a"] = batchNewest;
        QCOMPARE(m.get("!a"), readTo);
        QVERIFY(!isUnread(batchNewest, m.get("!a")));
    }

    void yourOwnMessageMustNotStrandYourOwnDot()
    {
        // The reported bug, as arithmetic, once the client grew a local echo.
        //
        // The echo is stamped when we hit send. The server stamps
        // origin_server_ts when it RECEIVES it — later, always, by at least
        // the round trip, and further on a phone's radio. The room's
        // "newest message" that the dot compares against is the server's
        // number; the marker the view wrote is ours.
        const qint64 sentAt    = 1'700'000'000'000;   // our clock, on send
        const qint64 stampedAt = 1'700'000'000'450;   // origin_server_ts

        // What the unfixed client wrote: the echo's own timestamp.
        QVERIFY2(isUnread(stampedAt, /*lastRead=*/sentAt),
                 "this is the defect — the room is 'unread' because of a "
                 "message the user typed themselves");
        // And it never recovers: every further at-bottom persist reads the
        // same echo row and writes the same number, which by now does not
        // even advance the marker.
        QVERIFY(!readMarkerAdvances(sentAt, sentAt));

        // What the fixed client writes: the timestamp the server put on it.
        QVERIFY(!isUnread(stampedAt, /*lastRead=*/stampedAt));
    }

    // ── THE TRIGGER A PHONE CAN ACTUALLY PRODUCE ────────────────────────
    //
    // This is the test that would have caught the reported bug, and it is
    // deliberately about the SHAPE of the trigger rather than any one
    // expression, because the fault was not a wrong condition — it was that
    // every condition in the client described a desktop.
    //
    // A read marker left this client in exactly three ways: a row arriving
    // while the message list happened to be parked at its end, a right-click
    // context-menu item, and /sync noticing a new message in the open room.
    // All three need something to HAPPEN. A phone user opens the app, reads
    // what is already on screen, and swipes away: no new row, no scroll, no
    // click, and then the process is suspended and killed without
    // aboutToQuit. There was no path at all from "the user looked at this and
    // left" to "tell the server".
    //
    // So: the application-state hook — the one signal a mobile app raises on
    // its own, and the one the foreground re-poll already rides on — must
    // carry a read-marker write, and must persist it.
    void leavingTheForegroundMarksAndPersists()
    {
        const QString main = withoutComments(
            readAll(QStringLiteral(BSFCHAT_SRC_DIR "/main.cpp")));
        QVERIFY2(!main.isEmpty(), "main.cpp not readable — has it moved?");

        // The handler, from the connect to the end of its lambda.
        const QRegularExpression handler(
            QStringLiteral(R"(applicationStateChanged.*?\n\s*\}\);)"),
            QRegularExpression::DotMatchesEverythingOption);
        const auto m = handler.match(main);
        QVERIFY2(m.hasMatch(),
                 "nothing is wired to QGuiApplication::applicationStateChanged; "
                 "a mobile build then has no lifecycle signal at all");
        const QString body = m.captured(0);

        QVERIFY2(body.contains(QStringLiteral("markActiveRoomsRead")),
                 "the application-state handler does not mark the open room "
                 "read. Every other read-marker trigger in this client needs a "
                 "row, a scroll or a click — none of which a phone produces "
                 "when its owner reads what is on screen and swipes away.");
        QVERIFY2(body.contains(QStringLiteral("flush")),
                 "the application-state handler does not flush Settings. "
                 "QSettings buffers until sync() or its destructor, and a "
                 "phone's process is killed without running either — so the "
                 "marker it just wrote never reaches disk.");
        // ApplicationActive is the RESUME, not the departure: marking there
        // would claim a backlog the user has not looked at yet.
        QVERIFY2(body.contains(QStringLiteral("ApplicationActive")),
                 "the handler no longer distinguishes resume from departure");
    }

    void nothingInTheReadPathAsksWhetherAWindowIsFocused()
    {
        // The desktop notion of "the user is reading this" is a focused
        // window. A phone has no such thing — Qt reports it through
        // applicationState instead — so a read marker gated on it can never
        // be sent from one. The client did not in fact have such a gate, and
        // this is what keeps it that way: the obvious "fix" for an
        // over-eager marker is to add one.
        const QString view = withoutComments(readAll(
            QStringLiteral(BSFCHAT_QML_DIR "/components/MessageView.qml")));
        QVERIFY2(!view.isEmpty(), "MessageView.qml not readable — has it moved?");
        QVERIFY2(!view.contains(QStringLiteral("Window.active")),
                 "MessageView gates on Window.active, which is false for the "
                 "whole life of a mobile app");

        const QString conn = withoutComments(
            readAll(QStringLiteral(BSFCHAT_SRC_DIR "/net/ServerConnection.cpp")));
        QVERIFY2(!conn.isEmpty(), "ServerConnection.cpp not readable?");
        const QRegularExpression focusGate(QStringLiteral(
            R"((focusWindow|isActiveWindow|isExposed)\s*\()"));
        QVERIFY2(!focusGate.match(conn).hasMatch(),
                 "ServerConnection asks a window whether it is focused; on a "
                 "phone there is no window to ask");
    }

    void theForegroundMarkStillRespectsWhereTheUserWasReading()
    {
        // U-M3 in the new path: leaving the app is not "I read everything".
        // Somebody halfway up their history when the phone locks has read
        // what is above them and nothing below, so the write is gated on the
        // view's last at-bottom sample, exactly as the room-switch persist is.
        const QString conn = withoutComments(
            readAll(QStringLiteral(BSFCHAT_SRC_DIR "/net/ServerConnection.cpp")));
        const QRegularExpression fn(
            QStringLiteral(R"(void ServerConnection::markActiveRoomRead\(\).*?\n\})"),
            QRegularExpression::DotMatchesEverythingOption);
        const auto m = fn.match(conn);
        QVERIFY2(m.hasMatch(), "ServerConnection::markActiveRoomRead is gone");
        QVERIFY2(m.captured(0).contains(QStringLiteral("m_timelineAtBottom")),
                 "markActiveRoomRead no longer honours the at-bottom sample — "
                 "backgrounding the app now marks history the user never "
                 "scrolled down to (U-M3)");
        QVERIFY2(m.captured(0).contains(QStringLiteral("newestServerTimestampMs")),
                 "markActiveRoomRead reads a timestamp that may be a local "
                 "echo's client clock");
    }

    // ── The marker the server sends down ─────────────────────────────

    void theServerMarkerIsReadFromItsTimestamp()
    {
        // The spec field this client cannot use, and the one it can.
        QJsonObject doc;
        doc[QStringLiteral("event_id")] = QStringLiteral("$abc:server");
        doc[QStringLiteral("bsfchat.origin_server_ts")] = 1'700'000'000'000.0;
        QCOMPARE(serverReadMarkerTs(doc), 1'700'000'000'000LL);

        // A server that does not send the timestamp, a malformed one, and a
        // zero all mean "nothing to apply" — never "read up to time zero",
        // which would be a marker that moves a real one backwards.
        QJsonObject specOnly;
        specOnly[QStringLiteral("event_id")] = QStringLiteral("$abc:server");
        QCOMPARE(serverReadMarkerTs(specOnly), 0LL);
        QJsonObject wrongType;
        wrongType[QStringLiteral("bsfchat.origin_server_ts")] = QStringLiteral("soon");
        QCOMPARE(serverReadMarkerTs(wrongType), 0LL);
        QJsonObject zero;
        zero[QStringLiteral("bsfchat.origin_server_ts")] = 0.0;
        QCOMPARE(serverReadMarkerTs(zero), 0LL);
        QCOMPARE(serverReadMarkerTs(QJsonObject{}), 0LL);
    }

    // The phone reads to the end; the desktop, which has been asleep, learns
    // about it from the next sync and its dot goes out. This is the whole
    // feature, as arithmetic.
    void aMarkerFromAnotherDeviceClearsTheDot()
    {
        Markers m;
        const qint64 newest = 1'700'000'009'000;
        m.seed(QStringLiteral("!r"), 1'700'000'000'000);
        QVERIFY(isUnread(newest, m.get(QStringLiteral("!r"))));

        QJsonObject fromSync;
        fromSync[QStringLiteral("bsfchat.origin_server_ts")] =
            static_cast<double>(newest);
        const qint64 ts = serverReadMarkerTs(fromSync);
        if (readMarkerAdvances(m.get(QStringLiteral("!r")), ts)) m.ts[QStringLiteral("!r")] = ts;

        QVERIFY2(!isUnread(newest, m.get(QStringLiteral("!r"))),
                 "a room read on another device still shows a dot here");
    }

    // And the other direction, which is the one that would be a REGRESSION
    // rather than a missing feature: this device is ahead, the server has not
    // heard from it yet, and the marker it sends is older. Forward-only is
    // what stops it re-lighting a dot the user cleared here a second ago.
    void anOfflineDeviceCannotRewindThisOne()
    {
        Markers m;
        m.ts[QStringLiteral("!r")] = 1'700'000'009'000;  // we are caught up

        QJsonObject stale;
        stale[QStringLiteral("bsfchat.origin_server_ts")] = 1'700'000'001'000.0;
        const qint64 ts = serverReadMarkerTs(stale);
        QVERIFY(ts > 0);
        QVERIFY2(!readMarkerAdvances(m.get(QStringLiteral("!r")), ts),
                 "a stale marker from the server was allowed to move this "
                 "device's cursor backwards");
        if (readMarkerAdvances(m.get(QStringLiteral("!r")), ts)) m.ts[QStringLiteral("!r")] = ts;
        QCOMPARE(m.get(QStringLiteral("!r")), 1'700'000'009'000LL);
        QVERIFY(!isUnread(1'700'000'009'000, m.get(QStringLiteral("!r"))));
    }

    // The ordering that is easy to lose in a refactor: the marker has to be
    // applied BEFORE the timeline loop that gates notifications on it, or a
    // catch-up batch of messages already read on the phone toasts on this
    // device first and clears second.
    void theServerMarkerIsAppliedBeforeTheNotificationGate()
    {
        const QString conn = withoutComments(
            readAll(QStringLiteral(BSFCHAT_SRC_DIR "/net/ServerConnection.cpp")));
        QVERIFY2(!conn.isEmpty(), "ServerConnection.cpp not readable?");

        const int applied = conn.indexOf(QStringLiteral("serverReadMarkerTs"));
        QVERIFY2(applied >= 0,
                 "nothing in ServerConnection reads the server's m.fully_read "
                 "marker, so read state does not cross devices at all");

        // The gate: the notification suppression that compares an event's
        // timestamp against the stored marker.
        const int gate = conn.indexOf(QStringLiteral("m_firstSyncProcessed && eventTs > readTs"));
        QVERIFY2(gate >= 0, "the notification read-gate has moved — re-pin this");
        QVERIFY2(applied < gate,
                 "the server's read marker is applied AFTER the notification "
                 "gate, so a batch the user already read on another device "
                 "toasts here before the marker silences it");
    }

    void channelListStaysInTheServerClock() {
        // Guard the QML half: the dot goes through Settings::isRoomUnread,
        // and no read marker is ever written from the client clock.
        const QString src = readAll(
            QStringLiteral(BSFCHAT_QML_DIR "/components/ChannelList.qml"));
        QVERIFY2(!src.isEmpty(), "ChannelList.qml not readable — has it moved?");
        QVERIFY(src.contains(QStringLiteral("appSettings.isRoomUnread(")));
        const QRegularExpression clockWrite(QStringLiteral(
            R"((setLastReadTs|markRoomRead)\s*\([^)]*Date\.now\s*\()"));
        QVERIFY2(!clockWrite.match(src).hasMatch(),
                 "read marker written from Date.now() — it is compared against "
                 "origin_server_ts and must be in the server's clock");
    }
};

QTEST_APPLESS_MAIN(TestReadState)
#include "test_read_state.moc"
