#pragma once

// Which audio device the voice pipeline should be using, decided as a
// value rather than as a side effect.
//
// Until now the answer was computed exactly once, inside
// AudioWorker::startDevices(), by resolving the saved description and
// falling back to QMediaDevices::defaultAudio{Input,Output}(). Nothing
// subscribed to QMediaDevices::audio{Inputs,Outputs}Changed, so the
// answer was frozen at join:
//
//   * AirPods connected after join were never used, even though the OS
//     had already made them the system-wide default;
//   * changing the default output in Control Centre mid-call did
//     nothing;
//   * a device that disappeared mid-call left a dead sink behind, and
//     the call went silent with no way back short of rejoining.
//
// The device set now arrives as a stream of snapshots and this header
// decides what to do with each one. It is deliberately free of Qt
// Multimedia: no QAudioDevice, no QMediaDevices, no device can be
// opened from here. That is what lets the interesting cases — a
// preferred device vanishing and coming back, two devices sharing one
// description, a default that moved — be tested on a headless CI box
// with no sound card at all.
//
// The caller (AudioWorker, on the audio thread) owns the mapping back to
// real QAudioDevice objects and the actual restart.

#include <QList>
#include <QString>
#include <QtGlobal>

namespace bsfchat::voice {

// One entry of a device snapshot, flattened out of QAudioDevice.
//
// `id` is QAudioDevice::id() as a string. It is stable while a device
// stays connected, which is all this needs, but NOT across reboots on
// every platform — which is why the user's choice is persisted as a
// description and the id is only ever a tie-breaker hint.
struct DeviceInfo {
    QString id;
    QString description;
    bool isDefault = false;

    bool operator==(const DeviceInfo&) const = default;
};

enum class DeviceAction {
    // The resolved device is already the one in use. Nothing to do —
    // and, importantly, nothing may be done: restarting a sink that is
    // already on the right device is an audible gap for no reason, and
    // a Bluetooth connect delivers several notifications that all
    // resolve to the same device.
    Keep,
    // Restart this direction on DeviceDecision::id.
    Switch,
    // There is no usable device in this direction at all.
    None,
};

struct DeviceDecision {
    DeviceAction action = DeviceAction::None;
    QString id;
    QString description;
    // The resolved device is the system default, not an explicit match
    // on the saved description. True both when the user asked to follow
    // the default (empty preference) and when we fell back to it.
    bool isSystemDefault = false;
    // A specific device was asked for and is not in the snapshot. The
    // caller logs this differently: "your device went away" is a
    // different event from "the default moved", even though both end up
    // on the same device.
    bool preferenceMissing = false;

    bool operator==(const DeviceDecision&) const = default;
};

// The snapshot's default device, or its first entry when no entry is
// flagged (some backends only mark the default implicitly, by ordering).
// Null when the snapshot is empty.
inline const DeviceInfo* defaultDevice(const QList<DeviceInfo>& devices)
{
    for (const DeviceInfo& d : devices) {
        if (d.isDefault) return &d;
    }
    return devices.isEmpty() ? nullptr : &devices.constFirst();
}

// `savedDescription` is the persisted preference: empty means "follow
// the system default". `hintId` is the id this preference resolved to
// last time, persisted alongside it purely to break ties. `devices` is
// the current snapshot. `inUseId` is the id of the device this direction
// is currently open on, empty when nothing is open.
inline DeviceDecision resolveDevice(const QString& savedDescription,
                                    const QString& hintId,
                                    const QList<DeviceInfo>& devices,
                                    const QString& inUseId)
{
    DeviceDecision out;
    if (devices.isEmpty()) {
        out.action = DeviceAction::None;
        return out;
    }

    const DeviceInfo* chosen = nullptr;

    if (!savedDescription.isEmpty()) {
        // Match by description, because that is what is persisted — but
        // a description is not unique. Two identical USB headsets
        // present the same string, and so do some virtual-device
        // drivers. Prefer whichever one we resolved to last time, then
        // the system default, then the first; anything is better than
        // silently alternating between two devices across restarts.
        const DeviceInfo* first = nullptr;
        const DeviceInfo* byHint = nullptr;
        const DeviceInfo* byDefault = nullptr;
        for (const DeviceInfo& d : devices) {
            if (d.description != savedDescription) continue;
            if (!first) first = &d;
            if (!byHint && !hintId.isEmpty() && d.id == hintId) byHint = &d;
            if (!byDefault && d.isDefault) byDefault = &d;
        }
        chosen = byHint ? byHint : (byDefault ? byDefault : first);
        out.preferenceMissing = (chosen == nullptr);
    }

    if (!chosen) {
        // Either the user follows the default, or their device is gone
        // and the default is the only sane place to put the audio. Both
        // land here; preferenceMissing distinguishes them for the log.
        chosen = defaultDevice(devices);
        out.isSystemDefault = true;
    }
    if (!chosen) {
        out.action = DeviceAction::None;
        return out;
    }

    out.id = chosen->id;
    out.description = chosen->description;
    out.action = (!inUseId.isEmpty() && chosen->id == inUseId)
                     ? DeviceAction::Keep
                     : DeviceAction::Switch;
    return out;
}

// Collapses a burst of device-change notifications into one restart per
// direction per window.
//
// Bursts are the normal case, not the pathological one. Connecting a
// Bluetooth headset fires several notifications as the OS publishes the
// device, then makes it the default, then the profile settles; AirPods
// fire more again when the microphone opens and they drop into
// hands-free mode. Acting on each one means several sink restarts in a
// second, every one of them an audible gap.
//
// Trailing edge, first-event-armed: the first event of a window arms a
// timer, every event inside that window is absorbed into it, and the
// restart happens once when the window closes against the newest
// snapshot. Deliberately NOT the "restart the timer on every event"
// variant, which never fires at all while events keep arriving.
//
// The clock is the caller's — a millisecond count, monotonic or not, it
// is only ever subtracted from itself. That keeps this testable against
// a fake clock instead of against sleeps.
class RestartDebounce {
public:
    explicit RestartDebounce(qint64 windowMs = 500) : m_windowMs(windowMs) {}

    // Records a notification arriving at `nowMs`. Returns true when the
    // caller should arm its timer for windowMs() from now, false when an
    // already-armed window will cover this event.
    bool onEvent(qint64 nowMs)
    {
        if (m_armedAtMs >= 0 && nowMs - m_armedAtMs < m_windowMs) return false;
        m_armedAtMs = nowMs;
        return true;
    }

    // The window closed and the caller is performing the restart now.
    void onFire() { m_armedAtMs = -1; }

    // Teardown: forget any armed window so a later session starts clean.
    void reset() { m_armedAtMs = -1; }

    bool armed() const { return m_armedAtMs >= 0; }
    qint64 windowMs() const { return m_windowMs; }

private:
    qint64 m_windowMs;
    qint64 m_armedAtMs = -1;  // -1 == idle
};

} // namespace bsfchat::voice
