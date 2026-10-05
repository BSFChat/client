#pragma once

// The whole decision half of the gamepad input layer, composed and
// SDL-free.
//
// Events in (GamepadEvent), actions out (GamepadAction), a clock passed
// as a parameter, and nothing else. GamepadBridge is a thin shell that
// translates SDL events into the former, calls these two methods, and
// emits Qt signals for the latter — so everything interesting about this
// feature is reachable from a test binary that neither links SDL nor has
// a controller plugged in.
//
// That split is not tidiness. The cases that actually break a gamepad
// layer are the ones that need a device AND a stopwatch AND an
// adversarial hand: a pad unplugged with push-to-talk held, two pads
// holding the same button, a stick released diagonally, a poll that
// arrived 600 ms late. None of those can be tested by hand twice, and
// one of them leaves a microphone open.
//
// USAGE, once per poll:
//
//     for (each pending platform event)
//         actions += core.handle(event, nowMs);
//     actions += core.tick(nowMs);          // auto-repeat steps
//     timer.setInterval(core.pollIntervalMs(nowMs));
//
// tick() must be called even when no events arrived — that is where
// auto-repeat comes from.

#include <QList>
#include <QString>

#include "input/GamepadDeadzone.h"
#include "input/GamepadPollCadence.h"
#include "input/GamepadRegistry.h"
#include "input/GamepadRepeat.h"
#include "input/GamepadTypes.h"

namespace bsfchat::input {

class GamepadCore {
public:
    // Feed one platform event. Returns everything it produced, in order.
    QList<GamepadAction> handle(const GamepadEvent& event, qint64 nowMs);

    // Produce any auto-repeat navigation steps due at `nowMs`. Call once
    // per poll, after handling events.
    QList<GamepadAction> tick(qint64 nowMs);

    // How long the pump timer should wait before the next poll.
    int pollIntervalMs(qint64 nowMs) const;

    bool connected() const { return m_registry.anyConnected(); }
    int controllerCount() const { return m_registry.count(); }
    QString primaryName() const { return m_registry.primaryName(); }

    // Normalised value for an axis, aggregated across every attached pad
    // (the largest magnitude wins, sign preserved). Exposed for a
    // poll-style read from a QML binding; the AxisChanged action is the
    // push-style equivalent and they always agree.
    double axisValue(GamepadAxis axis) const;

    // The direction currently driving navigation, if any. Intended for
    // tests and diagnostics rather than for driving UI — a consumer that
    // reads this instead of handling Navigate actions has reimplemented
    // the repeat cadence badly.
    NavDirection heldDirection() const { return m_repeat.direction(); }

    // Whether a button is held on any attached pad. The logical OR that
    // the ButtonPressed/ButtonReleased actions are edges of.
    bool buttonHeld(GamepadButton button) const
    {
        return m_registry.holdersOf(button) > 0;
    }

private:
    // Recompute the navigation direction from current device state and
    // fold it into the repeat schedule, appending an immediate Navigate
    // action if the direction changed to something real.
    void refreshNavigation(QList<GamepadAction>& out, qint64 nowMs);

    // Append AxisChanged actions for any axis whose aggregated value has
    // moved far enough since it was last reported.
    void reportAxisChanges(QList<GamepadAction>& out);

    GamepadRegistry m_registry;
    GamepadRepeat m_repeat;

    // Last aggregated value handed out per axis, so the report epsilon
    // is measured against what the consumer last saw rather than against
    // the previous raw reading. Measuring against the previous reading
    // would let a slow drift cross any distance in sub-epsilon steps
    // without ever reporting.
    std::array<double, kAxisCount> m_reported{};

    // When input last did something worth reporting. See the comment in
    // pollIntervalMs() for why this tracks ACTIONS and not events.
    qint64 m_lastActionMs = 0;
    bool m_haveLastAction = false;
};

} // namespace bsfchat::input
