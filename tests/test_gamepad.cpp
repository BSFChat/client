// Unit tests for the native gamepad input layer.
//
// WHAT IS AND IS NOT PROVEN HERE
//
// Everything below drives GamepadCore through injected GamepadEvents and
// a hand-stepped clock. No SDL, no controller, no event loop, no device
// of any kind — which is the whole reason the layer is split the way it
// is. The cases that actually break gamepad support are the ones that
// need a device AND a stopwatch AND an adversarial hand at once:
//
//   * a pad unplugged with push-to-talk still held down,
//   * two pads holding the same button,
//   * a stick released diagonally,
//   * a poll that arrived 600 ms late because the GUI thread stalled.
//
// None of those can be tested by hand twice, and the first one leaves a
// microphone open with no controller left to close it with.
//
// What these tests CANNOT prove, and what therefore still needs a real
// controller in the owner's hands:
//
//   1. That SDL's mapping database actually maps his pads the way the
//      semantic enum claims — that South really is the bottom face
//      button on his hardware.
//   2. That SDL delivers hotplug events at all on each platform. The
//      bookkeeping for "a pad went away" is proven; that SDL says so is
//      not.
//   3. That the chosen deadzone clears HIS pads' resting drift. 7849 is
//      Microsoft's figure and clears a healthy pad by a wide margin, but
//      a badly worn stick could sit outside it.
//   4. The realised end-to-end latency of the poll cadence.

#include <QtTest>

#include "input/GamepadCore.h"
#include "input/GamepadDeadzone.h"
#include "input/GamepadPollCadence.h"
#include "input/GamepadRegistry.h"
#include "input/GamepadRepeat.h"

using namespace bsfchat::input;

namespace {

GamepadEvent added(quint32 id, const char* name)
{
    GamepadEvent e;
    e.kind = GamepadEvent::Kind::Added;
    e.instanceId = id;
    e.name = QString::fromLatin1(name);
    return e;
}

GamepadEvent removed(quint32 id)
{
    GamepadEvent e;
    e.kind = GamepadEvent::Kind::Removed;
    e.instanceId = id;
    return e;
}

GamepadEvent button(quint32 id, GamepadButton b, bool down)
{
    GamepadEvent e;
    e.kind = down ? GamepadEvent::Kind::ButtonDown
                  : GamepadEvent::Kind::ButtonUp;
    e.instanceId = id;
    e.button = b;
    return e;
}

GamepadEvent axis(quint32 id, GamepadAxis a, int raw)
{
    GamepadEvent e;
    e.kind = GamepadEvent::Kind::AxisMotion;
    e.instanceId = id;
    e.axis = a;
    e.raw = qint16(raw);
    return e;
}

// Full deflection, and a push comfortably past kNavEngage (0.55 of full
// scale after deadzone rescaling).
constexpr int kFull = 32767;
constexpr int kPush = 28000;

int countOf(const QList<GamepadAction>& as, GamepadAction::Kind k)
{
    int n = 0;
    for (const auto& a : as)
        if (a.kind == k) ++n;
    return n;
}

QList<GamepadAction> only(const QList<GamepadAction>& as,
                          GamepadAction::Kind k)
{
    QList<GamepadAction> out;
    for (const auto& a : as)
        if (a.kind == k) out.append(a);
    return out;
}

} // namespace

class GamepadTest : public QObject {
    Q_OBJECT

private slots:

    // ================= deadzone: reporting =================

    // A stick resting anywhere inside the deadzone reads exactly zero,
    // not "nearly zero". The regression this pins is phantom input on a
    // worn pad: anything that reports 0.03 while untouched will, given a
    // consumer that integrates it, drift on its own forever.
    void restingStickReadsExactlyZero()
    {
        QCOMPARE(normaliseAxis(GamepadAxis::LeftX, 0), 0.0);
        QCOMPARE(normaliseAxis(GamepadAxis::LeftX, kStickDeadzone), 0.0);
        QCOMPARE(normaliseAxis(GamepadAxis::LeftX, -kStickDeadzone), 0.0);
        QCOMPARE(normaliseAxis(GamepadAxis::LeftX, kStickDeadzone - 1), 0.0);
        // One unit past the threshold is non-zero but tiny — i.e. the
        // rescale starts from 0, it does not start from the threshold.
        const double justOver =
            normaliseAxis(GamepadAxis::LeftX, kStickDeadzone + 1);
        QVERIFY(justOver > 0.0);
        QVERIFY(justOver < 0.001);
    }

