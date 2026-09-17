// Unit tests for AudioDevicePolicy — the decision half of live audio
// device handling.
//
// The bug these exist for: the device in use was resolved exactly once,
// in AudioWorker::startDevices(), so AirPods that connected after join
// were never picked up even once macOS had made them the system-wide
// default, and a device that vanished mid-call left a dead sink behind.
// Fixing that means re-deciding on every QMediaDevices change
// notification, which in turn means the decision has to be a pure
// function — it cannot be exercised on a CI box with no sound card
// otherwise, and "does it switch back when the headset reappears" is
// exactly the case nobody will test by hand twice.
//
// No QAudioDevice, no QMediaDevices, no audio thread: everything below
// is DeviceInfo structs and a fake clock.

#include <QtTest>

#include "voice/AudioDevicePolicy.h"

using namespace bsfchat::voice;

namespace {

DeviceInfo dev(const char* id, const char* desc, bool isDefault = false)
{
    return DeviceInfo{QString::fromLatin1(id), QString::fromLatin1(desc),
                      isDefault};
}

// The owner's Mac, roughly: built-in output is the default, a USB mic is
// plugged in, no AirPods yet.
QList<DeviceInfo> macOutputsWithoutAirPods()
{
    return {dev("BuiltInSpeakerDevice", "External Headphones", true),
            dev("AppleUSBAudioEngine", "RODE NT-USB+")};
}

// The same Mac once AirPods connect: macOS makes them the default, and
// Qt's darwin backend publishes the default first with isDefault set.
QList<DeviceInfo> macOutputsWithAirPods()
{
    return {dev("AirPodsPro-Josh", "Josh's AirPods Pro", true),
            dev("BuiltInSpeakerDevice", "External Headphones"),
            dev("AppleUSBAudioEngine", "RODE NT-USB+")};
}

} // namespace

class AudioDevicePolicyTest : public QObject {
    Q_OBJECT

private slots:
    // ---- following the system default ----

    // The owner's actual configuration: audio/outputDevice is "", so the
    // pipeline follows whatever macOS says the default is.
    void emptyPreferenceTakesTheDefault()
    {
        const DeviceDecision d =
            resolveDevice(QString(), QString(), macOutputsWithoutAirPods(),
                          QString());
        QCOMPARE(d.action, DeviceAction::Switch);
        QCOMPARE(d.id, QStringLiteral("BuiltInSpeakerDevice"));
        QCOMPARE(d.description, QStringLiteral("External Headphones"));
        QVERIFY(d.isSystemDefault);
        QVERIFY(!d.preferenceMissing);
    }

    // The reported bug, as a test: AirPods connect mid-call, macOS makes
    // them the default, and we are already playing out of the built-in
    // output. Before the fix this stayed on External Headphones forever.
    void defaultMovingMidCallSwitches()
    {
        const DeviceDecision d =
            resolveDevice(QString(), QString(), macOutputsWithAirPods(),
                          QStringLiteral("BuiltInSpeakerDevice"));
        QCOMPARE(d.action, DeviceAction::Switch);
        QCOMPARE(d.id, QStringLiteral("AirPodsPro-Josh"));
        QVERIFY(d.isSystemDefault);
    }

    // Once we are on the new default, further notifications resolve to
    // the same id and must NOT restart the sink. This is the guard that
    // makes a Bluetooth notification burst harmless even if the debounce
    // lets more than one through.
    void unchangedDefaultKeeps()
    {
        const DeviceDecision d =
            resolveDevice(QString(), QString(), macOutputsWithAirPods(),
                          QStringLiteral("AirPodsPro-Josh"));
        QCOMPARE(d.action, DeviceAction::Keep);
        QCOMPARE(d.id, QStringLiteral("AirPodsPro-Josh"));
    }

    // AirPods disconnect while we are on them: the default falls back to
    // the built-in output and we follow it down as well as up.
    void defaultMovingBackSwitchesBack()
    {
        const DeviceDecision d =
            resolveDevice(QString(), QString(), macOutputsWithoutAirPods(),
                          QStringLiteral("AirPodsPro-Josh"));
        QCOMPARE(d.action, DeviceAction::Switch);
        QCOMPARE(d.id, QStringLiteral("BuiltInSpeakerDevice"));
        QVERIFY(d.isSystemDefault);
    }

    // Not every backend flags the default; some only order the list.
    // Falling back to the first entry beats falling back to nothing.
    void noFlaggedDefaultUsesTheFirstEntry()
    {
        const QList<DeviceInfo> devices{dev("a", "Alpha"), dev("b", "Beta")};
        const DeviceDecision d =
            resolveDevice(QString(), QString(), devices, QString());
        QCOMPARE(d.action, DeviceAction::Switch);
        QCOMPARE(d.id, QStringLiteral("a"));
        QVERIFY(d.isSystemDefault);
    }

