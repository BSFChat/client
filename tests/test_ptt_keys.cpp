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
// With the platform's physical-key identity, as real events carry.
QKeyEvent nativeEvent(QEvent::Type type, int key, Qt::KeyboardModifiers mods,
                      quint32 scanCode, bool repeat = false)
{
    return QKeyEvent(type, key, mods, scanCode, /*nativeVirtualKey=*/0,
                     /*nativeModifiers=*/0, QString(), repeat);
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

    void shiftedKeyReleasesWhenShiftGoesFirst()
    {
        // Captured as Shift+! (what a US keyboard delivers for Shift+1).
        // Letting go of Shift first turns the key's release into "1",
        // which never matched: PTT stuck open. The physical key decides.
        PttKeyFilter f;
        f.configure(true, PttKeyFilter::sequenceFor(Qt::Key_Exclam, Qt::ShiftModifier));
        auto p = nativeEvent(QEvent::KeyPress, Qt::Key_Exclam, Qt::ShiftModifier, 10);
        QVERIFY(f.handleKeyEvent(&p));
        QVERIFY(f.pressed());
        auto rep = nativeEvent(QEvent::KeyPress, Qt::Key_1, Qt::NoModifier, 10, true);
        QVERIFY(f.handleKeyEvent(&rep));     // its repeats, as "1" now
        QVERIFY(f.pressed());
        auto other = nativeEvent(QEvent::KeyRelease, Qt::Key_1, Qt::NoModifier, 11);
        QVERIFY(!f.handleKeyEvent(&other));  // a different physical key
        QVERIFY(f.pressed());
        auto r = nativeEvent(QEvent::KeyRelease, Qt::Key_1, Qt::NoModifier, 10);
        QVERIFY(f.handleKeyEvent(&r));
        QVERIFY(!f.pressed());
    }

    void captureProducesPortableText()
    {
        QCOMPARE(PttKeyFilter::sequenceFor(Qt::Key_Space, Qt::ControlModifier),
                 QStringLiteral("Ctrl+Space"));
        QCOMPARE(PttKeyFilter::sequenceFor(Qt::Key_F13, Qt::NoModifier), QStringLiteral("F13"));
        // Modifiers on their own: still waiting for the key.
        QCOMPARE(PttKeyFilter::sequenceFor(Qt::Key_Shift, Qt::ShiftModifier), QString());
        QCOMPARE(PttKeyFilter::sequenceFor(Qt::Key_Control, Qt::ControlModifier), QString());
        // The keypad flag is not part of the combination.
        QCOMPARE(PttKeyFilter::sequenceFor(Qt::Key_5, Qt::AltModifier | Qt::KeypadModifier),
                 QStringLiteral("Alt+5"));
    }

    void validationNamesTheProblem()
    {
        QVERIFY(PttKeyFilter::problem("Ctrl+Space").isEmpty());
        QVERIFY(PttKeyFilter::problem("F13").isEmpty());
        QVERIFY(PttKeyFilter::problem("`").isEmpty());
        QVERIFY(PttKeyFilter::problem("Alt+Space").isEmpty());
        QVERIFY(!PttKeyFilter::problem("").isEmpty());
        QVERIFY(!PttKeyFilter::problem("Ctrl+Spcae").isEmpty());   // the old silent typo
        QVERIFY(!PttKeyFilter::problem("Ctrl+A, Ctrl+B").isEmpty());
        QVERIFY(!PttKeyFilter::problem("Space").isEmpty());        // typing needs it
        QVERIFY(!PttKeyFilter::problem("Shift+A").isEmpty());
        QVERIFY(!PttKeyFilter::problem("Return").isEmpty());
        QVERIFY(PttKeyFilter::problem("Ctrl+A").isEmpty());
    }

    void macDefaultAvoidsSpotlight()
    {
#ifdef Q_OS_MACOS
        // Qt's Ctrl is Command on macOS: Ctrl+Space would be Spotlight.
        QCOMPARE(PttKeyFilter::defaultSequence(), QStringLiteral("Alt+Space"));
#else
        QCOMPARE(PttKeyFilter::defaultSequence(), QStringLiteral("Ctrl+Space"));
#endif
        QVERIFY(PttKeyFilter::problem(PttKeyFilter::defaultSequence()).isEmpty());
    }

    void suspendedWhileCapturing()
    {
        PttKeyFilter f;
        f.configure(true, QStringLiteral("F13"));
        f.setSuspended(true);
        auto p = press(Qt::Key_F13, Qt::NoModifier);
        QVERIFY(!f.handleKeyEvent(&p));       // reaches the capture field
        QVERIFY(!f.pressed());
        f.setSuspended(false);
        QVERIFY(f.handleKeyEvent(&p));
    }
};

QTEST_GUILESS_MAIN(TestPttKeys)
#include "test_ptt_keys.moc"
