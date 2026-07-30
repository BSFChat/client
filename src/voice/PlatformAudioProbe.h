#pragma once

// Result of the livekit::PlatformAudio device-enumeration prototype.
// See PlatformAudioProbe.cpp for what is and is not exercised.

#include <QString>
#include <QStringList>

struct PlatformAudioProbeResult {
    bool constructed = false;      // the ADM was created at all
    QString error;                 // non-empty when construction/enumeration threw

    int recordingCount = 0;
    int playoutCount = 0;
    QStringList recordingNames;    // AudioDeviceInfo::name
    QStringList recordingIds;      // AudioDeviceInfo::id (stable selector)
    QStringList playoutNames;

    QStringList qtInputDescriptions;   // QAudioDevice::description()
    QStringList qtOutputDescriptions;

    // How many PlatformAudio recording names match a Qt description()
    // EXACTLY. The settings UI persists a Qt description string and
    // AudioWorker looks it up by exact match, so this is the number that
    // decides whether stored device preferences survive a switch.
    int exactInputNameMatches = 0;
};

PlatformAudioProbeResult probePlatformAudio();
