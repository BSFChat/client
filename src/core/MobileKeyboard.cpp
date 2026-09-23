#include "MobileKeyboard.h"

#include <QGuiApplication>
#include <QInputMethod>
#include <QLoggingCategory>
#include <QStringList>

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
// the keyboard rectangle's units differ per platform and the safe-area
// insets are the device's — so a build handed to somebody with a phone
// has to print them without anyone having to set QT_LOGGING_RULES first.
// It logs only when the numbers change, so a session is a handful of
// lines, not a stream.
Q_LOGGING_CATEGORY(logMobileKeyboard, "bsfchat.mobile.keyboard", QtInfoMsg)

MobileKeyboard::MobileKeyboard(QObject* parent) : QObject(parent) {}

void MobileKeyboard::settle(const QVariantMap& state)
{
    // Qt::ImCursorRectangle is the bit QIOSInputContext::update() tests
    // before re-running scrollToCursor(). Everything else in the query
    // set would be a no-op here, and ImHints/ImEnabled would additionally
    // ask the text responder to reconfigure, which can make the keyboard
    // itself flicker — so this is deliberately the single narrowest flag
    // that does the job.
    if (QInputMethod* im = QGuiApplication::inputMethod())
        im->update(Qt::ImCursorRectangle);

    refresh(state);
}

void MobileKeyboard::refresh(const QVariantMap& state)
{
    const int before = m_platformScroll;
    m_platformScroll = bsfchat::platform::iosPlatformScrollOffset();

    if (m_platformScroll != before)
        Q_EMIT platformScrollChanged();

    if (state.isEmpty()) return;

    // One line, sorted so it diffs cleanly between captures, with the
    // platform scroll on both sides of the update() call — if those two
    // differ, the request to recompute is doing something.
    QStringList parts;
    parts << QStringLiteral("platformScroll=%1->%2").arg(before).arg(m_platformScroll);
    for (auto it = state.constBegin(); it != state.constEnd(); ++it)
        parts << QStringLiteral("%1=%2").arg(it.key(), it.value().toString());
    parts.sort();

    const QString line = parts.join(QLatin1Char(' '));
    if (line == m_lastLogged) return;
    m_lastLogged = line;
    qCInfo(logMobileKeyboard).noquote() << line;
}
