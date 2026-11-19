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
// the configured key (with its modifiers) opens, the release of that same
// physical key (native scan code / virtual key) closes —
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
    // Settings is capturing a new key: let every key through meanwhile.
    void setSuspended(bool suspended);

    // ---- the settings field (audit 3) ------------------------------
    // The default: Ctrl+Space, except on macOS, where Qt's "Ctrl" is
    // Command — and Cmd+Space is Spotlight, Control+Space the input-source
    // switch. Option+Space (portable "Alt+Space") is free there.
    // Inline: Settings.cpp uses it, in test targets without this .cpp.
    static QString defaultSequence()
    {
#ifdef Q_OS_MACOS
        return QStringLiteral("Alt+Space");
#else
        return QStringLiteral("Ctrl+Space");
#endif
    }
    // Portable text for a key the user just pressed in the capture field.
    // "" while only modifiers are down (keep waiting for the real key).
    static QString sequenceFor(int key, Qt::KeyboardModifiers mods);
    // "" when `sequence` is usable, otherwise why not (for an inline
    // message): not one key, a bare modifier, or a key that typing needs
    // (a letter, digit, Space, Enter… with no modifier would be swallowed
    // while PTT is on, and could not be typed into the composer).
    static QString problem(const QString& sequence);
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
    bool m_suspended = false;
    int m_key = 0;
    Qt::KeyboardModifiers m_mods;
    // The physical key that opened the transmission. Its release (and
    // repeats) are matched on this, not on key(): with Shift in the
    // combination the key code itself changes when Shift is let go first
    // ("!" pressed, "1" released), which left PTT stuck open or never
    // matching at all.
    quint32 m_heldScanCode = 0;
    quint32 m_heldVirtualKey = 0;
    bool isHeldKey(const QKeyEvent* event) const;
};
