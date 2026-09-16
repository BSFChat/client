// ServerManager's connection-list lifecycle (U-C1).
//
// The bug this exists to pin: removing the *active* server when it was
// not the last one in the list used to deleteLater() the connection and
// then call setActiveServer(sameIndex), which early-returned because the
// index hadn't changed. `activeServer` kept pointing at a deleted object
// and activeServerChanged never fired, so QML and C++ both kept using
// freed memory until something crashed.
//
// The bookkeeping lives in ServerRoster (header-only, no Qt beyond
// QList) precisely so it can be driven here without a Settings object, a
// ServerConnection, an event loop, a QSettings file or a network.

#include <QtTest/QtTest>
#include <QStringList>

#include "net/ServerRoster.h"

namespace {

// Stand-in for ServerConnection: the roster only ever stores and
// compares the pointer, so a name is all a test needs to read the log.
struct FakeConn {
    QString name;
    bool retired = false;
};

} // namespace

class TestServerRoster : public QObject {
    Q_OBJECT

private:
    ServerRoster<FakeConn> roster;
    QStringList log;
    QList<FakeConn*> owned;

    FakeConn* makeConn(const QString& name)
    {
        auto* c = new FakeConn{name, false};
        owned.append(c);
        return c;
    }

    void installHooks()
    {
        roster.hooks.rowRemoved = [this](int index) {
            log << QStringLiteral("rowRemoved(%1)").arg(index);
        };
        roster.hooks.persistIndex = [this](int index) {
            log << QStringLiteral("persistIndex(%1)").arg(index);
        };
        roster.hooks.activeChanged = [this]() {
            // Record what QML would see at the instant the signal fires —
            // this is where the old code handed out a dangling pointer.
            log << QStringLiteral("activeChanged[%1@%2]")
                       .arg(roster.active() ? roster.active()->name
                                            : QStringLiteral("null"))
                       .arg(roster.activeIndex());
        };
        roster.hooks.serverRemoved = [this](int index) {
            log << QStringLiteral("serverRemoved(%1)").arg(index);
        };
        roster.hooks.retire = [this](FakeConn* conn) {
            conn->retired = true;
            log << QStringLiteral("retire(%1)").arg(conn->name);
        };
    }

private slots:
    void init()
    {
        roster = ServerRoster<FakeConn>();
        log.clear();
        installHooks();
    }

    void cleanup()
    {
        qDeleteAll(owned);
        owned.clear();
    }

    // ── add / switch ──────────────────────────────────────────────

    void addAndActivate()
    {
        QCOMPARE(roster.count(), 0);
        QVERIFY(roster.active() == nullptr);
        QCOMPARE(roster.activeIndex(), -1);

        auto* a = makeConn("A");
        roster.append(a);
        QCOMPARE(roster.count(), 1);
        // Appending alone must not activate anything — ServerManager
        // decides that, and only for the first server.
        QVERIFY(roster.active() == nullptr);
        QVERIFY(log.isEmpty());

        roster.setActive(0);
        QCOMPARE(roster.active(), a);
        QCOMPARE(roster.activeIndex(), 0);
        QCOMPARE(log, QStringList({"persistIndex(0)", "activeChanged[A@0]"}));
    }

    void switchBetweenServers()
    {
        auto* a = makeConn("A");
        auto* b = makeConn("B");
        roster.append(a);
        roster.append(b);
        roster.setActive(0);
        log.clear();

        roster.setActive(1);
        QCOMPARE(roster.active(), b);
        QCOMPARE(roster.activeIndex(), 1);
        QCOMPARE(log, QStringList({"persistIndex(1)", "activeChanged[B@1]"}));

        // Re-selecting the same server is a no-op, so bindings don't churn.
        log.clear();
        roster.setActive(1);
        QVERIFY(log.isEmpty());

        // Out-of-range is ignored rather than clearing the selection.
        roster.setActive(7);
        roster.setActive(-2);
        QCOMPARE(roster.active(), b);
        QVERIFY(log.isEmpty());
    }

    // ── remove ────────────────────────────────────────────────────

    // The U-C1 case: three servers, the middle one active, remove it.
    // The old code early-returned out of setActiveServer here.
    void removeActiveNotLast()
    {
        auto* a = makeConn("A");
        auto* b = makeConn("B");
        auto* c = makeConn("C");
        roster.append(a);
        roster.append(b);
        roster.append(c);
        roster.setActive(1);
        log.clear();

        roster.remove(1);

        // C slid into slot 1 and is now active — not a freed B.
        QCOMPARE(roster.count(), 2);
        QCOMPARE(roster.active(), c);
        QCOMPARE(roster.activeIndex(), 1);
        QVERIFY(b->retired);
        QVERIFY(!c->retired);

        QCOMPARE(log, QStringList({
            "rowRemoved(1)",
            // No persistIndex: the stored index is still 1 and still
            // names the right row, it just names a different server now.
            // The POINTER moving is what has to be announced.
            "activeChanged[C@1]",
            "serverRemoved(1)",
            // Rule 3: never before activeChanged.
            "retire(B)",
        }));
    }