    // ---- explicit choices ----

    // The owner's input preference: a named device that is present. It
    // wins over the system default, which is the whole point of having
    // chosen it.
    void explicitPreferenceWinsOverDefault()
    {
        const DeviceDecision d =
            resolveDevice(QStringLiteral("RODE NT-USB+"), QString(),
                          macOutputsWithoutAirPods(), QString());
        QCOMPARE(d.action, DeviceAction::Switch);
        QCOMPARE(d.id, QStringLiteral("AppleUSBAudioEngine"));
        QVERIFY(!d.isSystemDefault);
        QVERIFY(!d.preferenceMissing);
    }

    // Absent at join — the pre-existing behaviour, kept: use the default
    // rather than refusing to open anything.
    void missingPreferenceFallsBackToDefault()
    {
        const DeviceDecision d =
            resolveDevice(QStringLiteral("Some Unplugged Headset"), QString(),
                          macOutputsWithoutAirPods(), QString());
        QCOMPARE(d.action, DeviceAction::Switch);
        QCOMPARE(d.id, QStringLiteral("BuiltInSpeakerDevice"));
        QVERIFY(d.isSystemDefault);
        // Distinguished from "the user follows the default" so the log
        // line can say which of the two happened.
        QVERIFY(d.preferenceMissing);
    }

    // Disappears mid-call while in use: fall back rather than keep
    // writing into a dead sink.
    void preferenceDisappearingMidCallFallsBack()
    {
        const DeviceDecision d =
            resolveDevice(QStringLiteral("RODE NT-USB+"), QString(),
                          QList<DeviceInfo>{dev("BuiltInSpeakerDevice",
                                                "External Headphones", true)},
                          QStringLiteral("AppleUSBAudioEngine"));
        QCOMPARE(d.action, DeviceAction::Switch);
        QCOMPARE(d.id, QStringLiteral("BuiltInSpeakerDevice"));
        QVERIFY(d.preferenceMissing);
    }

    // ...and comes back: we switch back to it without the user touching
    // anything.
    void preferenceReappearingSwitchesBack()
    {
        const DeviceDecision d =
            resolveDevice(QStringLiteral("RODE NT-USB+"), QString(),
                          macOutputsWithoutAirPods(),
                          QStringLiteral("BuiltInSpeakerDevice"));
        QCOMPARE(d.action, DeviceAction::Switch);
        QCOMPARE(d.id, QStringLiteral("AppleUSBAudioEngine"));
        QVERIFY(!d.preferenceMissing);
    }

    // An explicit device that is already in use stays put even when it
    // is not the system default — following the default must not
    // override an explicit choice.
    void explicitPreferenceInUseKeeps()
    {
        const DeviceDecision d =
            resolveDevice(QStringLiteral("RODE NT-USB+"), QString(),
                          macOutputsWithAirPods(),
                          QStringLiteral("AppleUSBAudioEngine"));
        QCOMPARE(d.action, DeviceAction::Keep);
    }

    // ---- duplicate descriptions ----

    // Two devices, one description. Without the id hint this is a coin
    // flip that can land differently on every notification, which means
    // a sink restart per notification forever.
    void duplicateDescriptionsPreferTheHintedId()
    {
        const QList<DeviceInfo> devices{
            dev("usb-port-1", "USB Audio Device", true),
            dev("usb-port-2", "USB Audio Device"),
        };
        const DeviceDecision d =
            resolveDevice(QStringLiteral("USB Audio Device"),
                          QStringLiteral("usb-port-2"), devices, QString());
        QCOMPARE(d.action, DeviceAction::Switch);
        QCOMPARE(d.id, QStringLiteral("usb-port-2"));
    }

    // Hint names a device that is no longer there: fall through to the
    // default among the matches rather than to nothing.
    void duplicateDescriptionsStaleHintPrefersTheDefault()
    {
        const QList<DeviceInfo> devices{
            dev("usb-port-1", "USB Audio Device"),
            dev("usb-port-2", "USB Audio Device", true),
        };
        const DeviceDecision d =
            resolveDevice(QStringLiteral("USB Audio Device"),
                          QStringLiteral("usb-port-9"), devices, QString());
        QCOMPARE(d.id, QStringLiteral("usb-port-2"));
        QVERIFY(!d.isSystemDefault);   // an explicit match, not a fallback
    }

