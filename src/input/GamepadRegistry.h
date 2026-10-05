#pragma once

// Which pads are attached, and what each of them is currently holding
// down.
//
// Those two things are one class rather than two because of the failure
// they exist to prevent: SDL sends no button-up when a controller
// disappears. A pad whose button was held when the cable was pulled —
// or when the Bluetooth link dropped, which on a handheld happens
// because the battery died — leaves that button logically pressed
// forever. For a focus-navigation button that is a stuck focus ring.
// For push-to-talk, which is the whole reason this layer exists, it is
// an open microphone with no UI anywhere showing it is open, and no
// controller left to release it with.
//
// So: per-device held state is owned by the thing that knows when the
// device went away, and removal RETURNS the list of buttons to
// synthesise releases for rather than leaving the caller to remember.
//
// No SDL, no Qt event loop, no clock. Pure bookkeeping.
//
// ---- Multiple pads ----
//
// Input from EVERY connected pad is reported, with no arbitration. This
// is a chat client, not a split-screen game: there is one focus ring and
// one microphone, and the user with a pad in each hand means to use
// both. Arbitration would have to answer "which pad owns the UI", and
// every answer is wrong somewhere — "most recently connected" hijacks
// mid-use when a charger reconnects a spare, "first connected" means the
// Steam Deck's built-in pad permanently outranks the one the user
// deliberately plugged in.
//
// `primaryName()` exists only so the UI has a name to display. It is the
// oldest still-connected pad, which is stable (it does not change when a
// pad is added) and recovers sensibly (it moves to the next-oldest when
// the one being named goes away).

#include <algorithm>
#include <array>

#include <QHash>
#include <QList>
#include <QString>

#include "input/GamepadDeadzone.h"
#include "input/GamepadTypes.h"

namespace bsfchat::input {

// Axes are stored in a flat array indexed by the enum value, not a hash.
// Two reasons: iteration order is then defined, which the tests depend
// on when they assert the order synthesised zeroing happens in; and
// there is no need to answer whether QSet/QHash of a scoped enum hashes
// the same way across Qt versions.
inline constexpr int kAxisCount = 7; // Unknown + 6 real axes

inline int axisIndex(GamepadAxis axis)
{
    const int i = int(axis);
    return (i >= 0 && i < kAxisCount) ? i : 0;
}

class GamepadRegistry {
public:
    // What removal leaves for the caller to clean up.
    struct Removal {
        // False when the instance id was never added. SDL does send
        // removal events for joysticks it never mapped as gamepads, so
        // this is a normal case and not an error.
        bool known = false;
        // Buttons that were down, in the order they were pressed. The
        // caller must emit a release for each; nothing else will.
        QList<GamepadButton> releasedButtons;
        // Axes that were away from zero, in enum order. The caller must
        // report each as 0 — a stick deflected when the pad vanished
        // otherwise leaves whatever it was driving stuck at that value.
        QList<GamepadAxis> zeroedAxes;
        // The removed pad was the one primaryName() was naming, so the
        // displayed name has changed even if pads remain.
        bool wasPrimary = false;
        // No pads are left.
        bool nowEmpty = false;
    };

    // Returns true when this is a new device.
    //
    // A duplicate Added for an id already present is ignored rather than
    // treated as a re-add. SDL re-enumerates on some platforms (and
    // Steam's virtual-gamepad shim re-announces when a game grabs the
    // pad), so a second Added for a live id is normal traffic. Treating
    // it as new would double-count the pad, and resetting its held state
    // would drop a button that is still physically down.
    bool add(quint32 instanceId, const QString& name)
    {
        if (m_devices.contains(instanceId)) return false;
        Device d;
        d.name = name;
        m_devices.insert(instanceId, d);
        m_order.append(instanceId);
        return true;
    }

    Removal remove(quint32 instanceId)
    {
        Removal r;
        const auto it = m_devices.find(instanceId);
        if (it == m_devices.end()) return r;

        r.known = true;
        r.wasPrimary = (!m_order.isEmpty() && m_order.first() == instanceId);
        r.releasedButtons = it->heldButtons;
        for (int i = 0; i < kAxisCount; ++i) {
            if (it->rawAxis[i] != 0)
                r.zeroedAxes.append(GamepadAxis(i));
        }

        m_devices.erase(it);
        m_order.removeAll(instanceId);
        r.nowEmpty = m_devices.isEmpty();
        return r;
    }