    // Full deflection is exactly 1.0 in BOTH directions. SDL's negative
    // range is one unit wider than its positive range, so a naive
    // divide-by-32768 gives 1.0 one way and 0.99997 the other, and any
    // "is the stick fully over" comparison then depends on the sign.
    void fullDeflectionIsOneBothWays()
    {
        QCOMPARE(normaliseAxis(GamepadAxis::LeftX, kFull), 1.0);
        QCOMPARE(normaliseAxis(GamepadAxis::LeftX, -kFull), -1.0);
        // -32768 exists on the wire and must clamp, not overshoot.
        QCOMPARE(normaliseAxis(GamepadAxis::LeftX, -32768), -1.0);
    }

    // Triggers get a much smaller deadzone than sticks, because they
    // rest against a mechanical stop and are used for their analogue
    // value. Sharing the stick's 24% would throw away the bottom quarter
    // of a trigger pull.
    void triggersUseTheirOwnSmallerDeadzone()
    {
        QVERIFY(kTriggerDeadzone < kStickDeadzone);
        // A reading a stick would call zero is a real trigger pull.
        const int mid = (kTriggerDeadzone + kStickDeadzone) / 2;
        QCOMPARE(normaliseAxis(GamepadAxis::LeftX, mid), 0.0);
        QVERIFY(normaliseAxis(GamepadAxis::LeftTrigger, mid) > 0.0);
        QVERIFY(triggerActuated(mid));
        QVERIFY(!triggerActuated(kTriggerDeadzone));
    }

    // ================= deadzone: navigation =================

    // Hysteresis: engage high, release low. Without the gap a stick
    // resting near one threshold chatters and the focus ring flickers
    // between two items.
    void navigationEngagesHighAndReleasesLow()
    {
        QVERIFY(kNavRelease < kNavEngage);

        // Just under engage, nothing held -> no direction.
        const double belowEngage = kNavEngage - 0.05;
        QCOMPARE(navDirection(0.0, -belowEngage, NavDirection::None),
                 NavDirection::None);

        // Same value while Up is already engaged -> still Up, because
        // it is above the RELEASE threshold.
        QCOMPARE(navDirection(0.0, -belowEngage, NavDirection::Up),
                 NavDirection::Up);

        // Below release, even while engaged -> gone.
        QCOMPARE(navDirection(0.0, -(kNavRelease - 0.05), NavDirection::Up),
                 NavDirection::None);
    }

    // A true diagonal is NOT the nearest direction, it is no direction.
    // "Larger component wins" makes a 45-degree push alternate between
    // two directions several times a second as the thumb moves.
    void trueDiagonalProducesNoDirection()
    {
        QCOMPARE(navDirection(0.8, 0.8, NavDirection::None),
                 NavDirection::None);
        QCOMPARE(navDirection(-0.9, 0.9, NavDirection::None),
                 NavDirection::None);

        // NEAR-diagonal, not an exact tie, and this is the case that
        // actually pins the dominance margin. An exact tie returns None
        // for ANY margin including 1.0 — both strict comparisons fail —
        // so a test built only on ties passes with the margin removed,
        // which is precisely what it exists to catch. A thumb never
        // produces an exact tie; it produces this.
        QCOMPARE(navDirection(0.80, 0.75, NavDirection::None),
                 NavDirection::None);
        QCOMPARE(navDirection(0.75, 0.80, NavDirection::None),
                 NavDirection::None);
        QCOMPARE(navDirection(-0.70, 0.78, NavDirection::None),
                 NavDirection::None);
        // Just inside the margin is still nothing; just outside decides.
        QCOMPARE(navDirection(0.9, 0.9 / kNavDominance + 0.01,
                              NavDirection::None),
                 NavDirection::None);
        QCOMPARE(navDirection(0.9, 0.9 / kNavDominance - 0.01,
                              NavDirection::None),
                 NavDirection::Right);
        // And it does not stick while engaged either, or releasing
        // diagonally would hold the old direction.
        QCOMPARE(navDirection(0.8, 0.8, NavDirection::Right),
                 NavDirection::None);

        // Clear of the diagonal by the dominance margin -> decided.
        QCOMPARE(navDirection(0.9, 0.1, NavDirection::None),
                 NavDirection::Right);
        QCOMPARE(navDirection(0.1, 0.9, NavDirection::None),
                 NavDirection::Down);
    }

