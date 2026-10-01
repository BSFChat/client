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
// The loop guard itself — settle() provokes the platform by republishing
// the cursor rectangle, which comes straight back as
// cursorRectangleChanged — needs a real input panel to exercise, and is
// asserted structurally in test_qml_hygiene.cpp instead. Said plainly
// rather than faked: a test that passes here because isVisible() is
// hardwired false would be measuring the absence of a keyboard.

#include <QtTest>
#include <QGuiApplication>
#include <QInputMethod>
#include <QSignalSpy>

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
