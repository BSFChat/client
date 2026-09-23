// Software-keyboard bridge for the mobile shell (qml/mobile/MobileMain.qml).
//
// WHY THIS EXISTS
//
// On iOS the window is never resized for the keyboard. Instead
// QIOSInputContext::scrollToCursor() translates the ENTIRE Qt scene up,
// by setting a translation on the root UIView's layer.sublayerTransform,
// until the text cursor is clear of the keyboard. That is a reasonable
// default for an app with no chrome of its own, and wrong for this one:
// it takes the channel header off the top of the screen along with
// everything else, so while you type there is no channel name, no member
// list button and no overflow menu, and the top message row renders under
// the status bar.
//
// So the shell does its own, better-shaped push: it shrinks the content
// area from the bottom, which lifts the composer and leaves the header
// alone. The problem that makes this class necessary is that the two
// compose. Qt's scroll is computed at UIKeyboardWillShow, from the cursor
// position as it was BEFORE the shell's push; the shell then pushes on
// top of that, and the composer ends up a whole extra keyboard height in
// the air with a void underneath it. That is what shipped in the first
// cut of feat/mobile-ui-touch and what the device showed.
//
// There is no switch for it. The shipped plugin
// (Qt/6.10.3/ios/plugins/platforms/libqios.a, qiosinputcontext.mm.o)
// carries no environment variable and no property that turns the scroll
// off; its only early-out is for application extensions. What it DOES
// have, read out of the disassembly rather than assumed:
//
//   * scrollToCursor() calls scroll(0) — i.e. undoes itself — as soon as
//     the cursor rectangle is contained in (root view bounds − keyboard
//     rectangle). Once the shell's push has landed, that is true.
//   * QIOSInputContext::update(queries) calls scrollToCursor() whenever
//     queries carries Qt::ImCursorRectangle (bit 1). QInputMethod::update
//     is public API.
//
// So the supported way to "suppress" the platform scroll is to get out of
// the keyboard's way and then ask the platform to look again. That is
// settle(). And because nothing guarantees it worked, the shell also
// subtracts whatever the platform has actually scrolled from its own
// push, which it reads back here.
//
// WHAT THE DEVICE SAID (iPhone 16 Pro Max, iOS 26, 440x956pt)
//
// It did not stand down. It scrolled the full keyboard height and stayed
// there, so the subtraction took our push to zero and the platform owns
// keyboard avoidance in practice. Three things the capture settled:
//
//   * keyboardRectangle is in logical points on iOS, not device pixels
//     (the logged width ratio was exactly 1.000). The shell's
//     normalisation is right and needs no devicePixelRatio division.
//   * Qt already lays the window out INSIDE the safe area — the window
//     was 440x860 on a 440x956 screen, 96 = 62 + 34 — so SafeArea
//     margins of 0 are correct and the header was never under the
//     Dynamic Island. Content appearing under the status clock was the
//     scene translation, nothing else.
//   * keyboardRectangle is itself polluted by the scroll. Reported y was
//     894 where the keyboard's true top is 611 on screen and 549 in
//     window coordinates: 611 + 345(scroll) - 62(window origin) = 894.
//     The rectangle is converted through the translated layer, so only
//     its SIZE can be trusted, never its position.
//
// That last point also explains why scrollToCursor() will not stand
// down. Its containment test compares QPlatformInputContext::
// cursorRectangle(), which is in Qt WINDOW coordinates, against a region
// built from UIKit rects — [focusView window] and [UIScreen mainScreen]
// bounds, via CGRectGetMaxY. On a device where the window is inset 62pt
// from the top of the screen those are not the same space, and the
// keyboard rectangle it subtracts has the scroll baked into it. The test
// is not something the shell can satisfy from the outside.
//
// So the shell assumes the platform will scroll, from the instant the
// keyboard becomes visible (assumePlatformScroll below), instead of
// discovering it 50ms later. That is the difference between "the
// composer is where it should be" and the one frame of composer floating
// over a void that the owner saw. If a future Qt does stand down, the
// samples correct downwards and the shell takes the push back — from too
// little room to the right amount, which is the harmless direction.
#pragma once

#include <QObject>
#include <QVariantMap>

class MobileKeyboard : public QObject {
    Q_OBJECT

    // How far the platform has translated the whole scene up, in Qt
    // logical pixels. Always 0 off iOS. Refreshed by settle(); the shell
    // binds its push to it.
    Q_PROPERTY(int platformScroll READ platformScroll NOTIFY platformScrollChanged)

public:
    explicit MobileKeyboard(QObject* parent = nullptr);

    int platformScroll() const { return m_platformScroll; }

    // Ask the platform input context to recompute its own scroll against
    // the layout as it is NOW, then re-read the result. Call it after the
    // keyboard-driven layout change has been applied — and again a few
    // times while the keyboard animates, because the plugin recomputes
    // from whatever the cursor rectangle happens to be when asked.
    //
    // `state` is the shell's own numbers, logged alongside the platform's
    // so one line says whether the two agree. Pass an empty map to skip
    // the logging. The log samples the platform scroll FRESH on both
    // sides of the recompute request: the first version of this compared
    // against the previous sample, which cannot tell "we caused this"
    // from "we had not looked since before the keyboard opened", and that
    // ambiguity cost a device round trip.
    Q_INVOKABLE void settle(const QVariantMap& state = QVariantMap());

Q_SIGNALS:
    void platformScrollChanged();

private Q_SLOTS:
    // The platform emits keyboardRectangleChanged BEFORE it scrolls (the
    // scroll is the tail call of its own keyboardWillShow handler), so a
    // shell that measures at that moment measures zero and pushes the
    // full keyboard height on top of a scroll that is about to happen.
    // Assume instead: the platform has always scrolled, so credit it with
    // the keyboard's height until a real sample says otherwise.
    void assumePlatformScroll();

private:
    void setPlatformScroll(int value);

    int m_platformScroll = 0;
    bool m_assumed = false;
    QString m_lastLogged;
};
