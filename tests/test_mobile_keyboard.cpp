// MobileKeyboard's cursor watch.
//
// WHAT THIS IS FOR
//
// The shell's keyboard avoidance converges by asking the platform to
// recompute its own scroll and then re-reading how far it actually moved
// the scene (src/core/MobileKeyboard.h has the long version). That whole
// cadence had exactly one trigger: a change to the gap. Three signals fed
// it — QInputMethod::visibleChanged, QInputMethod::keyboardRectangleChanged
// and QScreen::availableGeometryChanged — and not one of them fires when
// the composer grows a reply banner underneath a keyboard that is already
// up and already settled. The keyboard's visibility, its rectangle and the
// screen are all exactly as they were.
//
// The cursor is not. QIOSInputContext::update() re-runs scrollToCursor()
// for Qt::ImCursorRectangle and for nothing else in the query set, so that
// is the one platform event that was happening with nobody watching — and
// a scroll nobody watches is the header translated off the top of the
// screen with the rest of the scene.
//
// WHAT A MAC CAN AND CANNOT PROVE
//
// There is no software keyboard here and no iOS plugin, so
// QInputMethod::isVisible() is false for the whole run and
// iosPlatformScrollOffset() is a stub that returns 0. What is real and
// worth guarding is the WIRING either side of that: that the bridge
// actually subscribes to the signal (checked by removing the connection
// and seeing that there was one to remove, not by grepping for the word),
// and that the slot stands down when the keyboard is down instead of
// starting a 400ms timer every time a caret moves in a desktop text field.
//
// Offscreen does give us a real QScreen and a real QWindow, though, and
// those are enough for the one quantity in this class that is pure
// arithmetic over rectangles: the no-keyboard baseline, and which of the
// two rectangles on offer it is taken from. See
// aScreenChangeReBaselinesFromTheWindowAndNotTheScreen().
//
// The loop guard itself — settle() provokes the platform by republishing
// the cursor rectangle, which comes straight back as
// cursorRectangleChanged — needs a real input panel to exercise, and is
// asserted structurally in test_qml_hygiene.cpp instead. Said plainly
// rather than faked: a test that passes here because isVisible() is
// hardwired false would be measuring the absence of a keyboard.

#include <QtTest>
#include <QGuiApplication>
#include <QInputMethod>
#include <QScreen>
#include <QSignalSpy>
#include <QWindow>

#include "core/MobileKeyboard.h"

class MobileKeyboardTest : public QObject {
    Q_OBJECT

private Q_SLOTS:

    void theBridgeWatchesTheCursorRectangle()
    {
        // Behavioural, not textual: QObject::disconnect() returns true only
        // if it actually removed a connection, so this fails on a bridge
        // that merely mentions the signal in a comment.
        MobileKeyboard kb;
        QInputMethod* im = QGuiApplication::inputMethod();
        QVERIFY2(im, "no QInputMethod on this platform");

        const bool wasConnected = QObject::disconnect(
            im, &QInputMethod::cursorRectangleChanged, &kb, nullptr);

        QVERIFY2(wasConnected,
                 "MobileKeyboard does not subscribe to "
                 "QInputMethod::cursorRectangleChanged. That is the only "
                 "platform event raised when the composer grows a reply "
                 "banner under an already-settled keyboard, and it is the "
                 "one that re-runs QIOSInputContext::scrollToCursor() — so "
                 "without it the platform re-scrolls the scene and nothing "
                 "asks it to stand down or re-reads how far it went.");
    }

    void aCursorMoveWithTheKeyboardDownStartsNothing()
    {
        // cursorRectangleChanged is a hot signal — every caret keystroke
        // in every text field raises it — and an unguarded slot would
        // re-arm the 50ms settle cadence on each one, asking the platform
        // to recompute a scroll for a keyboard that is not on screen.
        //
        // WHAT THIS DOES AND DOES NOT PIN DOWN. It fails against a slot
        // that emits unconditionally, which is the mutation worth having
        // a test for. It does NOT isolate the keyboard-visibility gate
        // from the moved-rectangle check: on a Mac there is no focus
        // object, so QInputMethod::cursorRectangle() is null and equal to
        // the rectangle the bridge starts with, and either guard alone
        // suppresses the emission. Deleting just the visibility gate is
        // caught structurally instead, by
        // theCursorWatchCannotFeedItself() in test_qml_hygiene.cpp —
        // verified by mutation, which is how this limitation was found
        // rather than assumed.
        MobileKeyboard kb;
        QSignalSpy moved(&kb, &MobileKeyboard::cursorMoved);
        QVERIFY(moved.isValid());

        QVERIFY2(QGuiApplication::inputMethod()
                     && !QGuiApplication::inputMethod()->isVisible(),
                 "this test's premise is a keyboard that is down");

        // Private slot, reached by name through the meta-object, because
        // the point is the slot's own behaviour rather than whatever the
        // offscreen platform happens to emit.
        QVERIFY2(QMetaObject::invokeMethod(&kb, "onCursorRectangleChanged"),
                 "MobileKeyboard has no onCursorRectangleChanged slot");

        QCOMPARE(moved.count(), 0);
    }

