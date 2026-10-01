#include "MobileKeyboard.h"

#include <QGuiApplication>
#include <QInputMethod>
#include <QLoggingCategory>
// Explicitly, not transitively: the macOS build happens to pull QRectF in
// through another header and the iOS framework headers do not, so leaving
// it out compiles on the desktop and fails only on the platform this file
// exists for.
#include <QRectF>
#include <QScreen>
#include <QStringList>
#include <QWindow>
#include <QtMath>

namespace bsfchat {
namespace platform {

#ifdef Q_OS_IOS
// Implemented in src/core/MobileKeyboardIos.mm. Returns how far the iOS
// platform plugin has translated the Qt scene up, in Qt logical pixels
// (0 when it has not).
int iosPlatformScrollOffset();
#else
inline int iosPlatformScrollOffset() { return 0; }
#endif

// Only iOS needs the window shrunk by hand. Android's
// windowSoftInputMode=adjustResize already does it, and doing it twice
// there would take the composer off the top of the screen.
constexpr bool kShrinkWindowForKeyboard =
#ifdef Q_OS_IOS
    true;
#else
    false;
#endif

// How many consecutive times the platform may ignore the geometry we ask
// for before we stop asking. Three, because settle() samples every 50ms
// and a resize that has merely not been applied yet should not be
// mistaken for one that never will be.
constexpr int kResizeRejectionLimit = 3;

} // namespace platform
} // namespace bsfchat

// Info rather than the project's usual QtWarningMsg default: this is the
// one subsystem whose numbers cannot be reasoned out from the source —
// the keyboard rectangle's position is polluted by the platform's own
// scroll, the safe-area insets are the device's, and whether the platform
// honours a geometry request is not knowable from here — so a build handed
// to somebody with a phone has to print them without anyone having to set
// QT_LOGGING_RULES first. It logs only when the numbers change, so a
// session is a handful of lines, not a stream.
Q_LOGGING_CATEGORY(logMobileKeyboard, "bsfchat.mobile.keyboard", QtInfoMsg)

MobileKeyboard::MobileKeyboard(QObject* parent) : QObject(parent)
{
    if (QInputMethod* im = QGuiApplication::inputMethod()) {
        connect(im, &QInputMethod::visibleChanged,
                this, &MobileKeyboard::onKeyboardChanged);
        connect(im, &QInputMethod::keyboardRectangleChanged,
                this, &MobileKeyboard::onKeyboardChanged);
        // The composer can move the cursor without touching any of the
        // above: a reply or edit banner grows it while the keyboard is
        // already up and settled. That is the case the device still
        // showed, because QIOSInputContext re-runs scrollToCursor() on
        // this change and, until this connection existed, nothing asked
        // it to recompute afterwards and nothing re-read how far it had
        // moved the scene.
        connect(im, &QInputMethod::cursorRectangleChanged,
                this, &MobileKeyboard::onCursorRectangleChanged);
    }
}

void MobileKeyboard::trackWindow()
{
    if (m_window) return;
    // The window whose text input raised the keyboard, which is the one
    // the platform plugin itself works on (QIOSInputContext reaches for
    // QGuiApplication::focusWindow()->winId()). Taking it from here rather
    // than having QML hand it over keeps the shell's diff to one
    // expression, which matters while other branches are editing the same
    // file.
    m_window = QGuiApplication::focusWindow();
    if (!m_window) return;
    if (QScreen* screen = m_window->screen()) {
        connect(screen, &QScreen::availableGeometryChanged,
                this, &MobileKeyboard::onScreenGeometryChanged,
                Qt::UniqueConnection);
    }
}

