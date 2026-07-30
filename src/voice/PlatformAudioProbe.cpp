// Answers the phase-1 exit question the migration plan named: can
// livekit::PlatformAudio's device enumeration back the existing
// device-selection settings UI and the mic level meter?
//
// This is a PROTOTYPE PROBE, not the audio path. It deliberately does the
// minimum that produces a real answer:
//
//   - constructs a PlatformAudio (creates WebRTC's Audio Device Module),
//   - enumerates recording and playout devices,
//   - compares those names against what QMediaDevices reports, because the
//     settings UI persists a device by its Qt description() string
//     (see AudioWorker.cpp:24-33 and Settings.cpp:236-240,359).
//
// It does NOT call createAudioSource(). That would start microphone capture
// and trigger a macOS TCC prompt; enumeration alone does not.

#include "voice/PlatformAudioProbe.h"

#include <livekit/livekit.h>
#include <livekit/platform_audio.h>

#include <QMediaDevices>
#include <QAudioDevice>

PlatformAudioProbeResult probePlatformAudio() {
    PlatformAudioProbeResult r;
    // The SDK is a process-wide singleton behind an FFI boundary and every
    // call fails with "LiveKit is not initialized" until this runs. Worth
    // recording: nothing in the type system hints at it, and the failure
    // surfaces as a generic FfiClient error from an unrelated-looking call.
    if (!livekit::initialize(livekit::LogLevel::Warn)) {
        r.error = QStringLiteral("livekit::initialize() failed");
        return r;
    }
    try {
        const livekit::PlatformAudio pa;
        r.constructed = true;

        for (const auto& d : pa.recordingDevices()) {
            r.recordingNames.append(QString::fromStdString(d.name));
            r.recordingIds.append(QString::fromStdString(d.id));
        }
        for (const auto& d : pa.playoutDevices()) {
            r.playoutNames.append(QString::fromStdString(d.name));
        }
        r.recordingCount = pa.recordingDeviceCount();
        r.playoutCount = pa.playoutDeviceCount();
    } catch (const livekit::PlatformAudioError& e) {
        r.error = QString::fromUtf8(e.what());
        return r;
    } catch (const std::exception& e) {
        r.error = QString::fromUtf8(e.what());
        return r;
    }

    for (const auto& d : QMediaDevices::audioInputs()) {
        r.qtInputDescriptions.append(d.description());
    }
    for (const auto& d : QMediaDevices::audioOutputs()) {
        r.qtOutputDescriptions.append(d.description());
    }

    // The settings UI stores a Qt description() and AudioWorker looks the
    // device back up by exact string match. If PlatformAudio's names do not
    // match those strings exactly, the stored preference cannot be carried
    // over as-is and a migration is needed.
    for (const QString& n : r.recordingNames) {
        if (r.qtInputDescriptions.contains(n)) {
            ++r.exactInputNameMatches;
        }
    }

    // The SDK logs a warning at process exit if this is skipped. Noted
    // because the real transport will need the same teardown, and an FFI
    // singleton left running past main() is a crash-on-exit waiting to
    // happen.
    livekit::shutdown();
    return r;
}