    // SDL's Y axis points DOWN (screen coordinates, not maths). Pushing
    // the stick away from you is negative and means Up. Getting this
    // backwards inverts every list in the application.
    void stickYAxisPointsDown()
    {
        QCOMPARE(navDirection(0.0, -0.9, NavDirection::None),
                 NavDirection::Up);
        QCOMPARE(navDirection(0.0, 0.9, NavDirection::None),
                 NavDirection::Down);
    }

    // ================= repeat cadence =================

    // The initial press steps once, then nothing until the delay has
    // elapsed, then one step per interval.
    void repeatWaitsTheDelayThenRunsAtTheInterval()
    {
        GamepadRepeat r;
        QVERIFY(r.setDirection(NavDirection::Down, 1000));

        // Nothing at all before the delay is up.
        QCOMPARE(r.stepsDue(1000 + kRepeatDelayMs - 1), 0);
        // Exactly at the deadline is one step, not zero.
        QCOMPARE(r.stepsDue(1000 + kRepeatDelayMs), 1);
        // Then one per interval, and nothing in between.
        QCOMPARE(r.stepsDue(1000 + kRepeatDelayMs + kRepeatIntervalMs - 1), 0);
        QCOMPARE(r.stepsDue(1000 + kRepeatDelayMs + kRepeatIntervalMs), 1);
        QCOMPARE(r.stepsDue(1000 + kRepeatDelayMs + 2 * kRepeatIntervalMs), 1);
    }

    // Re-asserting the direction already held must NOT restart the
    // delay. A trembling thumb produces a stream of axis events that all
    // resolve to the same direction; restarting on each would suppress
    // auto-repeat entirely, and the symptom is "the repeat feature does
    // not work" rather than anything that points at this.
    void reassertingTheSameDirectionDoesNotRestartTheDelay()
    {
        GamepadRepeat r;
        QVERIFY(r.setDirection(NavDirection::Down, 0));
        // Hammer the same direction right up to the deadline.
        for (qint64 t = 0; t < kRepeatDelayMs; t += 10)
            QVERIFY(!r.setDirection(NavDirection::Down, t));
        // The repeat still arrives on the original schedule.
        QCOMPARE(r.stepsDue(kRepeatDelayMs), 1);
    }

    // Changing direction restarts the delay, so a deliberate flick the
    // other way is one step and not an immediate repeat.
    void changingDirectionRestartsTheDelay()
    {
        GamepadRepeat r;
        QVERIFY(r.setDirection(NavDirection::Down, 0));
        QCOMPARE(r.stepsDue(kRepeatDelayMs), 1);
        QVERIFY(r.setDirection(NavDirection::Up, kRepeatDelayMs));
        QCOMPARE(r.direction(), NavDirection::Up);
        QCOMPARE(r.stepsDue(kRepeatDelayMs + kRepeatIntervalMs), 0);
        QCOMPARE(r.stepsDue(kRepeatDelayMs + kRepeatDelayMs), 1);
    }

    // Releasing produces nothing and schedules nothing.
    void releasingStopsTheRepeat()
    {
        GamepadRepeat r;
        QVERIFY(r.setDirection(NavDirection::Left, 0));
        QVERIFY(!r.setDirection(NavDirection::None, 50));
        QVERIFY(!r.holding());
        QCOMPARE(r.stepsDue(100000), 0);
    }

    // A LATE poll owes several steps and delivers them, so the realised
    // repeat rate does not depend on scheduler luck — but the catch-up
    // is clamped, because a 2-second stall owing 22 steps would fling
    // the focus to the end of a list in one frame with nothing on screen
    // to explain it.
    void lateCatchUpIsDeliveredButClamped()
    {
        GamepadRepeat r;
        r.setDirection(NavDirection::Down, 0);

        // Two intervals late: both owed steps arrive.
        QCOMPARE(r.stepsDue(kRepeatDelayMs + kRepeatIntervalMs), 2);

        // Now stall for two seconds.
        GamepadRepeat s;
        s.setDirection(NavDirection::Down, 0);
        const int clamped = s.stepsDue(kRepeatDelayMs + 2000);
        QCOMPARE(clamped, kMaxCatchUpSteps);

        // And the clamp RE-BASES rather than advancing: the very next
        // poll owes nothing. Advancing by three intervals would leave
        // the machine still behind, firing the clamped maximum every
        // poll until it caught up — one stall becoming a sustained
        // runaway.
        QCOMPARE(s.stepsDue(kRepeatDelayMs + 2000 + 1), 0);
        QCOMPARE(s.stepsDue(kRepeatDelayMs + 2000 + kRepeatIntervalMs), 1);
    }

    // ================= hotplug bookkeeping =================

