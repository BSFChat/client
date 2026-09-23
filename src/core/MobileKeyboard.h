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
// settle(). And because nothing guarantees it worked — a future Qt could
// change the rule — the shell also subtracts whatever the platform has
// actually scrolled from its own push, which it reads back here. The two
// together converge on "platform scrolls 0, shell pushes the keyboard's
// height", and degrade to "platform scrolls, shell pushes nothing" rather
// than to the doubled push.
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
    // the logging.
    Q_INVOKABLE void settle(const QVariantMap& state = QVariantMap());

Q_SIGNALS:
    void platformScrollChanged();

private:
    void refresh(const QVariantMap& state);

    int m_platformScroll = 0;
    QString m_lastLogged;
};