    void theBridgeSaysNothingBeforeAnythingHappens()
    {
        // Construction must not emit: MobileMain kicks the settle cadence
        // off cursorMoved, and a bridge that fired on creation would run
        // it once per launch with no keyboard anywhere.
        MobileKeyboard kb;
        QSignalSpy moved(&kb, &MobileKeyboard::cursorMoved);
        QCoreApplication::processEvents();
        QCOMPARE(moved.count(), 0);
    }

    void aScreenChangeReBaselinesFromTheWindowAndNotTheScreen()
    {
        // THE BUG THIS PINS DOWN
        //
        // m_baseGeometry is "the window with no keyboard": the rectangle
        // applyWindowGeometry() shrinks from and restores to, and the one
        // measureShrink() subtracts the live window height from. Two paths
        // set it, and they used to set it from two different rectangles —
        // onKeyboardChanged() from the WINDOW, onScreenGeometryChanged()
        // from QScreen::availableGeometry().
        //
        // Those are not the same space on the platform the file exists
        // for. Since Qt 6.9 the iOS plugin reports the whole screen as
        // available and insets the window instead, so on the captured
        // device (iPhone 16 Pro Max) the window is 440x860 at y=62 and
        // availableGeometry() is 440x956 at y=0. A baseline taken from the
        // screen moves the window origin 62pt up into the Dynamic Island
        // and measures shrink against a rectangle 96pt too tall.
        //
        // WHAT A MAC PROVES AND WHAT IT DOES NOT
        //
        // Offscreen has a real QScreen and a real QWindow, and
        // availableGeometry() here is the whole offscreen screen — much
        // taller than a small window — so the two candidate baselines are
        // far apart and this test can tell them apart. windowShrink is the
        // probe: it is exactly baseline − window height, so the right
        // baseline reads 0 and the screen's reads the whole difference.
        //
        // It does NOT prove the origin half. kShrinkWindowForKeyboard is
        // false off iOS, so applyWindowGeometry() never calls setGeometry()
        // here and the y=62 error has nothing to move. Only a device shows
        // that, and the window-origin field in the settle() log is what
        // reports it there.
        QWindow w;
        w.setGeometry(40, 30, 300, 200);
        w.show();
        w.requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(), &w);

        const QScreen* screen = w.screen();
        QVERIFY2(screen, "no QScreen for the test window");
        const int available = screen->availableGeometry().height();

        MobileKeyboard kb;
        // Gets the bridge to adopt the window; the keyboard is down, so
        // this takes the baseline from the window and measures no shrink.
        QVERIFY2(QMetaObject::invokeMethod(&kb, "onKeyboardChanged"),
                 "MobileKeyboard has no onKeyboardChanged slot");
        QCOMPARE(kb.windowShrink(), 0);

        // Stand in for what a rotation leaves behind: the window is a
        // different shape and the baseline we hold belongs to the old one.
        w.setGeometry(40, 30, 300, 150);
        QCoreApplication::processEvents();
        QCOMPARE(w.height(), 150);

        // The premise, asserted rather than assumed, so this cannot quietly
        // become a test of two numbers that happen to be equal: the stale
        // baseline (200), the screen's (the whole offscreen height) and the
        // window's own (150) all have to be distinguishable.
        QVERIFY2(available > 150 + 64,
                 qPrintable(QStringLiteral("offscreen available height %1 is "
                                           "too close to the window's 150 for "
                                           "this test to discriminate")
                                .arg(available)));

        QVERIFY2(QMetaObject::invokeMethod(&kb, "onScreenGeometryChanged"),
                 "MobileKeyboard has no onScreenGeometryChanged slot");

        // 0 is the window's own rectangle. A stale baseline would read 50
        // and QScreen::availableGeometry() would read the whole difference.
        QCOMPARE(kb.windowShrink(), 0);
    }

    void aScreenChangeWithNoTrackedWindowIsInert()
    {
        // availableGeometryChanged is only ever connected once a window is
        // adopted, but the slot is reachable by name and the guard is the
        // only thing between it and a null QPointer deref.
        MobileKeyboard kb;
        QVERIFY2(QMetaObject::invokeMethod(&kb, "onScreenGeometryChanged"),
                 "MobileKeyboard has no onScreenGeometryChanged slot");
        QCOMPARE(kb.windowShrink(), 0);
        QCOMPARE(kb.platformScroll(), 0);
    }

    void settleIsSafeWithNoKeyboardAndNoWindow()
    {
        // settle() is called from a repeating QML timer, so it runs on a
        // platform with no input panel and before any window is tracked.
        // It must be inert rather than fatal, and must leave the two
        // measurements at their off-iOS values.
        MobileKeyboard kb;
        QSignalSpy moved(&kb, &MobileKeyboard::cursorMoved);
        kb.settle();
        kb.settle(QVariantMap{{QStringLiteral("probe"), 1}});
        QCOMPARE(kb.platformScroll(), 0);
        QCOMPARE(kb.windowShrink(), 0);
        QCOMPARE(moved.count(), 0);
    }
};

QTEST_MAIN(MobileKeyboardTest)
#include "test_mobile_keyboard.moc"