    // Removing the active server when it *is* the last row: selection
    // falls back to the new last row.
    void removeActiveLastRow()
    {
        auto* a = makeConn("A");
        auto* b = makeConn("B");
        roster.append(a);
        roster.append(b);
        roster.setActive(1);
        log.clear();

        roster.remove(1);

        QCOMPARE(roster.count(), 1);
        QCOMPARE(roster.active(), a);
        QCOMPARE(roster.activeIndex(), 0);
        QCOMPARE(log, QStringList({
            "rowRemoved(1)",
            "persistIndex(0)",
            "activeChanged[A@0]",
            "serverRemoved(1)",
            "retire(B)",
        }));
    }

    // Removing the only server leaves nothing active — and the null must
    // be visible by the time activeServerChanged fires, because every
    // QML binding on activeServer.* re-evaluates inside that signal.
    void removeOnlyServer()
    {
        auto* a = makeConn("A");
        roster.append(a);
        roster.setActive(0);
        log.clear();

        roster.remove(0);

        QCOMPARE(roster.count(), 0);
        QVERIFY(roster.active() == nullptr);
        QCOMPARE(roster.activeIndex(), -1);
        QCOMPARE(log, QStringList({
            "rowRemoved(0)",
            "persistIndex(-1)",
            "activeChanged[null@-1]",
            "serverRemoved(0)",
            "retire(A)",
        }));
    }

    // Removing a server *before* the active one keeps the same pointer
    // but shifts its index — activeServerIndex is a Q_PROPERTY notified
    // by activeServerChanged, so that still has to be emitted.
    void removeBeforeActiveShiftsIndex()
    {
        auto* a = makeConn("A");
        auto* b = makeConn("B");
        auto* c = makeConn("C");
        roster.append(a);
        roster.append(b);
        roster.append(c);
        roster.setActive(2);
        log.clear();

        roster.remove(0);

        QCOMPARE(roster.active(), c);
        QCOMPARE(roster.activeIndex(), 1);
        QCOMPARE(log, QStringList({
            "rowRemoved(0)",
            "persistIndex(1)",
            "activeChanged[C@1]",
            "serverRemoved(0)",
            "retire(A)",
        }));
    }

    // Removing a server *after* the active one touches neither the
    // pointer nor the index, so no activeServerChanged and no rewrite of
    // the persisted index.
    void removeAfterActiveIsQuiet()
    {
        auto* a = makeConn("A");
        auto* b = makeConn("B");
        roster.append(a);
        roster.append(b);
        roster.setActive(0);
        log.clear();

        roster.remove(1);

        QCOMPARE(roster.active(), a);
        QCOMPARE(roster.activeIndex(), 0);
        QCOMPARE(log, QStringList({
            "rowRemoved(1)",
            "serverRemoved(1)",
            "retire(B)",
        }));
    }

    void removeOutOfRangeDoesNothing()
    {
        auto* a = makeConn("A");
        roster.append(a);
        roster.setActive(0);
        log.clear();

        roster.remove(-1);
        roster.remove(5);

        QCOMPARE(roster.count(), 1);
        QCOMPARE(roster.active(), a);
        QVERIFY(!a->retired);
        QVERIFY(log.isEmpty());
    }

    // Removing every server one at a time must never leave a stale
    // pointer behind at any step — the "log out of my last account"
    // path (D-H6) walks straight through this.
    void removeAllSequentially()
    {
        auto* a = makeConn("A");
        auto* b = makeConn("B");
        auto* c = makeConn("C");
        roster.append(a);
        roster.append(b);
        roster.append(c);
        roster.setActive(0);

        while (roster.count() > 0) {
            roster.remove(roster.activeIndex());
            // Either nothing is left, or the active pointer is a row that
            // is genuinely still in the list.
            if (roster.count() == 0) {
                QVERIFY(roster.active() == nullptr);
                QCOMPARE(roster.activeIndex(), -1);
            } else {
                QVERIFY(roster.active() != nullptr);
                QCOMPARE(roster.indexOf(roster.active()), roster.activeIndex());
                QVERIFY(!roster.active()->retired);
            }
        }
        QVERIFY(a->retired && b->retired && c->retired);
    }

    // ── replace (reconnect / change URL) ──────────────────────────

    void replaceActiveSwapsPointerAndNotifies()
    {
        auto* a = makeConn("A");
        auto* b = makeConn("B");
        roster.append(a);
        roster.append(b);
        roster.setActive(0);
        log.clear();

        auto* a2 = makeConn("A2");
        roster.replace(0, a2);

        QCOMPARE(roster.active(), a2);
        QCOMPARE(roster.activeIndex(), 0);
        QCOMPARE(roster.at(0), a2);
        QCOMPARE(log, QStringList({"activeChanged[A2@0]"}));
    }

    void replaceInactiveIsQuiet()
    {
        auto* a = makeConn("A");
        auto* b = makeConn("B");
        roster.append(a);
        roster.append(b);
        roster.setActive(0);
        log.clear();

        auto* b2 = makeConn("B2");
        roster.replace(1, b2);

        QCOMPARE(roster.active(), a);
        QCOMPARE(roster.at(1), b2);
        QVERIFY(log.isEmpty());
    }

    // ── lookup helpers ────────────────────────────────────────────

    void atClampsInsteadOfCrashing()
    {
        auto* a = makeConn("A");
        roster.append(a);
        QCOMPARE(roster.at(0), a);
        QVERIFY(roster.at(-1) == nullptr);
        QVERIFY(roster.at(1) == nullptr);
    }
};

QTEST_APPLESS_MAIN(TestServerRoster)
#include "test_server_roster.moc"
