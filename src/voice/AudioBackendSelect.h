#pragma once

// Which audio backend the voice pipeline should open, decided as a
// value. Same split, same reason as AudioDevicePolicy.h and
// VpioFormat.h: the decision is testable, the opening is not.
//
// There are two backends (see AudioBackend.h):
//
//   Qt          QAudioSource + QAudioSink. What every platform used
//               until now, and what Windows, Linux and Android keep
//               using unchanged.
//   DarwinVpio  One kAudioUnitSubType_VoiceProcessingIO unit doing both
//               directions, on macOS and iOS. Acoustic echo
//               cancellation, noise suppression and automatic gain
//               control, applied by the OS.
//
// Why there is an off switch at all
// ---------------------------------
// Two independent reasons, and both of them matter:
//
//  1. VPIO APPLIES PROCESSING SOME USERS DO NOT WANT. Noise suppression
//     and AGC are tuned for speech on a phone. Someone on a good
//     microphone in a treated room — which describes a real part of
//     this product's users — will hear the AGC pumping and the noise
//     suppressor chewing the tails off words, and will be right to want
//     it off. "Echo cancellation" is therefore a preference, not a bug
//     fix to be forced.
//
//  2. IT IS NEW CODE IN THE MOST USER-VISIBLE PART OF THE APP. A
//     regression here is "nobody can hear me", reported by a user who
//     cannot tell us why. A switch that puts the pipeline back on the
//     path that has shipped for months is the difference between a bad
//     evening and a bad release.
//
// The switch is therefore a user setting (Settings::voiceProcessing,
// persisted as audio/voiceProcessing) AND a build flag
// (BSFCHAT_DARWIN_VPIO, which compiles the backend out entirely). The
// third route back to Qt needs no switch: a VPIO unit that fails to
// start demotes itself, once, for the life of the process.

#include <QtGlobal>

namespace bsfchat::voice {

enum class AudioBackendKind {
    Qt,
    DarwinVpio,
};

struct BackendSelection {
    AudioBackendKind kind = AudioBackendKind::Qt;
    // Why, for the one log line that explains the whole session's audio
    // character. A literal; never owned.
    const char* reason = "";

    bool operator==(const BackendSelection&) const = default;
};

// `vpioCompiledIn`  — BSFCHAT_DARWIN_VPIO was defined for this target.
// `userWantsIt`     — Settings::voiceProcessing, default true on Apple.
// `demotedThisRun`  — a VPIO unit already failed to start in this
//                     process. Once is enough: retrying per join turns
//                     one failure into an audible stutter on every
//                     rejoin, and nothing about a failed audio unit gets
//                     better by asking again a minute later.
inline BackendSelection selectAudioBackend(bool vpioCompiledIn,
                                           bool userWantsIt,
                                           bool demotedThisRun)
{
    if (!vpioCompiledIn)
        return {AudioBackendKind::Qt, "platform has no VoiceProcessingIO backend"};
    if (demotedThisRun)
        return {AudioBackendKind::Qt,
                "VoiceProcessingIO failed earlier this run; staying on Qt"};
    if (!userWantsIt)
        return {AudioBackendKind::Qt, "voice processing turned off in settings"};
    return {AudioBackendKind::DarwinVpio,
            "VoiceProcessingIO (echo cancellation, noise suppression, AGC)"};
}

} // namespace bsfchat::voice
