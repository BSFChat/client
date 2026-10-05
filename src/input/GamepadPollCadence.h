#pragma once

// How often to pump SDL, as a pure function of what is attached and what
// the user is doing.
//
// This exists as its own testable decision because getting it wrong is
// invisible in development and expensive in the field, in both
// directions:
//
//   * Too slow and the layer adds input lag. Lag in a focus ring reads
//     as "the app is broken", not "the poll interval is 50 ms", and it
//     is the kind of complaint nobody can reproduce on request.
//
//   * Too fast, forever, and a handheld pays for it in battery. A fixed
//     8 ms timer is 125 wakeups a second for the whole life of the
//     process, including the overwhelming majority of it where no
//     controller is attached at all. On a Steam Deck, which is the
//     device this feature is mostly for, that is a real cost for no
//     benefit — and it is the kind of thing that gets a chat client
//     blamed for a handheld's battery life.
//
// The reason a timer is needed at all rather than a blocking wait, and
// the reason it can never stop entirely, are both in GamepadBridge.h.
// The short version of the second one: SDL only discovers a newly
// connected pad during a pump, so a layer that stopped polling when
// nothing was attached would never notice anything being attached.

#include <QtGlobal>

namespace bsfchat::input {

// Nothing attached. The only job of a poll at this cadence is to notice
// a hotplug, and 4 wakeups a second is negligible against everything
// else a Qt GUI process does while idle. A user plugging in a pad
// tolerates a quarter second before the UI acknowledges it — they are
// still moving their hand.
inline constexpr int kPollIdleMs = 250;

// Attached and being used. 8 ms is about one frame at 120 Hz, and under
// one frame at every refresh rate this app will meet. The input layer
// must not be the component that adds perceptible latency, and the
// budget for "imperceptible" in a UI response is roughly one frame.
//
// Not 1 ms: SDL's own backends sample HID at their own rate and nothing
// downstream of this can act faster than a frame anyway, so the extra
// 120 wakeups a second would buy nothing measurable.
inline constexpr int kPollActiveMs = 8;

// Attached, but nothing held and nothing moved recently. 20 Hz.
//
// The worst case this costs is the latency of the FIRST input after
// idling — up to 50 ms — because the poll that notices it also switches
// the cadence back to active, so everything after it is at full speed.
// 50 ms on the first press of a session is at the edge of perceptible
// and is the correct thing to trade; 50 ms on every press would not be.
inline constexpr int kPollDozeMs = 50;

// How long with no input before dozing.
//
// Long enough that it never bites during use: 2 seconds of a completely
// still stick and no buttons is someone who has stopped, not someone
// pausing between presses. Short enough that a pad left attached
// overnight is not held at the active cadence.
inline constexpr qint64 kPollDozeAfterMs = 2000;

// The cadence to run the pump timer at.
//
// `anyInputHeld` has to be a separate input from `msSinceLastEvent` and
// cannot be inferred from it, because a held control produces no events
// while it is held. Push-to-talk is the case that makes this mandatory:
// the button goes down, events stop, and if the cadence then decayed on
// elapsed time alone the mic would stay open for up to a doze interval
// after the user let go. The release edge is the one edge in this entire
// layer that must not be late.
inline int pollIntervalMs(bool anyConnected, bool anyInputHeld,
                          qint64 msSinceLastEvent)
{
    if (!anyConnected) return kPollIdleMs;
    if (anyInputHeld) return kPollActiveMs;
    return msSinceLastEvent >= kPollDozeAfterMs ? kPollDozeMs : kPollActiveMs;
}

} // namespace bsfchat::input
