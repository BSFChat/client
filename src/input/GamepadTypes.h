#pragma once

// The vocabulary of the gamepad input layer: semantic buttons, axes,
// directional intent, and the event/action structs the decision code
// passes around.
//
// Everything here is deliberately free of SDL. The translation from
// SDL_GamepadButton / SDL_GamepadAxis into these enums happens in exactly
// one place (GamepadBridge.cpp) as an explicit switch, and these values
// are explicitly numbered. Both of those are on purpose:
//
//   * The numbers are part of the QML surface once the focus chain binds
//     to the signals, so they must not move when the SDL pin moves.
//   * An explicit switch with no default fallthrough to a *valid* value
//     means a new SDL button we do not know about arrives as Unknown and
//     is dropped, rather than silently aliasing onto whichever of our
//     values happens to share its integer.
//
// SEMANTIC, NOT PHYSICAL. `South`/`East`/`West`/`North` are positions on
// the face-button diamond, which is what SDL's gamepad API reports after
// applying its mapping database. They are not letters: South is A on an
// Xbox pad, Cross on a PlayStation pad and B on a Nintendo pad, and the
// whole point of using SDL's gamepad API rather than its joystick API is
// that we never have to know which. Naming these `A`/`B`/`X`/`Y` would
// re-import exactly the per-vendor confusion the mapping database exists
// to remove, and would read as a lie on two of the three major pads.

#include <QObject>
#include <QString>
#include <QtGlobal>

namespace bsfchat::input {

Q_NAMESPACE

// Semantic buttons. Values are explicit and permanent; append only.
enum class GamepadButton {
    Unknown = 0,

    // Face diamond, by position. See the note above about why these are
    // not called A/B/X/Y.
    South = 1,   // Xbox A      / PlayStation Cross    / Nintendo B
    East = 2,    // Xbox B      / PlayStation Circle   / Nintendo A
    West = 3,    // Xbox X      / PlayStation Square   / Nintendo Y
    North = 4,   // Xbox Y      / PlayStation Triangle / Nintendo X

    Back = 5,    // Xbox View   / PlayStation Create   / Steam Deck View
    Guide = 6,   // Xbox button / PS button            / Steam button
    Start = 7,   // Xbox Menu   / PlayStation Options  / Steam Deck Menu

    LeftStick = 8,   // stick click, not stick motion
    RightStick = 9,

    LeftShoulder = 10,   // L1 / LB
    RightShoulder = 11,  // R1 / RB

    DpadUp = 12,
    DpadDown = 13,
    DpadLeft = 14,
    DpadRight = 15,

    // Xbox Share, PlayStation microphone, Nintendo Capture; the Steam
    // Deck has no equivalent. Listed because it is the most interesting
    // button on the pad for this application: almost nothing in any game
    // binds it, and on a DualSense it is physically labelled as a
    // microphone key, which makes it the obvious default for
    // push-to-talk. Nothing binds it yet — see GamepadBridge.h.
    Misc1 = 16,

    Touchpad = 17,  // PlayStation touchpad click

    // Back paddles. Present on Xbox Elite, Steam Deck (L4/L5/R4/R5) and
    // the 8BitDo pro pads. Worth exposing precisely because a handheld
    // user's thumbs are busy.
    LeftPaddle1 = 18,
    LeftPaddle2 = 19,
    RightPaddle1 = 20,
    RightPaddle2 = 21,
};
Q_ENUM_NS(GamepadButton)

enum class GamepadAxis {
    Unknown = 0,
    LeftX = 1,
    LeftY = 2,
    RightX = 3,
    RightY = 4,
    // Triggers are one-sided: 0 at rest, 1 fully pulled. They are axes
    // and not buttons because the analogue value is the point — a trigger
    // is the only control on the pad that can express "a bit".
    LeftTrigger = 5,
    RightTrigger = 6,
};
Q_ENUM_NS(GamepadAxis)

// Directional intent, after the deadzone and the repeat schedule have had
// their say. This is what a focus chain consumes: never a raw axis value.
enum class NavDirection {
    None = 0,
    Up = 1,
    Down = 2,
    Left = 3,
    Right = 4,
};
Q_ENUM_NS(NavDirection)

// ---------------------------------------------------------------------
// Input: what the platform told us happened.
// ---------------------------------------------------------------------
//
// GamepadBridge fills these in from SDL events; the tests fill them in by
// hand. That is the entire test seam for this layer, and it is why the
// deadzone maths, the repeat cadence and the hotplug bookkeeping are all
// exercised on a CI box with no controller plugged into it.
struct GamepadEvent {
    enum class Kind {
        Added,
        Removed,
        ButtonDown,
        ButtonUp,
        AxisMotion,
    };

    Kind kind = Kind::Added;

    // SDL's joystick instance id. Unique per physical connection for the
    // life of the process: unplugging and replugging the same pad yields
    // a NEW id, which is what makes it safe to use as a map key and
    // unsafe to use as a stable identity for, say, a saved binding.
    quint32 instanceId = 0;

    // Added only.
    QString name;

    // ButtonDown / ButtonUp only.
    GamepadButton button = GamepadButton::Unknown;

    // AxisMotion only.
    GamepadAxis axis = GamepadAxis::Unknown;
    // SDL's raw range: -32768..32767 for sticks, 0..32767 for triggers.
    // Kept raw rather than pre-normalised so the deadzone constants can
    // be written in the same units the hardware and every published
    // reference deadzone (XInput's in particular) use.
    qint16 raw = 0;
};

// ---------------------------------------------------------------------
// Output: what the UI should do about it.
// ---------------------------------------------------------------------
//
// Returned as values rather than emitted as side effects, for the same
// reason voice/AudioDevicePolicy.h returns a DeviceDecision: it is the
// only shape that lets "what happens when a pad is unplugged while a
// button is held" be a unit test instead of a hardware session.
struct GamepadAction {
    enum class Kind {
        // The set of connected pads went from empty to non-empty.
        Connected,
        // ...and back to empty. NOT emitted when one of several pads
        // goes away; see GamepadRegistry.h.
        Disconnected,
        // The primary pad changed identity (its name is now different),
        // without the connected/disconnected state changing.
        PrimaryChanged,

        ButtonPressed,
        ButtonReleased,

        // One directional step for a focus chain. `repeat` is false for
        // the step produced by the initial press/push and true for each
        // auto-repeat step after it, so a consumer that wants to treat a
        // held direction differently (page-scroll, say) can.
        Navigate,

        // A normalised analogue value changed by more than the report
        // epsilon. `value` is -1..1 for sticks and 0..1 for triggers.
        AxisChanged,
    };

    Kind kind = Kind::Connected;
    GamepadButton button = GamepadButton::Unknown;
    GamepadAxis axis = GamepadAxis::Unknown;
    NavDirection direction = NavDirection::None;
    double value = 0.0;
    bool repeat = false;
};

} // namespace bsfchat::input
