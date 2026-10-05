#pragma once

// The QML-visible face of native gamepad input.
//
// Owns SDL's gamepad subsystem, pumps it, translates SDL's events into
// bsfchat::input's vocabulary, hands them to GamepadCore and emits the
// actions it gets back as Qt signals. There is no decision-making in
// this class on purpose: everything that could be got wrong lives in the
// SDL-free headers next door, where it is unit-tested without a
// controller. This file is the part that cannot be.
//
// Nothing here is wired into QML yet. The type is registered and the
// signals exist; binding them to a focus chain is a deliberately
// separate, small step for once the QML focus work lands.
//
// ---------------------------------------------------------------------
// WHY SDL AT ALL
// ---------------------------------------------------------------------
//
// Qt 6 removed QtGamepad and shipped no replacement. The alternatives
// were raw evdev (Linux only, and then XInput and IOKit/GameController
// written by hand for the other two), or SDL. SDL's gamepad API — as
// distinct from its joystick API — applies a mapping database so that
// "the south face button" and "the left trigger" mean the same thing on
// an Xbox pad, a DualSense, a Switch Pro controller and a Steam Deck,
// with no per-device code here. That database is the feature; writing
// the platform backends would be the easy part by comparison.
//
// The database is COMPILED IN (SDL's src/joystick/SDL_gamepad_db.h,
// ~900 entries), so there is no data file to ship, install or keep in
// sync. SDL also reads the SDL_GAMECONTROLLERCONFIG environment variable
// and a user-level mapping file by itself, so a user with an exotic pad
// has a supported way to add a mapping without a release from us.
//
// ---------------------------------------------------------------------
// WHY ONLY THE GAMEPAD SUBSYSTEM
// ---------------------------------------------------------------------
//
// SDL_Init(SDL_INIT_GAMEPAD) and nothing else, and the CMake build
// disables SDL's audio, video, render, GPU, camera, sensor, power,
// dialog and tray subsystems outright (see cmake/Dependencies.cmake).
// This application already owns its audio devices (src/voice/), its
// rendering (Qt Quick) and its event loop. A second library that also
// believes it owns any of those is not a bigger dependency, it is a
// conflict: SDL's audio backend would open the same CoreAudio/WASAPI
// devices the voice pipeline is using, and SDL_INIT_VIDEO installs
// platform event hooks that fight Qt's.
//
// ---------------------------------------------------------------------
// WHY A QTimer, AND NOT THE OTHER TWO OPTIONS
// ---------------------------------------------------------------------
//
// SDL only produces gamepad events while something calls SDL_PumpEvents
// (or SDL_UpdateGamepads, which is the narrower call we use). Three
// integrations were considered.
//
// 1. A DEDICATED THREAD running SDL_WaitEvent and posting to the GUI
//    thread. REJECTED, and not on taste — it does not work.
//
//    SDL's macOS IOKit backend schedules its HID device sources on
//    `CFRunLoopGetCurrent()` of whichever thread called SDL_Init, using
//    a private run-loop mode, and then drains that mode itself from
//    inside its Update call (SDL_iokitjoystick.c: IOHIDManagerScheduleWithRunLoop
//    at init, CFRunLoopRunInMode during detect/update). Initialise on
//    thread A and pump on thread B and the sources are scheduled on a
//    run loop nobody ever runs: zero events, forever, with no error.
//    The GameController backend is worse — it registers
//    NSNotificationCenter observers for connect/disconnect with
//    `queue:nil`, and the framework posts those on the main queue, so
//    hotplug notifications are delivered to the main thread regardless
//    of where SDL was initialised. Windows' XInput and Linux's
//    evdev/udev backends are less fussy, but a design that is correct on
//    one platform and silently inert on another is not a design.
//
//    SDL_WaitEvent is also not the win it looks like: for joystick
//    backends it internally polls on a short timer rather than blocking
//    on a device, so it busy-polls anyway — just on a thread where the
//    cost is invisible, and with a cross-thread hop added to every event.
//
// 2. A QAbstractEventDispatcher::aboutToBlock HOOK. REJECTED: it is
//    exactly backwards. aboutToBlock fires when the Qt loop is about to
//    go idle, and an idle Qt application then blocks in the platform
//    loop indefinitely. So after the hook returns there is nothing left
//    to wake the process — the next pump happens whenever some unrelated
//    event arrives. The latency is unbounded, and it is unbounded
//    precisely in the common case: a user navigating with a controller is
//    generating no keyboard, mouse or network traffic, so the app is
//    idle, so nothing wakes it, so the controller appears dead. It is
//    also the configuration that would test perfectly on a developer's
//    machine, where something is always arriving.
//
// 3. A QTimer ON THE GUI THREAD. CHOSEN. Same thread as SDL_Init, so
//    (1)'s run-loop problem does not arise; it brings its own wakeups, so
//    (2)'s problem does not either; and because the interval is ours to
//    choose, the battery cost is a decision rather than a side effect.
//
//    The interval is not fixed. GamepadPollCadence.h computes it from
//    what is attached and what is being held: 250 ms with no controller
//    (where the only job is to notice a hotplug), 8 ms while in use, and
//    50 ms while attached but idle. A fixed 8 ms timer — the obvious
//    implementation — is 125 wakeups a second for the entire life of the
//    process, including the overwhelming majority of it with no
//    controller attached at all, which on the handheld this feature is
//    mostly for is a battery cost for literally nothing.
//
//    The timer can never stop entirely, and that is the one genuinely
//    unavoidable cost here: SDL discovers a newly attached pad only
//    during a pump, so there is no "wake me when a controller appears"
//    to wait on.
//
// THREADING: GUI thread only. SDL is initialised, pumped and shut down
// on it, and every signal is emitted from it.

