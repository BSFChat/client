#include "input/GamepadCore.h"

#include <cmath>

namespace bsfchat::input {

namespace {

// Which controls drive focus navigation.
//
// The LEFT stick and the d-pad, never the right stick. The right stick
// is left free deliberately: it is the only control on the pad with
// enough resolution to drive a scroll velocity, and a message list is
// the thing in this application most worth scrolling. Binding it to
// focus steps as well would make it unavailable for that later, for no
// gain — a user with both sticks bound to the same thing has one stick.
bool isNavStick(GamepadAxis axis)
{
    return axis == GamepadAxis::LeftX || axis == GamepadAxis::LeftY;
}

bool isDpad(GamepadButton button)
{
    return button == GamepadButton::DpadUp || button == GamepadButton::DpadDown
        || button == GamepadButton::DpadLeft
        || button == GamepadButton::DpadRight;
}

} // namespace

double GamepadCore::axisValue(GamepadAxis axis) const
{
    double best = 0.0;
    for (const quint32 id : m_registry.ids()) {
        const double v = normaliseAxis(axis, m_registry.rawAxis(id, axis));
        if (std::abs(v) > std::abs(best)) best = v;
    }
    return best;
}

void GamepadCore::reportAxisChanges(QList<GamepadAction>& out)
{
    // Index 0 is GamepadAxis::Unknown and is never reported.
    for (int i = 1; i < kAxisCount; ++i) {
        const GamepadAxis axis = GamepadAxis(i);
        const double now = axisValue(axis);
        const double was = m_reported[i];
        // Returning to (or leaving) exactly centre is always reported,
        // however small the step. "The stick is back at rest" is the one
        // edge a consumer cannot afford to miss — anything driven by the
        // value would otherwise be left permanently slightly deflected.
        const bool zeroEdge = (now == 0.0) != (was == 0.0);
        if (!zeroEdge && std::abs(now - was) < kAxisReportEpsilon) continue;

        m_reported[i] = now;
        GamepadAction a;
        a.kind = GamepadAction::Kind::AxisChanged;
        a.axis = axis;
        a.value = now;
        out.append(a);
    }
}

void GamepadCore::refreshNavigation(QList<GamepadAction>& out, qint64 nowMs)
{
    const NavDirection engaged = m_repeat.direction();
    NavDirection wanted = NavDirection::None;

    // Connection order, first non-None wins, d-pad before stick on each
    // pad. The d-pad outranks the stick because it is unambiguous: a
    // user pressing a d-pad direction while a thumb rests on the stick
    // meant the d-pad.
    //
    // Across pads this is arbitrary but it is DEFINED, which is what
    // matters — the alternative is a direction that depends on hash
    // ordering and therefore changes between runs.
    for (const quint32 id : m_registry.ids()) {
        if (wanted != NavDirection::None) break;
        for (const GamepadButton b : {GamepadButton::DpadUp,
                                      GamepadButton::DpadDown,
                                      GamepadButton::DpadLeft,
                                      GamepadButton::DpadRight}) {
            if (m_registry.isHeld(id, b)) {
                wanted = dpadDirection(b);
                break;
            }
        }
        if (wanted != NavDirection::None) break;

        wanted = navDirection(
            normaliseAxis(GamepadAxis::LeftX,
                          m_registry.rawAxis(id, GamepadAxis::LeftX)),
            normaliseAxis(GamepadAxis::LeftY,
                          m_registry.rawAxis(id, GamepadAxis::LeftY)),
            engaged);
    }

    if (m_repeat.setDirection(wanted, nowMs)) {
        GamepadAction a;
        a.kind = GamepadAction::Kind::Navigate;
        a.direction = wanted;
        a.repeat = false;
        out.append(a);
    }
}

QList<GamepadAction> GamepadCore::handle(const GamepadEvent& event,
                                         qint64 nowMs)
{
    QList<GamepadAction> out;

    switch (event.kind) {
    case GamepadEvent::Kind::Added: {
        const bool wasEmpty = !m_registry.anyConnected();
        const QString wasPrimary = m_registry.primaryName();
        if (!m_registry.add(event.instanceId, event.name)) break;
        if (wasEmpty) {
            GamepadAction a;
            a.kind = GamepadAction::Kind::Connected;
            out.append(a);
        } else if (m_registry.primaryName() != wasPrimary) {
            // Should not happen — adding never changes the oldest pad —
            // but reported rather than assumed, so that if the primary
            // rule is ever changed the UI still follows it.
            GamepadAction a;
            a.kind = GamepadAction::Kind::PrimaryChanged;
            out.append(a);
        }
        break;
    }

    case GamepadEvent::Kind::Removed: {
        const GamepadRegistry::Removal r = m_registry.remove(event.instanceId);
        if (!r.known) break;

        // Synthesised releases FIRST, before the connection state
        // changes. A consumer handling ButtonReleased wants to do so
        // while the world still looks like a controller was attached —
        // and a push-to-talk consumer in particular must see the release
        // at all, because SDL will never send one.
        for (const GamepadButton b : r.releasedButtons) {
            // Only when no other attached pad is still holding it; the
            // button is a logical OR across pads.
            if (m_registry.holdersOf(b) > 0) continue;
            GamepadAction a;
            a.kind = GamepadAction::Kind::ButtonReleased;
            a.button = b;
            out.append(a);
        }

        // Any direction this pad was holding dies with it. Dropped
        // rather than released-and-reported: the user did not let go, so
        // there is no gesture to report, but repeating a direction from
        // a controller that is no longer attached would be a focus ring
        // walking a list on its own.
        //
        // refreshNavigation recomputes from the pads that REMAIN, so a
        // direction held only by the departing pad resolves to None and
        // the schedule clears itself. No explicit reset is needed, and
        // adding one would be wrong: another attached pad may legitimately
        // still be holding the same direction.
        refreshNavigation(out, nowMs);

        // Zeroed axes are reported through the normal epsilon path,
        // which will see the aggregated value fall to 0 now the device
        // is gone. r.zeroedAxes is therefore not iterated directly —
        // doing so would double-report when a second pad holds the same
        // axis deflected.
        reportAxisChanges(out);

        if (r.nowEmpty) {
            GamepadAction a;
            a.kind = GamepadAction::Kind::Disconnected;
            out.append(a);
        } else if (r.wasPrimary) {
            GamepadAction a;
            a.kind = GamepadAction::Kind::PrimaryChanged;
            out.append(a);
        }
        break;
    }

    case GamepadEvent::Kind::ButtonDown:
    case GamepadEvent::Kind::ButtonUp: {
        const bool down = (event.kind == GamepadEvent::Kind::ButtonDown);
        const int before = m_registry.holdersOf(event.button);
        if (!m_registry.setButton(event.instanceId, event.button, down)) break;
        const int after = m_registry.holdersOf(event.button);

        // Edge of the logical OR across pads, not of this pad's state.
        if (before == 0 && after > 0) {
            GamepadAction a;
            a.kind = GamepadAction::Kind::ButtonPressed;
            a.button = event.button;
            out.append(a);
        } else if (before > 0 && after == 0) {
            GamepadAction a;
            a.kind = GamepadAction::Kind::ButtonReleased;
            a.button = event.button;
            out.append(a);
        }

        if (isDpad(event.button)) refreshNavigation(out, nowMs);
        break;
    }

    case GamepadEvent::Kind::AxisMotion: {
        if (!m_registry.setAxis(event.instanceId, event.axis, event.raw)) break;
        reportAxisChanges(out);
        if (isNavStick(event.axis)) refreshNavigation(out, nowMs);
        break;
    }
    }

    if (!out.isEmpty()) {
        m_lastActionMs = nowMs;
        m_haveLastAction = true;
    }
    return out;
}

QList<GamepadAction> GamepadCore::tick(qint64 nowMs)
{
    QList<GamepadAction> out;
    const int steps = m_repeat.stepsDue(nowMs);
    for (int i = 0; i < steps; ++i) {
        GamepadAction a;
        a.kind = GamepadAction::Kind::Navigate;
        a.direction = m_repeat.direction();
        a.repeat = true;
        out.append(a);
    }
    if (!out.isEmpty()) {
        m_lastActionMs = nowMs;
        m_haveLastAction = true;
    }
    return out;
}

int GamepadCore::pollIntervalMs(qint64 nowMs) const
{
    // Elapsed time is measured from the last ACTION, not the last event.
    //
    // A resting stick emits a steady trickle of SDL_EVENT_GAMEPAD_AXIS_MOTION
    // as it trembles inside the deadzone. Those events produce no
    // actions, because normaliseAxis() flattens them to 0.0 — so
    // measuring from the last event would reset the doze countdown
    // forever and the cadence would never leave 8 ms while a pad was
    // attached. Which is precisely the battery cost this is here to
    // avoid, arrived at by a route that looks correct.
    const qint64 since = m_haveLastAction
        ? (nowMs - m_lastActionMs)
        : kPollDozeAfterMs;  // never had input: doze immediately
    // Qualified: the member function of the same name hides the free one.
    return bsfchat::input::pollIntervalMs(
        m_registry.anyConnected(), m_registry.anyInputHeld(), since);
}

} // namespace bsfchat::input
