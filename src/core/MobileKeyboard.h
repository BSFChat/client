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

private Q_SLOTS:
    // The platform emits keyboardRectangleChanged BEFORE it scrolls — the
    // scroll is the tail call of its own keyboardWillShow handler — so
    // this runs while there is still time to shrink the window out of the
    // keyboard's way and have the plugin find nothing to do.
    void onKeyboardChanged();

    // Rotation. The window is ours to place once it is NoState, so
    // nothing else will resize it back.
    void onScreenGeometryChanged();

private:
    void trackWindow();
    void applyWindowGeometry(int keyboardHeight);
    void measureShrink();
    void setPlatformScroll(int value);

    QPointer<QWindow> m_window;
    // The window as it is with no keyboard: the thing we shrink from and
    // restore to. Re-taken whenever the keyboard is down.
    QRect m_baseGeometry;

    int m_platformScroll = 0;
    int m_windowShrink = 0;
    bool m_assumedScroll = false;

    // Latched off after the platform ignores the geometry we ask for,
    // which hands keyboard avoidance back to it.
    bool m_resizeAvailable = true;
    int m_resizeRejections = 0;
    bool m_resizeApplied = false;

    QString m_lastLogged;
};