#include <QObject>
#include <QString>
#include <QTimer>
#include <QElapsedTimer>

#include "input/GamepadCore.h"
#include "input/GamepadTypes.h"

namespace bsfchat::input {

class GamepadBridge : public QObject {
    Q_OBJECT

    // Whether this build has a gamepad backend at all. CONSTANT because
    // it is a build-time fact: SDL is compiled out on iOS and Android
    // (see cmake/Dependencies.cmake for why). Exposed rather than
    // #ifdef'd away so QML never has to guard on the platform — the
    // object always exists, and on mobile it simply reports nothing,
    // exactly as core/Haptics does for desktop.
    Q_PROPERTY(bool available READ available CONSTANT)

    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(QString controllerName READ controllerName
                   NOTIFY controllerNameChanged)
    Q_PROPERTY(int controllerCount READ controllerCount NOTIFY connectedChanged)

public:
    explicit GamepadBridge(QObject* parent = nullptr);
    ~GamepadBridge() override;

    bool available() const;
    bool connected() const { return m_core.connected(); }
    int controllerCount() const { return m_core.controllerCount(); }
    QString controllerName() const { return m_core.primaryName(); }

    // Current normalised value of an axis: -1..1 for sticks, 0..1 for
    // triggers, aggregated across attached pads. For a QML binding that
    // wants to read rather than listen; axisChanged() is the push form.
    Q_INVOKABLE qreal axisValue(bsfchat::input::GamepadAxis axis) const
    {
        return m_core.axisValue(axis);
    }

    // Whether a button is held on any attached pad.
    Q_INVOKABLE bool buttonHeld(bsfchat::input::GamepadButton button) const
    {
        return m_core.buttonHeld(button);
    }

    // ---- test seam ----
    //
    // Injects a platform event as though SDL had produced it, with an
    // explicit clock reading, and emits whatever it produces. Everything
    // this class does other than talking to SDL is reachable through
    // here, which is how the hotplug and cadence behaviour is tested on
    // a machine with no controller attached.
    //
    // Deliberately NOT Q_INVOKABLE: it is not part of the QML surface,
    // and a synthetic-input entry point reachable from QML is a thing
    // that eventually gets used in production by accident.
    void injectForTest(const GamepadEvent& event, qint64 nowMs);
    void tickForTest(qint64 nowMs);

signals:
    void connectedChanged();
    void controllerNameChanged();

    // A semantic button went down / came up. Edges of the logical OR
    // across every attached pad, so two pads holding the same button
    // produce one press and one release.
    //
    // A release is ALWAYS delivered, including when the controller is
    // unplugged while the button is held — SDL sends no button-up in
    // that case and this layer synthesises one. Any consumer that opens
    // something on press (a microphone, most obviously) can rely on
    // getting the close.
    void buttonPressed(bsfchat::input::GamepadButton button);
    void buttonReleased(bsfchat::input::GamepadButton button);

    // One directional step for a focus chain, already deadzoned,
    // hysteresis-filtered and repeat-paced. `repeat` is false for the
    // step the user's press produced and true for each auto-repeat step.
    //
    // Consume THIS rather than axisChanged() for navigation. A focus
    // chain driven off raw axis values skips several items per push,
    // which is the single most common way this feature is got wrong.
    void navigate(bsfchat::input::NavDirection direction, bool repeat);

    // Normalised analogue value changed: -1..1 for sticks, 0..1 for
    // triggers. Rate-limited by a report epsilon, so a stick held still
    // is silent.
    void axisChanged(bsfchat::input::GamepadAxis axis, qreal value);

private:
    void poll();
    void dispatch(const QList<GamepadAction>& actions);
    void rescheduleTimer();
    qint64 nowMs() const;

    GamepadCore m_core;
    QTimer m_timer;
    QElapsedTimer m_clock;
    // Cached so connectedChanged / controllerNameChanged are emitted on
    // real transitions only.
    bool m_wasConnected = false;
    QString m_lastName;
    bool m_sdlReady = false;
};

} // namespace bsfchat::input
