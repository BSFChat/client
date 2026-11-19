#include "net/PttKeyFilter.h"

#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QKeySequence>

PttKeyFilter::PttKeyFilter(QObject* parent) : QObject(parent) {}

PttKeyFilter::~PttKeyFilter()
{
    if (m_installed && qApp) qApp->removeEventFilter(this);
}

void PttKeyFilter::install()
{
    auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    if (!app || m_installed) return;
    app->installEventFilter(this);
    m_installed = true;
    connect(app, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
                if (state != Qt::ApplicationActive) release();
            });
}

void PttKeyFilter::configure(bool enabled, const QString& sequence)
{
    const QKeySequence seq(sequence, QKeySequence::PortableText);
    if (seq.isEmpty()) {
        m_key = 0;
        m_mods = {};
    } else {
        const QKeyCombination combo = seq[0];
        m_key = int(combo.key());
        m_mods = combo.keyboardModifiers();
    }
    m_enabled = enabled && m_key != 0;
    if (!m_enabled) release();
}

void PttKeyFilter::release()
{
    setPressed(false);
}

void PttKeyFilter::setSuspended(bool suspended)
{
    m_suspended = suspended;
    if (suspended) release();
}

namespace {
bool isModifierKey(int key)
{
    switch (key) {
    case Qt::Key_Shift: case Qt::Key_Control: case Qt::Key_Meta: case Qt::Key_Alt:
    case Qt::Key_AltGr: case Qt::Key_CapsLock: case Qt::Key_NumLock:
    case Qt::Key_Super_L: case Qt::Key_Super_R: case Qt::Key_Hyper_L: case Qt::Key_Hyper_R:
        return true;
    default:
        return false;
    }
}
} // namespace

QString PttKeyFilter::sequenceFor(int key, Qt::KeyboardModifiers mods)
{
    if (key == 0 || key == Qt::Key_unknown || isModifierKey(key)) return {};
    mods &= (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    return QKeySequence(QKeyCombination(mods, Qt::Key(key)))
        .toString(QKeySequence::PortableText);
}

QString PttKeyFilter::problem(const QString& sequence)
{
    const QString text = sequence.trimmed();
    if (text.isEmpty()) return tr("Press the key you want to use.");
    const QKeySequence seq(text, QKeySequence::PortableText);
    if (seq.isEmpty() || seq[0].key() == Qt::Key_unknown)
        return tr("\"%1\" is not a key BSFChat recognises.").arg(text);
    if (seq.count() > 1) return tr("Use a single key, optionally with modifiers.");
    const QKeyCombination combo = seq[0];
    const int key = int(combo.key());
    if (isModifierKey(key)) return tr("A modifier on its own cannot be the key.");
    const Qt::KeyboardModifiers mods = combo.keyboardModifiers() & ~Qt::ShiftModifier;
    const bool typingKey = (key >= Qt::Key_A && key <= Qt::Key_Z)
        || (key >= Qt::Key_0 && key <= Qt::Key_9)
        || key == Qt::Key_Space || key == Qt::Key_Return || key == Qt::Key_Enter
        || key == Qt::Key_Tab || key == Qt::Key_Backtab || key == Qt::Key_Backspace
        || key == Qt::Key_Escape || key == Qt::Key_Delete;
    if (typingKey && mods == Qt::NoModifier)
        return tr("%1 is needed for typing. Add a modifier, or pick a key like F13 or `.")
            .arg(seq.toString(QKeySequence::NativeText));
    return {};
}

bool PttKeyFilter::isHeldKey(const QKeyEvent* ev) const
{
    if (m_heldScanCode != 0 && ev->nativeScanCode() != 0)
        return ev->nativeScanCode() == m_heldScanCode;
    if (m_heldVirtualKey != 0 && ev->nativeVirtualKey() != 0)
        return ev->nativeVirtualKey() == m_heldVirtualKey;
    return ev->key() == m_key;
}

void PttKeyFilter::setPressed(bool pressed)
{
    if (m_pressed == pressed) return;
    m_pressed = pressed;
    emit pressedChanged(pressed);
}

bool PttKeyFilter::handleKeyEvent(const QKeyEvent* ev)
{
    if (!ev || m_key == 0) return false;
    if (m_pressed) {
        // Held: the same physical key's repeats and release, however its
        // key code reads now that the modifiers may have changed.
        if (!isHeldKey(ev)) return false;
        if (ev->type() == QEvent::KeyRelease) {
            // Auto-repeat on X11/Windows comes as release+press pairs
            // flagged isAutoRepeat; only the real release ends it.
            if (ev->isAutoRepeat()) return true;
            setPressed(false);
            return true;
        }
        return ev->type() == QEvent::KeyPress;   // swallow repeats
    }
    if (ev->key() != m_key) return false;
    if (ev->type() != QEvent::KeyPress) return false;
    if (!m_enabled || m_suspended) return false;
    // Modifiers must match exactly (keypad flag aside) on the opening edge,
    // so Ctrl+Space does not fire on Ctrl+Shift+Space or plain Space.
    const Qt::KeyboardModifiers mods = ev->modifiers() & ~Qt::KeypadModifier;
    if (mods != m_mods) return false;
    // (An auto-repeat press whose first press we never saw — focus arrived
    // mid-hold — also opens: the key IS down.)
    m_heldScanCode = ev->nativeScanCode();
    m_heldVirtualKey = ev->nativeVirtualKey();
    setPressed(true);
    return true;
}

bool PttKeyFilter::eventFilter(QObject* watched, QEvent* event)
{
    const auto type = event->type();
    if ((type == QEvent::KeyPress || type == QEvent::KeyRelease)
        && watched && watched->isWindowType()) {
        if (handleKeyEvent(static_cast<QKeyEvent*>(event))) return true;
    }
    return QObject::eventFilter(watched, event);
}
