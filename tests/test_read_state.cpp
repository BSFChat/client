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