    void connectAndDisconnectReportOnce()
    {
        GamepadCore c;
        QVERIFY(!c.connected());

        auto as = c.handle(added(1, "Xbox Wireless Controller"), 0);
        QCOMPARE(countOf(as, GamepadAction::Kind::Connected), 1);
        QVERIFY(c.connected());
        QCOMPARE(c.primaryName(), QStringLiteral("Xbox Wireless Controller"));
        QCOMPARE(c.controllerCount(), 1);

        as = c.handle(removed(1), 10);
        QCOMPARE(countOf(as, GamepadAction::Kind::Disconnected), 1);
        QVERIFY(!c.connected());
        QCOMPARE(c.primaryName(), QString());
    }

    // SDL re-enumerates on some platforms, and Steam's virtual-gamepad
    // shim re-announces when a game grabs the pad. A second Added for a
    // live id must not double-count the device, and must not reset its
    // held state — that would drop a button still physically down.
    void duplicateAddIsIgnoredAndKeepsHeldState()
    {
        GamepadCore c;
        c.handle(added(1, "Pad"), 0);
        c.handle(button(1, GamepadButton::Misc1, true), 10);
        QVERIFY(c.buttonHeld(GamepadButton::Misc1));

        const auto as = c.handle(added(1, "Pad renamed"), 20);
        QVERIFY(as.isEmpty());
        QCOMPARE(c.controllerCount(), 1);
        QCOMPARE(c.primaryName(), QStringLiteral("Pad"));
        QVERIFY(c.buttonHeld(GamepadButton::Misc1));
    }

    // SDL does send removal events for joysticks it never mapped as
    // gamepads, so an unknown id is normal traffic, not an error — and
    // must not be reported as a disconnect.
    void removingAnUnknownDeviceIsSilent()
    {
        GamepadCore c;
        c.handle(added(1, "Pad"), 0);
        const auto as = c.handle(removed(99), 10);
        QVERIFY(as.isEmpty());
        QVERIFY(c.connected());
        QCOMPARE(c.controllerCount(), 1);
    }

    // THE ONE THAT MATTERS. SDL sends no button-up when a controller
    // disappears. A push-to-talk button held when the cable is pulled —
    // or when a Bluetooth pad's battery dies — would otherwise stay
    // logically pressed forever: an open microphone, with no UI showing
    // it is open and no controller left to release it with.
    void unpluggingWhileHeldSynthesisesTheRelease()
    {
        GamepadCore c;
        c.handle(added(1, "Pad"), 0);
        c.handle(button(1, GamepadButton::Misc1, true), 10);
        QVERIFY(c.buttonHeld(GamepadButton::Misc1));

        const auto as = c.handle(removed(1), 20);

        const auto releases = only(as, GamepadAction::Kind::ButtonReleased);
        QCOMPARE(releases.size(), 1);
        QCOMPARE(releases.first().button, GamepadButton::Misc1);
        QVERIFY(!c.buttonHeld(GamepadButton::Misc1));

        // The release must come BEFORE the disconnect, so a consumer
        // handling it still sees a world where a controller was attached.
        int releaseAt = -1;
        int disconnectAt = -1;
        for (int i = 0; i < as.size(); ++i) {
            if (as[i].kind == GamepadAction::Kind::ButtonReleased) releaseAt = i;
            if (as[i].kind == GamepadAction::Kind::Disconnected) disconnectAt = i;
        }
        QVERIFY(releaseAt >= 0);
        QVERIFY(disconnectAt >= 0);
        QVERIFY(releaseAt < disconnectAt);
        // Exactly one disconnect, so an implementation that emitted one
        // early AND one late could not satisfy the ordering check above
        // by accident.
        QCOMPARE(countOf(as, GamepadAction::Kind::Disconnected), 1);
    }

