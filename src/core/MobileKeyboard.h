// Software-keyboard bridge for the mobile shell (qml/mobile/MobileMain.qml).
//
// THE PROBLEM
//
// On iOS the window is never resized for the keyboard. QIOSInputContext::
// scrollToCursor() instead translates the ENTIRE Qt scene up, by setting a
// translation on the root UIView's layer.sublayerTransform, until the text
// cursor is clear of the keyboard. For an app with no chrome of its own
// that is a reasonable default. For this one it means that while you type
// there is no channel header, no member-list button and no overflow menu,
// and the top message row renders under the status clock — which is what
// an App Store reviewer meets in the first ten seconds of a chat app.
//
// The shell cannot simply push its own layout instead, because the two
// compose: the platform computes its scroll at UIKeyboardWillShow from the
// cursor as it was BEFORE the push, and the composer ends up a whole extra
// keyboard height in the air over a void. That shipped, and the device
// showed it.
//
// WHAT WAS TRIED, AND WHY IT DID NOT WORK
//
// Asking the platform to stand down. scrollToCursor() does call scroll(0)
// once the cursor is contained in (root view bounds − keyboard rect), and
// QIOSInputContext::update() re-runs it for Qt::ImCursorRectangle, which is
// public API. On the device it never stood down, and the plugin's own code
// says why: the containment test compares QPlatformInputContext::
// cursorRectangle(), which is in Qt WINDOW coordinates, against a region
// built from UIKit SCREEN rects ([focusView window], [UIScreen mainScreen]
// bounds via CGRectGetMaxY) — and on a device whose window is inset 62pt
// below a Dynamic Island those are not the same space. The keyboard
// rectangle it subtracts is itself converted through the translated layer,
// so the scroll is baked into it: the device reported y=894 where the true
// top is 611 on screen and 549 in window coordinates, and 611 + 345 − 62 is
// exactly 894. It is not a test the shell can satisfy from the outside.
//
// WHAT THIS DOES INSTEAD
//
// Removes the conflict rather than arbitrating it: shrink the WINDOW while
// the keyboard is up, the way Android's windowSoftInputMode=adjustResize
// already does. A keyboard that does not intersect the root view cannot
// cover the cursor, so the plugin's test passes trivially, it scrolls
// nothing, the header stays where it is and the layout simply has less room
// — which is the shape every other part of this app already expects.
//
// QIOSWindow::setGeometry() stores the rect but only applies it when
// window()->windowState() is Qt::WindowNoState; under Qt::WindowMaximized,
// which is what the shell asks for, it is silently dropped. So the geometry
// is set FIRST (which records it) and the state flipped to NoState SECOND
// (which applies the recorded rect) — one transition, no frame at a stale
// size. Both calls are public QWindow API.
//
// It is not assumed to work. The applied geometry is read back, logged
// either way, and after a few consecutive rejections the whole mechanism
// latches off and the platform goes back to owning keyboard avoidance. The
// shell's push is a subtraction over measured quantities, so that fallback
// needs no edit anywhere:
//
//     push = keyboardHeight − platformScroll − windowShrink
//
// Every term is observed rather than assumed, which is also why the
// expression needs no per-platform branch: on Android adjustResize supplies
// windowShrink, on iOS this class does, and if neither happens the shell
// pushes the layout itself.
#pragma once

#include <QObject>
#include <QPointer>
#include <QRect>
#include <QRectF>
#include <QVariantMap>

class QWindow;

class MobileKeyboard : public QObject {
    Q_OBJECT

    // How far the platform has translated the whole scene up, in Qt
    // logical pixels. Always 0 off iOS.
    Q_PROPERTY(int platformScroll READ platformScroll NOTIFY platformScrollChanged)

    // How much shorter the window is than it was with no keyboard,
    // whoever made it so — this class on iOS, adjustResize on Android.
    Q_PROPERTY(int windowShrink READ windowShrink NOTIFY windowShrinkChanged)

public:
    explicit MobileKeyboard(QObject* parent = nullptr);

    int platformScroll() const { return m_platformScroll; }
    int windowShrink() const { return m_windowShrink; }

    // Re-sample both quantities, ask the platform input context to
    // recompute its own scroll, and log one line. Called from the shell a
    // few times across the keyboard's animation, because every number
    // here is a measurement of something still moving.
    //
    // The log samples the platform scroll FRESH on both sides of the
    // recompute request: an earlier version compared against the previous
    // sample, which cannot tell "we caused this" from "we had not looked
    // since before the keyboard opened", and that ambiguity cost a device
    // round trip.
    Q_INVOKABLE void settle(const QVariantMap& state = QVariantMap());

Q_SIGNALS:
    void platformScrollChanged();
    void windowShrinkChanged();

