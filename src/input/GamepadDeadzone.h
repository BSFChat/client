#pragma once

// Turning noisy 16-bit stick and trigger readings into something a user
// interface can be driven by, as pure arithmetic.
//
// Three separate jobs live here and they want three different thresholds.
// Collapsing them into one "deadzone" number is the usual mistake and it
// has a characteristic symptom each way: too low and a worn pad walks the
// focus ring on its own, too high and the analogue value is useless
// because the bottom quarter of the stick's travel reads as nothing.
//
//   1. REPORTING (axisValue): what the analogue value is. Wants the
//      smallest threshold that silences a resting stick, and wants the
//      remaining travel rescaled so there is no step at the boundary.
//
//   2. NAVIGATION (navDirection): which way, if any, the user is pushing
//      with intent. Wants a much higher threshold than reporting, plus
//      hysteresis, plus a rule for diagonals.
//
//   3. TRIGGER ACTUATION: a trigger used as a button. Its own threshold,
//      because triggers rest at a hard mechanical zero and are therefore
//      far quieter than sticks.
//
// Raw units throughout: SDL reports -32768..32767 for sticks and
// 0..32767 for triggers. The constants are written in those units
// because that is the unit every published reference deadzone is quoted
// in, and converting them to fractions here would mean the next person
// cannot check them against Microsoft's header.

#include <algorithm>
#include <cmath>

#include "input/GamepadTypes.h"