void MobileKeyboard::onKeyboardChanged()
{
    trackWindow();
    if (!m_window) return;

    QInputMethod* im = QGuiApplication::inputMethod();
    if (!im) return;

    // Height is the one part of keyboardRectangle that the platform's own
    // scroll does not corrupt; its position is converted through the
    // translated layer and cannot be trusted. See the header.
    const int keyboard = im->isVisible()
        ? int(qCeil(im->keyboardRectangle().height())) : 0;

    if (keyboard <= 0) {
        // Keyboard down: give the window back, then re-take the baseline
        // from whatever it settles at. Re-taking it here is what keeps a
        // rotation, a split view or a future inset change from leaving us
        // restoring to a stale rectangle.
        applyWindowGeometry(0);
        if (m_window->geometry().isValid())
            m_baseGeometry = m_window->geometry();
        m_assumedScroll = false;
        setPlatformScroll(0);
        measureShrink();
        rememberCursorRect();
        return;
    }

    if (!m_baseGeometry.isValid())
        m_baseGeometry = m_window->geometry();

    applyWindowGeometry(keyboard);
    measureShrink();

    // If the window did shrink there is nothing for the platform to do
    // and it should report zero. If it did not, the platform is about to
    // scroll — this signal is emitted before it does — so credit it with
    // the scroll now rather than discovering it 50ms later, which is the
    // difference between a correct first frame and one frame of the
    // composer floating over a void.
    m_assumedScroll = !m_resizeApplied;
    setPlatformScroll(m_resizeApplied ? 0 : keyboard);

    // Fresh baseline for the cursor watch. The keyboard opening moves
    // the cursor too, and that move is already handled by the path above
    // — re-reporting it as a composer-driven move would start a second
    // settle cadence on top of the one the gap change starts.
    rememberCursorRect();
}

void MobileKeyboard::onScreenGeometryChanged()
{
    // Once the window is NoState it is ours to place, so nothing else
    // will resize it back after a rotation. Re-baseline from the screen
    // and re-apply.
    if (!m_window) return;
    QScreen* screen = m_window->screen();
    if (!screen) return;

    const QRect available = screen->availableGeometry();
    if (!available.isValid()) return;
    m_baseGeometry = available;

    QInputMethod* im = QGuiApplication::inputMethod();
    const int keyboard = (im && im->isVisible())
        ? int(qCeil(im->keyboardRectangle().height())) : 0;
    applyWindowGeometry(keyboard);
    measureShrink();
}

void MobileKeyboard::onCursorRectangleChanged()
{
    // Our own provocation, arriving back inside settle()'s call to
    // QInputMethod::update(). Not a cursor move; not news.
    if (m_settling) return;

    QInputMethod* im = QGuiApplication::inputMethod();
    if (!im) return;

    // Only while the keyboard is up. With it down there is no platform
    // scroll to provoke and no gap to converge on, so every caret move
    // in the composer would start a 400ms timer for nothing.
    if (!im->isVisible()) {
        rememberCursorRect();
        return;
    }

    // The move has to be a real one. This is what makes the guard
    // total rather than merely likely: settle() re-reads the rectangle
    // AFTER asking the platform to recompute, so an echo — whether it
    // arrives inside that call or a turn of the event loop later — is
    // by construction the rectangle we already have, and stops here.
    // A loop would need the cursor to keep landing somewhere new, which
    // needs the layout to keep moving, which is exactly the thing that
    // stops once the push reaches its fixed point.
    const QRectF now = im->cursorRectangle();
    if (now == m_lastCursorRect) return;
    m_lastCursorRect = now;

    Q_EMIT cursorMoved();
}

void MobileKeyboard::rememberCursorRect()
{
    if (QInputMethod* im = QGuiApplication::inputMethod())
        m_lastCursorRect = im->cursorRectangle();
}

void MobileKeyboard::applyWindowGeometry(int keyboardHeight)
{
    if (!bsfchat::platform::kShrinkWindowForKeyboard) return;
    if (!m_resizeAvailable || !m_window || !m_baseGeometry.isValid()) return;

    QRect target = m_baseGeometry;
    if (keyboardHeight > 0) {
        // Never shrink to nothing: a keyboard taller than the window
        // would otherwise leave a zero-height scene, and a hardware
        // keyboard with a tall accessory bar can come close.
        target.setHeight(qMax(120, m_baseGeometry.height() - keyboardHeight));
    }

    if (m_window->geometry() == target) {
        m_resizeApplied = (keyboardHeight > 0);
        return;
    }

    // Order matters and is not interchangeable. QIOSWindow::setGeometry
    // records the rectangle but only applies it when the window state is
    // Qt::WindowNoState; the shell asks for Qt::WindowMaximized, under
    // which it is silently dropped. Recording first and flipping the
    // state second makes the flip apply the rectangle we want, so the
    // window is never shown at a stale size in between.
    m_window->setGeometry(target);
    if (m_window->windowState() != Qt::WindowNoState)
        m_window->setWindowState(Qt::WindowNoState);

    const QRect actual = m_window->geometry();
    const bool accepted = (actual.height() == target.height());

    if (accepted) {
        m_resizeRejections = 0;
        m_resizeApplied = (keyboardHeight > 0);
        return;
    }

    // Loud, not silent. A platform that quietly ignores the request is
    // exactly the failure this was warned about, so it is counted, named
    // in the log, and after a few tries handed back to the platform.
    m_resizeApplied = false;
    if (++m_resizeRejections < bsfchat::platform::kResizeRejectionLimit)
        return;

    m_resizeAvailable = false;
    qCWarning(logMobileKeyboard).noquote()
        << QStringLiteral("window resize REJECTED by the platform after %1 "
                          "attempts (asked %2x%3, got %4x%5) — falling back to "
                          "letting the platform scroll the scene, which takes "
                          "the header off screen while typing")
               .arg(m_resizeRejections)
               .arg(target.width()).arg(target.height())
               .arg(actual.width()).arg(actual.height());
}

