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

void PttKeyFilter::setPressed(bool pressed)
{
    if (m_pressed == pressed) return;
    m_pressed = pressed;
    emit pressedChanged(pressed);
}

bool PttKeyFilter::handleKeyEvent(const QKeyEvent* ev)
{
    if (!ev || m_key == 0) return false;
    if (ev->key() != m_key) return false;
    if (ev->type() == QEvent::KeyRelease) {
        // Auto-repeat on X11/Windows comes as release+press pairs flagged
        // isAutoRepeat; only the real release ends the transmission.
        if (ev->isAutoRepeat()) return m_pressed;
        if (!m_pressed) return false;
        setPressed(false);
        return true;
    }
    if (ev->type() != QEvent::KeyPress) return false;
    if (!m_enabled) return false;
    if (m_pressed) return true;           // held: swallow repeats
    // Modifiers must match exactly (keypad flag aside) on the opening edge,
    // so Ctrl+Space does not fire on Ctrl+Shift+Space or plain Space.
    const Qt::KeyboardModifiers mods = ev->modifiers() & ~Qt::KeypadModifier;
    if (mods != m_mods) return false;
    // (An auto-repeat press whose first press we never saw — focus arrived
    // mid-hold — also opens: the key IS down.)
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