namespace bsfchat::input {

// SDL's own full-scale constant, restated so this header does not have
// to include SDL. Asserted against the real thing in GamepadBridge.cpp,
// so the two cannot drift apart silently.
inline constexpr int kAxisMax = 32767;

// ---- 1. Reporting ---------------------------------------------------

// Below this, an axis reads exactly 0.0.
//
// 7849 is XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE from Microsoft's XInput
// header, scaled to SDL's range (they share it: both are int16 axes).
// It is used rather than something smaller because it is the figure the
// whole industry's hardware tolerances are set against — a pad that
// rests inside it is considered healthy by the platform that defines the
// pad. Measured on worn pads, resting offsets of 5-8% of full scale are
// routine, and 7849 is 24%, which clears them with room to spare.
//
// The cost of this being generous is that the first 24% of stick travel
// is unavailable. That is the correct trade here: nothing in a chat
// client needs fine analogue control near centre, and the alternative —
// a per-device calibration pass — is a feature, not a constant.
inline constexpr int kStickDeadzone = 7849;

// Triggers get their own, much smaller, threshold. A trigger is sprung
// against a stop and reads a true 0 at rest, so it does not need the
// stick's noise margin. 3855 is XINPUT_GAMEPAD_TRIGGER_THRESHOLD (30 of
// 255) scaled to int16 — Microsoft's own "the trigger is now being
// pulled" line, and therefore the one a user's muscle memory from games
// is calibrated to.
inline constexpr int kTriggerDeadzone = 3855;

// Normalised values are only reported when they move at least this far.
//
// A stick held perfectly still still jitters by a few hundred raw units,
// and at the active poll cadence that would be ~125 signal emissions a
// second of pure noise crossing into QML. 0.02 is below the point where
// a change is visible in anything this value could plausibly drive, and
// comfortably above a healthy pad's noise floor (a few hundred raw units
// is ~0.01 after deadzone rescaling).
//
// Movement to or away from exactly 0.0 is always reported regardless, so
// "the stick returned to centre" can never be the edge that gets eaten.
inline constexpr double kAxisReportEpsilon = 0.02;

// Normalise one raw axis reading.
//
// RADIAL DEADZONE IS NOT APPLIED HERE and that is deliberate. A radial
// deadzone needs both components of a stick at once, and SDL delivers
// axis motion one axis at a time; doing it per-event means holding a
// shadow copy of the other axis and recomputing both whenever either
// moves, which is real complexity. It buys correctness for one case —
// a true diagonal where each component sits just under the threshold —
// and navDirection() below rejects true diagonals anyway. So: axis-wise
// for reporting, and the directional decision gets its own rule.
//
// The surviving range is rescaled to 0..1 so crossing the threshold does
// not jump the value from 0.0 to 0.24. Without the rescale, anything
// driven by this (a volume nudge, a scroll velocity) starts at a quarter
// of full deflection the instant it starts at all.
inline double normaliseAxis(GamepadAxis axis, int raw)
{
    const bool trigger = (axis == GamepadAxis::LeftTrigger
                          || axis == GamepadAxis::RightTrigger);
    const int dead = trigger ? kTriggerDeadzone : kStickDeadzone;

    // SDL's negative range is one unit wider than its positive range
    // (-32768..32767). Clamping the magnitude to kAxisMax rather than
    // dividing by 32768 means full deflection reads exactly 1.0 in both
    // directions, instead of 1.0 one way and 0.99997 the other — which
    // would make "is the stick fully over" an inequality that depends on
    // the sign.
    const int magnitude = std::min(std::abs(raw), kAxisMax);
    if (magnitude <= dead) return 0.0;

    const double scaled = double(magnitude - dead) / double(kAxisMax - dead);
    const double clamped = std::min(scaled, 1.0);
    return raw < 0 ? -clamped : clamped;
}

// Whether a trigger reading counts as "pulled" when a trigger is used as
// a button. Shares kTriggerDeadzone with reporting on purpose: a trigger
// that reads a non-zero analogue value but is not considered pressed
// would be a contradiction visible in the UI.
inline bool triggerActuated(int raw)
{
    return std::abs(raw) > kTriggerDeadzone;
}

// ---- 2. Navigation -------------------------------------------------

// A stick push must reach this fraction of full scale to start moving
// the focus, and must fall back below kNavRelease to stop.
//
// Hysteresis, not one threshold, because without a gap a stick resting
// anywhere near the line chatters: it crosses, disengages, crosses again,
// and the focus ring flickers between two items. The gap has to be wider
// than the hardware's noise amplitude plus the amount a thumb drifts
// while trying to hold still, which is a lot more than the noise.
//
// 0.55 for engage: a deliberate push lands above 0.8; a thumb resting on
// the stick without meaning to move sits below 0.4. 0.55 is between
// them and is well clear of the 0.24 reporting deadzone, so the engage
// decision is never made on a value that only just survived rescaling.
//
// 0.35 for release: far enough below engage (0.20 of full scale) to
// outlast drift, and still high enough that letting go of the stick
// disengages promptly rather than after the spring has fully settled.
inline constexpr double kNavEngage = 0.55;
inline constexpr double kNavRelease = 0.35;

// A diagonal push must be this much more one way than the other before
// it counts as a direction at all.
//
// The alternative — "whichever component is larger wins" — is actively
// bad on a 45-degree push: the two components cross each other
// repeatedly as the thumb moves, so the direction alternates between
// Right and Down several times a second and the focus ring scribbles.
// Requiring a 1.4x margin means a genuine diagonal produces NOTHING,
// which is the correct answer for a one-dimensional focus chain: the
// user has not expressed a direction, so none is invented.
//
// 1.4 is close to sqrt(2) (1.414), i.e. the margin is "you must be
// nearer one axis than you are to the diagonal".
inline constexpr double kNavDominance = 1.4;

// Which way a stick is being pushed, given both of its normalised
// components and whether a direction is currently engaged.
//
// `engaged` carries the hysteresis: pass the currently-held direction
// (NavDirection::None if none) and the thresholds swap from engage to
// release for that direction.
inline NavDirection navDirection(double x, double y, NavDirection engaged)
{
    const double ax = std::abs(x);
    const double ay = std::abs(y);

    // Dominance first, before either threshold. A diagonal is "no
    // direction" rather than "the direction it is closest to", and that
    // has to hold while disengaging too, or releasing a push diagonally
    // would stick on the old direction.
    const bool horizontal = ax > ay * kNavDominance;
    const bool vertical = ay > ax * kNavDominance;
    if (!horizontal && !vertical) return NavDirection::None;

    const double dominant = horizontal ? ax : ay;
    const NavDirection candidate = horizontal
        ? (x < 0 ? NavDirection::Left : NavDirection::Right)
        // SDL's Y axis points DOWN (positive is toward the user), which
        // matches screen coordinates and not maths coordinates. Pushing
        // the stick away from you is negative Y and means Up.
        : (y < 0 ? NavDirection::Up : NavDirection::Down);

    // Holding the same direction? Use the lower bar. Anything else —
    // including "the dominant axis flipped to the other axis" — has to
    // clear the higher one, so a sloppy release cannot hand the focus to
    // a direction the user never deliberately pushed.
    const double threshold =
        (candidate == engaged) ? kNavRelease : kNavEngage;
    return dominant >= threshold ? candidate : NavDirection::None;
}

// The d-pad's equivalent. No deadzone and no hysteresis: a d-pad is four
// switches, so there is nothing to threshold. It is a function rather
// than a switch at the call site only so that the "two opposite buttons
// held at once" case has one defined answer.
inline NavDirection dpadDirection(GamepadButton button)
{
    switch (button) {
    case GamepadButton::DpadUp:    return NavDirection::Up;
    case GamepadButton::DpadDown:  return NavDirection::Down;
    case GamepadButton::DpadLeft:  return NavDirection::Left;
    case GamepadButton::DpadRight: return NavDirection::Right;
    default:                       return NavDirection::None;
    }
}

} // namespace bsfchat::input