    // No hint and no default among the matches: first match, stably.
    void duplicateDescriptionsWithoutHintAreStable()
    {
        const QList<DeviceInfo> devices{
            dev("builtin", "External Headphones", true),
            dev("usb-port-1", "USB Audio Device"),
            dev("usb-port-2", "USB Audio Device"),
        };
        for (int i = 0; i < 3; ++i) {
            const DeviceDecision d =
                resolveDevice(QStringLiteral("USB Audio Device"), QString(),
                              devices, QString());
            QCOMPARE(d.id, QStringLiteral("usb-port-1"));
        }
    }

    // ---- degenerate snapshots ----

    void emptySnapshotYieldsNone()
    {
        const DeviceDecision followDefault =
            resolveDevice(QString(), QString(), {}, QString());
        QCOMPARE(followDefault.action, DeviceAction::None);
        QVERIFY(followDefault.id.isEmpty());

        // Same answer when a device was in use and every device went
        // away — the caller closes the sink rather than keeping a dead
        // one open.
        const DeviceDecision wasInUse =
            resolveDevice(QStringLiteral("RODE NT-USB+"), QString(), {},
                          QStringLiteral("AppleUSBAudioEngine"));
        QCOMPARE(wasInUse.action, DeviceAction::None);
    }

    // Nothing in use yet + a snapshot: always Switch, never Keep. Keep
    // with nothing open would leave the pipeline silent forever.
    void nothingInUseAlwaysSwitches()
    {
        const DeviceDecision d =
            resolveDevice(QString(), QString(), macOutputsWithoutAirPods(),
                          QString());
        QCOMPARE(d.action, DeviceAction::Switch);
    }

    // ---- debounce ----

    // A Bluetooth connect fires several notifications in a few hundred
    // milliseconds. Exactly one restart should come out of them.
    void debounceCoalescesABurst()
    {
        RestartDebounce d(500);
        qint64 now = 1'000'000;   // fake clock, arbitrary origin

        QVERIFY(d.onEvent(now));          // first event arms the window
        QVERIFY(d.armed());
        QVERIFY(!d.onEvent(now + 10));    // ...everything else is absorbed
        QVERIFY(!d.onEvent(now + 120));
        QVERIFY(!d.onEvent(now + 499));

        // The window closes and the caller restarts once.
        d.onFire();
        QVERIFY(!d.armed());
    }

    // A second burst after the window has closed gets its own restart —
    // debounce must not swallow a genuine later change (AirPods
    // reconnecting a minute after they dropped).
    void debounceArmsAgainAfterTheWindow()
    {
        RestartDebounce d(500);
        qint64 now = 0;

        QVERIFY(d.onEvent(now));
        QVERIFY(!d.onEvent(now + 100));
        d.onFire();

        QVERIFY(d.onEvent(now + 600));
        QVERIFY(d.armed());
    }

    // Events arriving without pause must still let the window close:
    // this is the failure mode of the "restart the timer on every event"
    // debounce, which never fires while notifications keep coming.
    void debounceDoesNotStarveUnderContinuousEvents()
    {
        RestartDebounce d(500);
        // The caller's single-shot timer, modelled: arming schedules a
        // fire one window later, and the fire is what performs the
        // restart and re-opens the debounce.
        qint64 fireAt = -1;
        int restarts = 0;
        for (qint64 t = 0; t <= 5000; t += 50) {
            if (fireAt >= 0 && t >= fireAt) {
                d.onFire();
                ++restarts;
                fireAt = -1;
            }
            if (d.onEvent(t)) fireAt = t + d.windowMs();
        }
        // 5000ms of unbroken notifications at 20Hz is 101 events. What
        // comes out is one restart per window, not one per event — and,
        // crucially, not zero.
        QVERIFY2(restarts >= 2, "debounce never fired under continuous events");
        QVERIFY2(restarts <= 11, "debounce let too many restarts through");
    }

    // The two directions debounce independently: an input burst must not
    // delay an output switch. (Kept as separate objects in AudioWorker;
    // this pins the reason.)
    void debounceIsPerDirection()
    {
        RestartDebounce input(500);
        RestartDebounce output(500);

        QVERIFY(input.onEvent(0));
        QVERIFY(!input.onEvent(100));
        // Output is untouched by the input burst.
        QVERIFY(output.onEvent(100));
    }

    void debounceResetClearsTheWindow()
    {
        RestartDebounce d(500);
        QVERIFY(d.onEvent(0));
        d.reset();
        QVERIFY(!d.armed());
        // A fresh session arms immediately rather than waiting out a
        // window from the previous one.
        QVERIFY(d.onEvent(10));
    }
};

QTEST_APPLESS_MAIN(AudioDevicePolicyTest)
#include "test_audio_device_policy.moc"