    // A stick deflected when the pad vanished must be reported back to
    // zero, or anything bound to the value is left stuck at it.
    void unpluggingZeroesADeflectedStick()
    {
        GamepadCore c;
        c.handle(added(1, "Pad"), 0);
        c.handle(axis(1, GamepadAxis::RightX, kFull), 10);
        QCOMPARE(c.axisValue(GamepadAxis::RightX), 1.0);

        const auto as = c.handle(removed(1), 20);
        const auto changes = only(as, GamepadAction::Kind::AxisChanged);
        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes.first().axis, GamepadAxis::RightX);
        QCOMPARE(changes.first().value, 0.0);
        QCOMPARE(c.axisValue(GamepadAxis::RightX), 0.0);
    }

    // An auto-repeating direction must die with the pad carrying it, or
    // the focus ring walks a list on its own with nothing attached.
    void unpluggingStopsAnInFlightRepeat()
    {
        GamepadCore c;
        c.handle(added(1, "Pad"), 0);
        c.handle(axis(1, GamepadAxis::LeftY, -kPush), 0);
        QCOMPARE(c.heldDirection(), NavDirection::Up);
        QCOMPARE(c.tick(kRepeatDelayMs).size(), 1);

        c.handle(removed(1), kRepeatDelayMs);
        QCOMPARE(c.heldDirection(), NavDirection::None);
        QVERIFY(c.tick(kRepeatDelayMs + 10 * kRepeatIntervalMs).isEmpty());
    }

    // Primary is the OLDEST still-connected pad: stable when one is
    // added (no hijack when a charger reconnects a spare), and it moves
    // to the next-oldest when the one being named goes away — WITHOUT a
    // disconnect, because a controller is still attached.
    void primaryIsOldestAndMovesOnItsRemoval()
    {
        GamepadCore c;
        c.handle(added(1, "First"), 0);
        auto as = c.handle(added(2, "Second"), 10);
        QCOMPARE(c.primaryName(), QStringLiteral("First"));
        QCOMPARE(countOf(as, GamepadAction::Kind::Connected), 0);
        QCOMPARE(c.controllerCount(), 2);

        as = c.handle(removed(1), 20);
        QCOMPARE(countOf(as, GamepadAction::Kind::Disconnected), 0);
        QCOMPARE(countOf(as, GamepadAction::Kind::PrimaryChanged), 1);
        QVERIFY(c.connected());
        QCOMPARE(c.primaryName(), QStringLiteral("Second"));
    }

    // Removing a non-primary pad changes neither the name nor the
    // connected state.
    void removingASecondaryPadIsQuiet()
    {
        GamepadCore c;
        c.handle(added(1, "First"), 0);
        c.handle(added(2, "Second"), 10);
        const auto as = c.handle(removed(2), 20);
        QCOMPARE(countOf(as, GamepadAction::Kind::Disconnected), 0);
        QCOMPARE(countOf(as, GamepadAction::Kind::PrimaryChanged), 0);
        QCOMPARE(c.primaryName(), QStringLiteral("First"));
    }

    // ================= buttons =================

    // A held button that re-reports down (some HID pads do, and SDL
    // passes it through) must not fire twice. For a toggle that means a
    // flicker; for push-to-talk it means a stream of redundant network
    // announcements.
    void repeatedDownReportsFireOnce()
    {
        GamepadCore c;
        c.handle(added(1, "Pad"), 0);
        auto as = c.handle(button(1, GamepadButton::South, true), 10);
        QCOMPARE(countOf(as, GamepadAction::Kind::ButtonPressed), 1);
        as = c.handle(button(1, GamepadButton::South, true), 20);
        QCOMPARE(countOf(as, GamepadAction::Kind::ButtonPressed), 0);
        as = c.handle(button(1, GamepadButton::South, false), 30);
        QCOMPARE(countOf(as, GamepadAction::Kind::ButtonReleased), 1);
        // And a spurious second up is also silent.
        as = c.handle(button(1, GamepadButton::South, false), 40);
        QVERIFY(as.isEmpty());
    }

    // Two pads holding the same button is one logical press and one
    // logical release. Without the reference count, pad A pressing, pad
    // B pressing and pad A releasing would close the microphone while
    // pad B was still held down.
    void twoPadsHoldingOneButtonIsOnePressAndOneRelease()
    {
        GamepadCore c;
        c.handle(added(1, "A"), 0);
        c.handle(added(2, "B"), 0);

        auto as = c.handle(button(1, GamepadButton::Misc1, true), 10);
        QCOMPARE(countOf(as, GamepadAction::Kind::ButtonPressed), 1);

        as = c.handle(button(2, GamepadButton::Misc1, true), 20);
        QCOMPARE(countOf(as, GamepadAction::Kind::ButtonPressed), 0);

        // Pad A lets go — still held by B, so NO release.
        as = c.handle(button(1, GamepadButton::Misc1, false), 30);
        QCOMPARE(countOf(as, GamepadAction::Kind::ButtonReleased), 0);
        QVERIFY(c.buttonHeld(GamepadButton::Misc1));

        // Now B does.
        as = c.handle(button(2, GamepadButton::Misc1, false), 40);
        QCOMPARE(countOf(as, GamepadAction::Kind::ButtonReleased), 1);
        QVERIFY(!c.buttonHeld(GamepadButton::Misc1));
    }

    // Same rule for the synthesised release on unplug: if the other pad
    // still holds it, nothing is released.
    void unpluggingOnePadDoesNotReleaseAButtonTheOtherHolds()
    {
        GamepadCore c;
        c.handle(added(1, "A"), 0);
        c.handle(added(2, "B"), 0);
        c.handle(button(1, GamepadButton::Misc1, true), 10);
        c.handle(button(2, GamepadButton::Misc1, true), 20);

        const auto as = c.handle(removed(1), 30);
        QCOMPARE(countOf(as, GamepadAction::Kind::ButtonReleased), 0);
        QVERIFY(c.buttonHeld(GamepadButton::Misc1));
    }

    // Events for a device that was never added are dropped, not applied
    // to some other pad.
    void buttonsFromAnUnknownDeviceAreDropped()
    {
        GamepadCore c;
        c.handle(added(1, "Pad"), 0);
        const auto as = c.handle(button(77, GamepadButton::South, true), 10);
        QVERIFY(as.isEmpty());
        QVERIFY(!c.buttonHeld(GamepadButton::South));
    }

    // ---- GamepadRegistry, tested directly ----
    //
    // These two look redundant against the GamepadCore cases above, and
    // a mutation run proves they are not. Breaking the registry's own
    // de-duplication, or its unknown-device rejection, does NOT fail any
    // GamepadCore test: the cross-pad reference count in GamepadCore
    // independently swallows both faults, so the outer behaviour is
    // unchanged and the inner guard is uncovered.
    //
    // That is defence in depth worth keeping — either layer alone is
    // enough to prevent a stuck button — but an uncovered guard is one
    // somebody deletes during a refactor on the grounds that nothing
    // tests it. So the guard is pinned where it lives.

    // setButton reports whether the state actually changed. A held
    // button re-reporting down (some HID pads do, and SDL passes it
    // through) must answer false.
    void registryDeduplicatesRepeatedButtonEdges()
    {
        GamepadRegistry r;
        QVERIFY(r.add(1, QStringLiteral("Pad")));
        QVERIFY(r.setButton(1, GamepadButton::South, true));
        QVERIFY(!r.setButton(1, GamepadButton::South, true));
        QCOMPARE(r.holdersOf(GamepadButton::South), 1);
        QVERIFY(r.setButton(1, GamepadButton::South, false));
        QVERIFY(!r.setButton(1, GamepadButton::South, false));
        QCOMPARE(r.holdersOf(GamepadButton::South), 0);
    }

    // Events for an id that was never added change nothing and say so,
    // rather than being attributed to some other pad.
    void registryRejectsUnknownDevices()
    {
        GamepadRegistry r;
        r.add(1, QStringLiteral("Pad"));
        QVERIFY(!r.setButton(99, GamepadButton::South, true));
        QVERIFY(!r.setAxis(99, GamepadAxis::LeftX, 30000));
        QCOMPARE(r.holdersOf(GamepadButton::South), 0);
        QCOMPARE(r.rawAxis(99, GamepadAxis::LeftX), 0);
        QCOMPARE(r.rawAxis(1, GamepadAxis::LeftX), 0);
        QCOMPARE(r.count(), 1);
        // GamepadButton::Unknown is never storable, so an SDL button we
        // do not map cannot collide with a real one.
        QVERIFY(!r.setButton(1, GamepadButton::Unknown, true));
        QCOMPARE(r.holdersOf(GamepadButton::Unknown), 0);
    }

    // ================= navigation integration =================

    // A d-pad press is one immediate step, then the delay, then repeats.
    void dpadStepsOnceThenRepeats()
    {
        GamepadCore c;
        c.handle(added(1, "Pad"), 0);

        const auto as = c.handle(button(1, GamepadButton::DpadDown, true), 0);
        const auto navs = only(as, GamepadAction::Kind::Navigate);
        QCOMPARE(navs.size(), 1);
        QCOMPARE(navs.first().direction, NavDirection::Down);
        QVERIFY(!navs.first().repeat);
        // A d-pad direction is also a button, for any consumer that
        // wants the raw edge.
        QCOMPARE(countOf(as, GamepadAction::Kind::ButtonPressed), 1);

        QVERIFY(c.tick(kRepeatDelayMs - 1).isEmpty());
        const auto rep = c.tick(kRepeatDelayMs);
        QCOMPARE(rep.size(), 1);
        QVERIFY(rep.first().repeat);
        QCOMPARE(rep.first().direction, NavDirection::Down);

        c.handle(button(1, GamepadButton::DpadDown, false), kRepeatDelayMs);
        QCOMPARE(c.heldDirection(), NavDirection::None);
        QVERIFY(c.tick(kRepeatDelayMs + 10 * kRepeatIntervalMs).isEmpty());
    }

    // The RIGHT stick must not navigate. It is kept free for a scroll
    // velocity, which is the thing in this app most worth scrolling; a
    // user with both sticks bound to the same action has one stick.
    void rightStickDoesNotNavigate()
    {
        GamepadCore c;
        c.handle(added(1, "Pad"), 0);
        const auto as = c.handle(axis(1, GamepadAxis::RightY, kFull), 0);
        QCOMPARE(countOf(as, GamepadAction::Kind::Navigate), 0);
        QCOMPARE(c.heldDirection(), NavDirection::None);
        // ...but it is still reported as an analogue value.
        QCOMPARE(countOf(as, GamepadAction::Kind::AxisChanged), 1);
    }

    // A stick nudged inside the deadzone produces NOTHING: no
    // navigation, no axis report. This is the phantom-input case and it
    // is also what lets the poll cadence doze (see below).
    void stickInsideTheDeadzoneIsSilent()
    {
        GamepadCore c;
        c.handle(added(1, "Pad"), 0);
        for (int raw : {100, -400, kStickDeadzone - 1, 0, 2000}) {
            const auto as = c.handle(axis(1, GamepadAxis::LeftX, raw), 10);
            QVERIFY2(as.isEmpty(), qPrintable(QString::number(raw)));
        }
        QCOMPARE(c.heldDirection(), NavDirection::None);
    }

    // A stick held still jitters by a few hundred raw units. At the
    // active cadence that would be ~125 signal emissions a second of
    // pure noise crossing into QML, so sub-epsilon movement is dropped.
    void heldStickDoesNotSpamAxisReports()
    {
        GamepadCore c;
        c.handle(added(1, "Pad"), 0);
        auto as = c.handle(axis(1, GamepadAxis::RightX, 20000), 0);
        QCOMPARE(countOf(as, GamepadAction::Kind::AxisChanged), 1);

        // Jitter well under the report epsilon.
        for (int d : {-150, 200, -80, 120}) {
            as = c.handle(axis(1, GamepadAxis::RightX, 20000 + d), 10);
            QCOMPARE(countOf(as, GamepadAction::Kind::AxisChanged), 0);
        }
        // A real move is reported.
        as = c.handle(axis(1, GamepadAxis::RightX, 26000), 20);
        QCOMPARE(countOf(as, GamepadAction::Kind::AxisChanged), 1);
    }

    // Returning to centre is ALWAYS reported, however small the final
    // step — otherwise anything driven by the value is left permanently
    // slightly deflected.
    void returnToCentreIsAlwaysReported()
    {
        GamepadCore c;
        c.handle(added(1, "Pad"), 0);
        // Just past the deadzone: the normalised value is far below the
        // report epsilon, so only the zero-edge rule reports it.
        auto as = c.handle(axis(1, GamepadAxis::RightX, kStickDeadzone + 2), 0);
        QCOMPARE(countOf(as, GamepadAction::Kind::AxisChanged), 1);
        QVERIFY(as.first().value > 0.0);
        QVERIFY(as.first().value < kAxisReportEpsilon);

        as = c.handle(axis(1, GamepadAxis::RightX, 0), 10);
        QCOMPARE(countOf(as, GamepadAction::Kind::AxisChanged), 1);
        QCOMPARE(as.first().value, 0.0);
    }

    // The d-pad outranks the stick: a user pressing a d-pad direction
    // while a thumb rests on the stick meant the d-pad.
    void dpadOutranksTheStick()
    {
        GamepadCore c;
        c.handle(added(1, "Pad"), 0);
        c.handle(axis(1, GamepadAxis::LeftX, kPush), 0);
        QCOMPARE(c.heldDirection(), NavDirection::Right);

        c.handle(button(1, GamepadButton::DpadUp, true), 10);
        QCOMPARE(c.heldDirection(), NavDirection::Up);

        // Releasing the d-pad falls back to the stick, which is still
        // pushed — and that is a direction CHANGE, so it steps again
        // rather than resuming mid-repeat.
        const auto as = c.handle(button(1, GamepadButton::DpadUp, false), 20);
        QCOMPARE(c.heldDirection(), NavDirection::Right);
        QCOMPARE(countOf(as, GamepadAction::Kind::Navigate), 1);
        QVERIFY(!only(as, GamepadAction::Kind::Navigate).first().repeat);
    }

    // Aggregation across pads: the largest magnitude wins, sign kept.
    void axisValueAggregatesAcrossPads()
    {
        GamepadCore c;
        c.handle(added(1, "A"), 0);
        c.handle(added(2, "B"), 0);
        c.handle(axis(1, GamepadAxis::RightX, 20000), 0);
        c.handle(axis(2, GamepadAxis::RightX, -30000), 0);
        QVERIFY(c.axisValue(GamepadAxis::RightX) < 0.0);
        QCOMPARE(c.axisValue(GamepadAxis::RightX),
                 normaliseAxis(GamepadAxis::RightX, -30000));
    }

    // ================= poll cadence =================

    void cadenceIdlesWithNothingAttached()
    {
        QCOMPARE(pollIntervalMs(false, false, 0), kPollIdleMs);
        // Even "input held" cannot speed up a cadence with no device —
        // there is nothing to hold.
        QCOMPARE(pollIntervalMs(false, true, 0), kPollIdleMs);
        QVERIFY(kPollIdleMs > kPollDozeMs);
    }

    void cadenceIsFastWhileInUseAndDozesWhenIdle()
    {
        QCOMPARE(pollIntervalMs(true, false, 0), kPollActiveMs);
        QCOMPARE(pollIntervalMs(true, false, kPollDozeAfterMs - 1),
                 kPollActiveMs);
        QCOMPARE(pollIntervalMs(true, false, kPollDozeAfterMs), kPollDozeMs);
        QVERIFY(kPollActiveMs < kPollDozeMs);
    }

    // THE PUSH-TO-TALK CASE. A held button generates no events while it
    // is held, so a cadence decided on elapsed time alone would doze off
    // mid-hold and notice the RELEASE up to a doze interval late. The
    // release edge is the one edge in this layer that must not be late:
    // it is the one that closes the microphone.
    void holdingSomethingPinsTheCadenceFast()
    {
        QCOMPARE(pollIntervalMs(true, true, 0), kPollActiveMs);
        QCOMPARE(pollIntervalMs(true, true, 60000), kPollActiveMs);
    }

    // The same thing, through GamepadCore, where "held" comes from real
    // device state rather than a bare boolean.
    void coreKeepsTheFastCadenceWhileAButtonIsHeld()
    {
        GamepadCore c;
        c.handle(added(1, "Pad"), 0);
        c.handle(button(1, GamepadButton::Misc1, true), 100);
        // Long after the doze threshold, with no further events.
        QCOMPARE(c.pollIntervalMs(100 + 10 * kPollDozeAfterMs), kPollActiveMs);

        c.handle(button(1, GamepadButton::Misc1, false),
                 100 + 10 * kPollDozeAfterMs);
        // Released: still fast briefly (the release was an action), then
        // dozes.
        QCOMPARE(c.pollIntervalMs(100 + 10 * kPollDozeAfterMs), kPollActiveMs);
        QCOMPARE(c.pollIntervalMs(100 + 11 * kPollDozeAfterMs + 1),
                 kPollDozeMs);
    }

    // The subtle one. A resting stick emits a steady trickle of axis
    // events that all flatten to 0.0. If the doze countdown were
    // measured from the last EVENT rather than the last ACTION, that
    // trickle would reset it forever and the cadence would never leave
    // 8 ms while a pad was attached — which is exactly the battery cost
    // the cadence exists to avoid, reached by a route that looks right.
    void deadzoneNoiseDoesNotPreventDozing()
    {
        GamepadCore c;
        c.handle(added(1, "Pad"), 0);

        qint64 t = 0;
        for (int i = 0; i < 400; ++i) {
            t += 8;
            c.handle(axis(1, GamepadAxis::LeftX, (i % 2) ? 300 : -250), t);
        }
        QVERIFY(t > kPollDozeAfterMs);
        QCOMPARE(c.pollIntervalMs(t), kPollDozeMs);
    }

    // A pad attached but never touched dozes immediately rather than
    // holding the fast cadence for the first two seconds of its life.
    void anUntouchedPadDozesStraightAway()
    {
        GamepadCore c;
        c.handle(added(1, "Pad"), 0);
        // The Added itself was an action, so it is fast at first...
        QCOMPARE(c.pollIntervalMs(0), kPollActiveMs);
        // ...and settles.
        QCOMPARE(c.pollIntervalMs(kPollDozeAfterMs), kPollDozeMs);
    }
};

QTEST_MAIN(GamepadTest)
#include "test_gamepad.moc"
