// Push-to-talk key edges (audit 2 V2-2). The old QML Shortcut + 120 ms
// release timer released inside the OS key-repeat delay and clipped every
// sentence; PttKeyFilter tracks the real press and release instead.

#include "net/PttKeyFilter.h"

#include <QKeyEvent>
#include <QSignalSpy>
#include <QTest>

namespace {
QKeyEvent press(int key, Qt::KeyboardModifiers mods, bool repeat = false)
{
    return QKeyEvent(QEvent::KeyPress, key, mods, QString(), repeat);
}
QKeyEvent release(int key, Qt::KeyboardModifiers mods, bool repeat = false)
{
    return QKeyEvent(QEvent::KeyRelease, key, mods, QString(), repeat);
}
} // namespace

class TestPttKeys : public QObject {
    Q_OBJECT
private slots:
    void heldKeyStaysOpenThroughRepeats()
    {
        PttKeyFilter f;
        f.configure(true, QStringLiteral("Ctrl+Space"));
        QSignalSpy edges(&f, &PttKeyFilter::pressedChanged);

        auto p = press(Qt::Key_Space, Qt::ControlModifier);
        QVERIFY(f.handleKeyEvent(&p));
        QVERIFY(f.pressed());
        // X11/Windows auto-repeat: release+press pairs flagged as repeats.
        for (int i = 0; i < 10; ++i) {
            auto rr = release(Qt::Key_Space, Qt::ControlModifier, true);
            auto rp = press(Qt::Key_Space, Qt::ControlModifier, true);
            QVERIFY(f.handleKeyEvent(&rr));   // swallowed
            QVERIFY(f.handleKeyEvent(&rp));
            QVERIFY(f.pressed());
        }
        // Ctrl let go first; Space still held — still talking.
        auto p2 = press(Qt::Key_Space, Qt::NoModifier, true);
        QVERIFY(f.handleKeyEvent(&p2));
        QVERIFY(f.pressed());
        auto r = release(Qt::Key_Space, Qt::NoModifier);
        QVERIFY(f.handleKeyEvent(&r));
        QVERIFY(!f.pressed());
        QCOMPARE(edges.size(), 2);           // one open, one close
    }

    void modifiersMustMatchToOpen()
    {
        PttKeyFilter f;
        f.configure(true, QStringLiteral("Ctrl+Space"));
        auto plain = press(Qt::Key_Space, Qt::NoModifier);
        QVERIFY(!f.handleKeyEvent(&plain));  // typing a space is not PTT
        auto extra = press(Qt::Key_Space, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(!f.handleKeyEvent(&extra));
        auto other = press(Qt::Key_A, Qt::ControlModifier);
        QVERIFY(!f.handleKeyEvent(&other));
        QVERIFY(!f.pressed());
    }

    void disablingReleases()
    {
        PttKeyFilter f;
        f.configure(true, QStringLiteral("F13"));
        QSignalSpy edges(&f, &PttKeyFilter::pressedChanged);
        auto p = press(Qt::Key_F13, Qt::NoModifier);
        QVERIFY(f.handleKeyEvent(&p));
        f.configure(false, QStringLiteral("F13"));
        QVERIFY(!f.pressed());
        QCOMPARE(edges.size(), 2);
        // Disabled: presses pass through untouched.
        QVERIFY(!f.handleKeyEvent(&p));
        // A stray release while disabled and closed is not ours either.
        auto r = release(Qt::Key_F13, Qt::NoModifier);
        QVERIFY(!f.handleKeyEvent(&r));
    }
};

QTEST_GUILESS_MAIN(TestPttKeys)
#include "test_ptt_keys.moc"