    // The text cursor moved for a reason that was not the keyboard.
    //
    // The shell answers this by kicking the same settle cadence a gap
    // change kicks, and it has to, because the three signals above
    // cannot see the event: opening a reply banner grows the composer
    // (qml/components/MessageInput.qml) while the keyboard's visibility,
    // its rectangle and the screen's geometry all stay exactly as they
    // were. QIOSInputContext::update() re-runs scrollToCursor() for
    // Qt::ImCursorRectangle and nothing else, so the platform DOES
    // reconsider — Qt republishes the rectangle as part of the very
    // layout pass that moved it. That single ask is the problem, not the
    // cure: the plugin answers from a layout still in motion, keeps the
    // answer, and until this signal existed nothing asked it again once
    // the layout had settled or re-read how far it had translated the
    // scene. An answer taken mid-flight and never revisited is the
    // header sitting off the top of the screen.
    void cursorMoved();

private Q_SLOTS:
    // The platform emits keyboardRectangleChanged BEFORE it scrolls — the
    // scroll is the tail call of its own keyboardWillShow handler — so
    // this runs while there is still time to shrink the window out of the
    // keyboard's way and have the plugin find nothing to do.
    void onKeyboardChanged();

    // Rotation, split view, an inset change. Re-takes the baseline,
    // because the rectangle we were restoring to belongs to the screen
    // as it was. The window is ours to place once it is NoState, which
    // is also why nothing else will resize it back and why the baseline
    // has to be re-derived here rather than waited for.
    void onScreenGeometryChanged();

    // QInputMethod::cursorRectangleChanged — the one platform event that
    // re-runs scrollToCursor() with the keyboard, the window and the gap
    // all unchanged. Emits cursorMoved() when the move is a real one and
    // the keyboard is up; see the two guards in the implementation,
    // which between them are why this cannot feed back on itself.
    void onCursorRectangleChanged();

private:
    void trackWindow();
    // Hand the window back to the platform in the state the shell asked
    // for, which is what makes it recompute its own maximized rectangle
    // against the screen as it is now. See onScreenGeometryChanged().
    void restoreShellWindowState();
    void applyWindowGeometry(int keyboardHeight);
    void measureShrink();
    void setPlatformScroll(int value);
    void rememberCursorRect();

    QPointer<QWindow> m_window;
    // The window as it is with no keyboard: the thing we shrink from and
    // restore to. Re-taken whenever the keyboard is down.
    //
    // Always read off m_window, never off the screen. applyWindowGeometry()
    // feeds it to QWindow::setGeometry(), so it has to be the rectangle
    // QWindow::geometry() reports, and QScreen::availableGeometry() is not
    // that rectangle on the platform this file exists for —
    // onScreenGeometryChanged() has the numbers.
    QRect m_baseGeometry;

    // The window state the shell asked for, remembered before we take it
    // away. applyWindowGeometry() has to hold the window at
    // Qt::WindowNoState for QIOSWindow::setGeometry() to apply at all, and
    // this is what gets given back when the screen changes under us.
    // Defaults to what qml/mobile/MobileMain.qml actually asks for, for
    // the case where the screen changes before any keyboard ever appears.
    Qt::WindowStates m_shellWindowState = Qt::WindowMaximized;

    int m_platformScroll = 0;
    int m_windowShrink = 0;
    bool m_assumedScroll = false;

    // Half one of the loop guard: settle() provokes the platform by
    // republishing the cursor rectangle, and that comes straight back
    // here as cursorRectangleChanged. Taking our own provocation for a
    // fresh cursor move would be a settle that re-arms itself for as
    // long as the keyboard is up, which is worse than the bug.
    bool m_settling = false;

    // Half two, and the half that matters: the cursor rectangle as it
    // was the last time we looked or provoked. A settle only starts if
    // the cursor is somewhere NEW, which our own republish can never
    // make true — so the guard holds even when the platform delivers
    // its echo through the event loop rather than inside our call,
    // where m_settling is already false again.
    QRectF m_lastCursorRect;

    // Latched off after the platform ignores the geometry we ask for,
    // which hands keyboard avoidance back to it.
    bool m_resizeAvailable = true;
    int m_resizeRejections = 0;
    bool m_resizeApplied = false;

    QString m_lastLogged;
};