    // Record a button edge. Returns true when the state actually changed
    // on a device we know about — i.e. when the caller should report it.
    //
    // The repeat de-duplication matters: a held button on some HID pads
    // produces repeated down reports, and SDL passes them through. A
    // caller that trusted every down would fire the action repeatedly,
    // which for a toggle (mute) means it flickers and for push-to-talk
    // means a stream of redundant network announcements.
    bool setButton(quint32 instanceId, GamepadButton button, bool down)
    {
        const auto it = m_devices.find(instanceId);
        if (it == m_devices.end()) return false;
        if (button == GamepadButton::Unknown) return false;

        const qsizetype at = it->heldButtons.indexOf(button);
        if (down) {
            if (at >= 0) return false;
            it->heldButtons.append(button);
            return true;
        }
        if (at < 0) return false;
        it->heldButtons.removeAt(at);
        return true;
    }

    // Record a raw axis reading. Returns false for an unknown device.
    bool setAxis(quint32 instanceId, GamepadAxis axis, qint16 raw)
    {
        const auto it = m_devices.find(instanceId);
        if (it == m_devices.end()) return false;
        if (axis == GamepadAxis::Unknown) return false;
        it->rawAxis[axisIndex(axis)] = raw;
        return true;
    }

    qint16 rawAxis(quint32 instanceId, GamepadAxis axis) const
    {
        const auto it = m_devices.constFind(instanceId);
        if (it == m_devices.constEnd()) return 0;
        return it->rawAxis[axisIndex(axis)];
    }

    bool isHeld(quint32 instanceId, GamepadButton button) const
    {
        const auto it = m_devices.constFind(instanceId);
        if (it == m_devices.constEnd()) return false;
        return it->heldButtons.contains(button);
    }

    bool contains(quint32 instanceId) const
    {
        return m_devices.contains(instanceId);
    }

    // How many attached pads are holding this button down.
    //
    // The caller turns button edges into signals using this as a
    // reference count, so that two pads holding the same button produce
    // one logical press and one logical release. Push-to-talk is why:
    // without the count, pad A pressing, pad B pressing and then pad A
    // releasing would close the microphone while pad B was still held
    // down — a stuck-closed mic rather than a stuck-open one, but just
    // as wrong and harder to explain.
    int holdersOf(GamepadButton button) const
    {
        int n = 0;
        for (auto it = m_devices.constBegin(); it != m_devices.constEnd(); ++it) {
            if (it->heldButtons.contains(button)) ++n;
        }
        return n;
    }

    int count() const { return int(m_devices.size()); }
    bool anyConnected() const { return !m_devices.isEmpty(); }

    // Oldest still-connected pad's name, or empty when none. See the
    // header comment for why "oldest" and not "most recent".
    QString primaryName() const
    {
        if (m_order.isEmpty()) return {};
        const auto it = m_devices.constFind(m_order.first());
        return it == m_devices.constEnd() ? QString() : it->name;
    }

    // Instance ids in connection order. Order is defined so tests can
    // assert it; m_devices (a QHash) has none.
    QList<quint32> ids() const { return m_order; }

    // Is the user touching anything right now?
    //
    // This drives the poll cadence, and the subtlety is that a HELD
    // button generates no events at all while it is held. A cadence
    // decision made on "time since the last event" alone would therefore
    // doze off while push-to-talk was held down, and the release — the
    // edge that closes the microphone — would be noticed up to a doze
    // interval late. Holding anything pins the poll at the fast cadence.
    bool anyInputHeld() const
    {
        for (auto it = m_devices.constBegin(); it != m_devices.constEnd(); ++it) {
            if (!it->heldButtons.isEmpty()) return true;
            for (int i = 1; i < kAxisCount; ++i) {
                // Deadzone, not != 0: a resting stick is almost never at
                // a raw 0, so a bare non-zero test would report "input
                // held" permanently and defeat dozing entirely.
                if (normaliseAxis(GamepadAxis(i), it->rawAxis[i]) != 0.0)
                    return true;
            }
        }
        return false;
    }

private:
    struct Device {
        QString name;
        // Press order, so synthesised releases come out in a defined
        // sequence rather than a hash order that varies per run.
        QList<GamepadButton> heldButtons;
        std::array<qint16, kAxisCount> rawAxis{};
    };

    QHash<quint32, Device> m_devices;
    QList<quint32> m_order;
};

} // namespace bsfchat::input
