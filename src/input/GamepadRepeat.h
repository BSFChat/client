#pragma once

// Auto-repeat for held directional input, as a clock-injected state
// machine.
//
// Why this is not "just a QTimer": the thing being repeated has to
// survive the direction changing under it, the pad being unplugged
// mid-repeat, and the GUI thread stalling for longer than the repeat
// interval. A QTimer handles none of those and all three are reachable
// in normal use — the third one especially, because moving a window on
// Windows or reloading QML both block the loop for hundreds of
// milliseconds.
//
// The clock is a parameter (monotonic milliseconds) rather than read
// from inside, which is the whole reason the cadence is testable: the
// tests below step time by hand, so "does it fire 11 times a second"
// is an assertion rather than a stopwatch.

#include <algorithm>

#include "input/GamepadTypes.h"

namespace bsfchat::input {

// Delay between the initial step and the first repeat.
//
// 400 ms, sitting inside the range every desktop platform picked for key
// repeat for the same reason: macOS's default initial delay is ~500 ms,
// Windows' is ~500 ms, GNOME's is 500 ms, and the shortest any of them
// offers is ~250 ms. Below about 300 ms a deliberate single nudge of the
// stick reliably produces two steps, which is the single most annoying
// failure a focus chain can have — it makes the UI feel like it is
// ignoring precision rather than merely being fast.
//
// Toward the slow end of that range rather than the middle because
// unlike a keyboard, a stick cannot be released instantly: it springs
// back through the release threshold, so the effective hold is always a
// little longer than the user intended.
inline constexpr int kRepeatDelayMs = 400;

// Interval between repeats once repeating. ~11 steps/second.
//
// For comparison: Windows' fastest key repeat is ~31/s, which in a list
// is uncontrollable — by the time the eye registers the moving highlight
// it has gone six items too far. Hand-measured comfortable list
// scrolling is 8-15/s, and 90 ms is the middle of that.
//
// 90 also divides acceptably by the 8 ms active poll cadence: the worst
// quantisation error is one poll, so the realised interval is 90-98 ms,
// under 10% jitter. A value like 85 would quantise to 88 some steps and
// 96 others, which is audible as unevenness even though it is smaller.
inline constexpr int kRepeatIntervalMs = 90;

// The most repeat steps one poll may deliver when the poll was late.
//
// stepsDue() returns a COUNT, not a bool, because dropping steps would
// make the realised repeat rate depend on scheduler luck rather than on
// kRepeatIntervalMs — a loaded machine would scroll slower, which is the
// opposite of what a user expects when the UI is already struggling.
//
// But unbounded catch-up is worse than dropping: a two-second GUI-thread
// stall would come back owing 22 steps and fling the focus to the end of
// the list in one frame, with no visible movement to explain it. Three
// is the compromise — enough that ordinary jitter (one or two missed
// polls) is invisible, few enough that a real stall produces a jump the
// user can see and recover from.
//
// When the clamp bites, the schedule is re-based to now rather than
// advanced by three intervals. Advancing would leave the machine still
// owing steps and it would keep firing the clamped maximum every poll
// until it caught up, which turns one stall into a sustained runaway.
inline constexpr int kMaxCatchUpSteps = 3;

// A held direction and its repeat schedule.
//
// Not a QObject and not copy-restricted: it is a value, and GamepadCore
// holds one. All times are monotonic milliseconds from the caller's
// clock; the class never reads a clock itself.
class GamepadRepeat {
public:
    // Set the direction currently being held.
    //
    // Returns true when the caller should produce an IMMEDIATE
    // navigation step — that is, when the direction changed to something
    // other than None. Re-asserting the direction already held returns
    // false and leaves the schedule untouched, so a stream of axis
    // events that all resolve to the same direction does not restart the
    // delay forever and thereby suppress repeat entirely. (That bug is
    // easy to write and hard to see: the stick keeps producing motion
    // events as the thumb trembles, so the repeat never arrives, and it
    // looks like the repeat feature simply does not work.)
    bool setDirection(NavDirection direction, qint64 nowMs)
    {
        if (direction == m_direction) return false;
        m_direction = direction;
        if (direction == NavDirection::None) {
            m_nextStepMs = 0;
            return false;
        }
        // The first repeat is due one DELAY from now; subsequent ones one
        // INTERVAL apart. Storing the absolute deadline rather than a
        // countdown is what makes a late poll recoverable at all.
        m_nextStepMs = nowMs + kRepeatDelayMs;
        return true;
    }

    // How many repeat steps are due at `nowMs`, advancing the schedule
    // past them. 0 when nothing is held, when the delay has not yet
    // elapsed, or when no interval has passed since the last step.
    int stepsDue(qint64 nowMs)
    {
        if (m_direction == NavDirection::None) return 0;
        if (m_nextStepMs == 0) return 0;
        if (nowMs < m_nextStepMs) return 0;

        // +1 because reaching the deadline exactly is one step, not zero.
        const qint64 overdue = nowMs - m_nextStepMs;
        const qint64 steps = 1 + overdue / kRepeatIntervalMs;

        if (steps > kMaxCatchUpSteps) {
            // Clamped: re-base so the next step is a full interval away.
            // See kMaxCatchUpSteps for why this is not an advance.
            m_nextStepMs = nowMs + kRepeatIntervalMs;
            return kMaxCatchUpSteps;
        }
        m_nextStepMs += steps * kRepeatIntervalMs;
        return int(steps);
    }

    NavDirection direction() const { return m_direction; }
    bool holding() const { return m_direction != NavDirection::None; }

    // Drop the held direction without producing anything. Used when the
    // pad carrying it disappears: the user did not release the stick, so
    // there is no release to report, but continuing to repeat a direction
    // from a controller that is no longer attached is worse.
    void reset()
    {
        m_direction = NavDirection::None;
        m_nextStepMs = 0;
    }

    // Exposed for the poll-cadence decision: how long until the next
    // scheduled step, or -1 when nothing is scheduled.
    qint64 msUntilNextStep(qint64 nowMs) const
    {
        if (m_direction == NavDirection::None || m_nextStepMs == 0) return -1;
        return std::max<qint64>(0, m_nextStepMs - nowMs);
    }

private:
    NavDirection m_direction = NavDirection::None;
    // Absolute deadline, 0 meaning "none scheduled".
    qint64 m_nextStepMs = 0;
};

} // namespace bsfchat::input