void MobileKeyboard::measureShrink()
{
    if (!m_window || !m_baseGeometry.isValid()) return;
    // Measured, never assumed, and deliberately blind to who caused it:
    // this class on iOS, windowSoftInputMode=adjustResize on Android.
    // That is what lets the shell's push be one expression with no
    // per-platform branch.
    const int shrink = qMax(0, m_baseGeometry.height() - m_window->height());
    if (shrink == m_windowShrink) return;
    m_windowShrink = shrink;
    Q_EMIT windowShrinkChanged();
}

void MobileKeyboard::settle(const QVariantMap& state)
{
    // Sampled fresh, not taken from the cached property: the question
    // this line has to answer is whether asking the platform to recompute
    // changed anything, and a stale "before" cannot answer it.
    const int before = bsfchat::platform::iosPlatformScrollOffset();

    // Qt::ImCursorRectangle is the bit QIOSInputContext::update() tests
    // before re-running scrollToCursor(). Everything else in the query
    // set would be a no-op here, and ImHints/ImEnabled would additionally
    // ask the text responder to reconfigure, which can make the keyboard
    // itself flicker — so this is deliberately the single narrowest flag
    // that does the job.
    m_settling = true;
    if (QInputMethod* im = QGuiApplication::inputMethod())
        im->update(Qt::ImCursorRectangle);
    m_settling = false;
    // Taken after the recompute, so the rectangle we now hold is the one
    // our own request produced. See onCursorRectangleChanged().
    rememberCursorRect();

    const int after = bsfchat::platform::iosPlatformScrollOffset();
    const bool wasAssumed = m_assumedScroll;
    m_assumedScroll = false;
    setPlatformScroll(after);
    measureShrink();

    if (state.isEmpty()) return;

    // One line, sorted so it diffs cleanly between captures. `scroll` is
    // the platform's translation either side of the recompute request, so
    // "345->345" means it ignored us and "345->0" means it stood down.
    // `resize` is the whole point of this build: shrunk means the window
    // moved and the keyboard should no longer intersect it at all.
    QStringList parts;
    parts << QStringLiteral("scroll=%1->%2").arg(before).arg(after);
    parts << QStringLiteral("wasAssumed=%1").arg(wasAssumed ? 1 : 0);
    parts << QStringLiteral("shrink=%1").arg(m_windowShrink);
    parts << QStringLiteral("resize=%1").arg(
        !bsfchat::platform::kShrinkWindowForKeyboard ? QStringLiteral("n/a")
        : !m_resizeAvailable ? QStringLiteral("rejected")
        : m_resizeApplied    ? QStringLiteral("shrunk")
                             : QStringLiteral("idle"));
    parts << QStringLiteral("base=%1x%2")
                 .arg(m_baseGeometry.width()).arg(m_baseGeometry.height());
    for (auto it = state.constBegin(); it != state.constEnd(); ++it)
        parts << QStringLiteral("%1=%2").arg(it.key(), it.value().toString());
    parts.sort();

    const QString line = parts.join(QLatin1Char(' '));
    if (line == m_lastLogged) return;
    m_lastLogged = line;
    qCInfo(logMobileKeyboard).noquote() << line;
}

void MobileKeyboard::setPlatformScroll(int value)
{
    if (value == m_platformScroll) return;
    m_platformScroll = value;
    Q_EMIT platformScrollChanged();
}
