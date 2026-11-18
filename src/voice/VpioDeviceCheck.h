#pragma once

// Did the VoiceProcessingIO unit actually open the device we asked for?
//
// We ask with kAudioOutputUnitProperty_CurrentDevice, one device per
// element, and the unit accepts the request with noErr. That is not the
// same as honouring it. A user reported (2026-11-14) BSFChat's settings
// saying "In use: RODE NT-USB" while the call was going out through the
// AirPods microphone. The caption only repeated what we had asked for, so
// it could not have caught that.
//
// So after the unit starts, DarwinVpioBackend reads the device back off
// each element and hands the result here. VPIO may report either the
// physical device or the private aggregate it builds internally. For an
// aggregate, the best available evidence is its list of active
// sub-devices.
//
// Only a DEFINITE mismatch counts. Anything we cannot read is Unknown and
// is let through: wrongly giving up echo cancellation for everyone would
// be a worse bug than the one this catches.
//
// Pure: no CoreAudio, so it is unit-tested on Linux.

#include <QString>
#include <QStringList>

namespace bsfchat::voice {

enum class DeviceCheck {
    Honoured,     // the requested device is the one (or among the ones) open
    NotHonoured,  // the unit opened something else
    Unknown,      // nothing was requested, or the read-back told us nothing
};

struct DeviceReadback {
    QString uid;                // UID of the device the element reports
    bool isAggregate = false;   // that device is an aggregate
    QStringList subDeviceUids;  // its active sub-devices, if it is one
};

inline DeviceCheck checkDeviceHonoured(const QString& requestedUid,
                                       const DeviceReadback& got)
{
    if (requestedUid.isEmpty() || got.uid.isEmpty()) return DeviceCheck::Unknown;
    if (got.uid == requestedUid) return DeviceCheck::Honoured;
    if (!got.isAggregate) return DeviceCheck::NotHonoured;
    // An aggregate that holds the requested device: as far as we can tell,
    // honoured. It might be supplying only the other direction, but nothing
    // here can tell those apart.
    if (got.subDeviceUids.isEmpty()) return DeviceCheck::Unknown;
    return got.subDeviceUids.contains(requestedUid) ? DeviceCheck::Honoured
                                                    : DeviceCheck::NotHonoured;
}

} // namespace bsfchat::voice
