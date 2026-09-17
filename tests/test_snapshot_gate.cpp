// The publish gate behind directRoomsChanged / serverMembersChanged /
// bannedMembersChanged (net/SnapshotGate.h).
//
// Two regressions are pinned here, pulling in opposite directions:
//   - the DM section going stale: a sync pass that moved a DM's order, typing
//     hint or presence MUST publish, or the sidebar never re-sorts;
//   - U-H2 churn: a sync pass that rebuilt an identical list must NOT publish,
//     or every delegate is destroyed and recreated on every sync.
// The rows below have the shape ServerConnection::directRooms() produces.

#include <QtTest/QtTest>
#include <QVariantList>
#include <QVariantMap>

#include "net/SnapshotGate.h"

using bsfchat::client::publishIfChanged;

namespace {

QVariantMap dmRow(const QString& roomId, const QString& peer, qint64 lastMs,
                  bool typing = false,
                  const QString& presence = QStringLiteral("offline"))
{
    QVariantMap m;
    m[QStringLiteral("roomId")] = roomId;
    m[QStringLiteral("peerId")] = peer;
    m[QStringLiteral("peerDisplayName")] = peer;
    m[QStringLiteral("peerPresence")] = presence;
    m[QStringLiteral("peerStatusMessage")] = QString();
    m[QStringLiteral("peerTyping")] = typing;
    m[QStringLiteral("lastMessageTime")] = lastMs;
    return m;
}

} // namespace

class TestSnapshotGate : public QObject {
    Q_OBJECT
private slots:
    void firstBuildPublishes()
    {
        QVariantList published;
        QVERIFY(publishIfChanged(published, QVariantList{dmRow("!a", "@alice", 10)}));
        QCOMPARE(published.size(), 1);
    }

    void emptyToEmptyIsSilent()
    {
        QVariantList published;
        QVERIFY(!publishIfChanged(published, QVariantList{}));
    }

    // The churn case: an independently rebuilt but equal list.
    void identicalRebuildIsSilent()
    {
        QVariantList published{dmRow("!a", "@alice", 20), dmRow("!b", "@bob", 10)};
        const QVariantList before = published;
        QVERIFY(!publishIfChanged(
            published, QVariantList{dmRow("!a", "@alice", 20), dmRow("!b", "@bob", 10)}));
        QCOMPARE(published, before);
    }

    // The stale-sidebar case: an inbound message bumps Bob above Alice.
    void reorderPublishes()
    {
        QVariantList published{dmRow("!a", "@alice", 20), dmRow("!b", "@bob", 10)};
        QVERIFY(publishIfChanged(
            published, QVariantList{dmRow("!b", "@bob", 30), dmRow("!a", "@alice", 20)}));
        QCOMPARE(published.first().toMap().value("roomId").toString(), QStringLiteral("!b"));
    }

    void nestedFieldChangePublishes_data()
    {
        QTest::addColumn<QVariantMap>("changed");
        QTest::newRow("typing") << dmRow("!a", "@alice", 10, true);
        QTest::newRow("presence") << dmRow("!a", "@alice", 10, false, QStringLiteral("online"));
        QTest::newRow("lastMessageTime") << dmRow("!a", "@alice", 11);
    }
    void nestedFieldChangePublishes()
    {
        QFETCH(QVariantMap, changed);
        QVariantList published{dmRow("!a", "@alice", 10)};
        QVERIFY(publishIfChanged(published, QVariantList{changed}));
        QCOMPARE(published.first().toMap(), changed);
        // …and the very next identical pass is quiet again.
        QVERIFY(!publishIfChanged(published, QVariantList{changed}));
    }

    // serverMembers rows carry a nested list; it must compare by value too.
    void nestedListComparesByValue()
    {
        auto row = [](const QStringList& rooms) {
            QVariantMap m;
            m[QStringLiteral("userId")] = QStringLiteral("@alice");
            m[QStringLiteral("rooms")] = rooms;
            return m;
        };
        QVariantList published{row({"!a", "!b"})};
        QVERIFY(!publishIfChanged(published, QVariantList{row({"!a", "!b"})}));
        QVERIFY(publishIfChanged(published, QVariantList{row({"!a"})}));
    }

    void removalPublishes()
    {
        QVariantList published{dmRow("!a", "@alice", 10)};
        QVERIFY(publishIfChanged(published, QVariantList{}));
        QVERIFY(published.isEmpty());
    }
};

QTEST_APPLESS_MAIN(TestSnapshotGate)
#include "test_snapshot_gate.moc"
