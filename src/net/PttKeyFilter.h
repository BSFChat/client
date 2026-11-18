#pragma once

// Push-to-talk key tracking, application-wide (audit 2 V2-2).
//
// The PTT key used to be a QML Shortcut with autoRepeat plus a 120 ms
// "no repeat seen lately" release timer. OS key repeat starts only after
// a 250-600 ms delay, so the timer fired during that delay on every
// press: the mic closed a fraction of a second into each sentence and
// re-opened on the first repeat — speech was clipped every time, and
// every flap was a voice/state PUT.
//
// This tracks the real edges instead: the first non-auto-repeat press of
// the configured key (with its modifiers) opens, its release closes —
// whatever the modifiers do in between. Auto-repeat presses are swallowed
// (so a held Ctrl+Space does not type spaces into the composer) and never
// change state. Losing application focus releases, because the release
// event will then go to another application.
//
// Installed on the QGuiApplication; it looks only at key events addressed
// to a QWindow, which is the first stop of every key event and therefore
// sees each keystroke exactly once (Qt Quick re-sends the same event to
// each item it tries, and an application filter would see all of those).

#include <QKeyCombination>
#include <QObject>
#include <QString>

class QKeyEvent;

class PttKeyFilter : public QObject {
    Q_OBJECT
public:
    explicit PttKeyFilter(QObject* parent = nullptr);
    ~PttKeyFilter() override;

    // Installs on qApp when there is a QGuiApplication; harmless otherwise.
    void install();

    // `sequence` in QKeySequence portable text ("Ctrl+Space"). Only its
    // first combination is used. Disabling releases.
    void configure(bool enabled, const QString& sequence);
    bool enabled() const { return m_enabled; }
    bool pressed() const { return m_pressed; }

    // The event logic, callable without an application (tests). Returns
    // true when the event belonged to PTT and should be consumed.
    bool handleKeyEvent(const QKeyEvent* event);
    // Force a release (focus lost, voice ended).
    void release();

signals:
    void pressedChanged(bool pressed);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setPressed(bool pressed);

    bool m_enabled = false;
    bool m_installed = false;
    bool m_pressed = false;
    int m_key = 0;
    Qt::KeyboardModifiers m_mods;
};
