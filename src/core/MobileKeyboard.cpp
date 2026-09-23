#include "MobileKeyboard.h"

#include <QGuiApplication>
#include <QInputMethod>
#include <QLoggingCategory>
// Explicitly, not transitively: the macOS build happens to pull QRectF in
// through another header and the iOS framework headers do not, so leaving
// it out compiles on the desktop and fails only on the platform this file
// exists for.
#include <QRectF>
#include <QStringList>
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

} // namespace platform
} // namespace bsfchat

// Info rather than the project's usual QtWarningMsg default: this is the
// one subsystem whose numbers cannot be reasoned out from the source —
// the keyboard rectangle's position is polluted by the platform's own
// scroll and the safe-area insets are the device's — so a build handed to
// somebody with a phone has to print them without anyone having to set
// QT_LOGGING_RULES first. It logs only when the numbers change, so a
// session is a handful of lines, not a stream.
Q_LOGGING_CATEGORY(logMobileKeyboard, "bsfchat.mobile.keyboard", QtInfoMsg)

MobileKeyboard::MobileKeyboard(QObject* parent) : QObject(parent)
{
    if (QInputMethod* im = QGuiApplication::inputMethod()) {
        connect(im, &QInputMethod::visibleChanged,
                this, &MobileKeyboard::assumePlatformScroll);
        connect(im, &QInputMethod::keyboardRectangleChanged,
                this, &MobileKeyboard::assumePlatformScroll);
    }
}

void MobileKeyboard::assumePlatformScroll()
{
#ifdef Q_OS_IOS
    QInputMethod* im = QGuiApplication::inputMethod();
    if (!im) return;

    // Only ever an assumption for the gap between the platform saying
    // "keyboard" and the platform acting on it; the first real sample
    // from settle() replaces it. Height is the one part of
    // keyboardRectangle that the scroll does not corrupt.
    const int assumed = im->isVisible()
        ? int(qCeil(im->keyboardRectangle().height())) : 0;
    m_assumed = true;
    setPlatformScroll(assumed);
#endif
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
    if (QInputMethod* im = QGuiApplication::inputMethod())
        im->update(Qt::ImCursorRectangle);

    const int after = bsfchat::platform::iosPlatformScrollOffset();
    const bool wasAssumed = m_assumed;
    m_assumed = false;
    setPlatformScroll(after);

    if (state.isEmpty()) return;

    // One line, sorted so it diffs cleanly between captures. `scroll` is
    // the platform's translation either side of the recompute request, so
    // "345->345" means it ignored us and "345->0" means it stood down.
    QStringList parts;
    parts << QStringLiteral("scroll=%1->%2").arg(before).arg(after);
    parts << QStringLiteral("wasAssumed=%1").arg(wasAssumed ? 1 : 0);
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
