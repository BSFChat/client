#pragma once

// Whether a voice session can possibly work, decided BEFORE anything is
// started and expressed as a value rather than a log line.
//
// V-H4: `VoiceEngine::start` used to return true after `AudioEngine::start`
// had already failed, so a user whose microphone was unavailable (the
// first-run macOS permission prompt is the common case) joined the channel
// deaf and mute, with the failure reduced to a toast. Nothing unwound, the
// member poll kept the server-side heartbeat alive, and the ghost reaper
// therefore never removed them.
//
// Pure logic, in its own header, so the decision is testable without an
// audio device — which is the one thing a unit test must never touch.

#include <QString>
#include <QtGlobal>

namespace voice {

enum class StartRefusal {
    None,
    // Relay-only policy with no TURN server: libdatachannel would
    // discard host and srflx candidates and gather nothing at all.
    RelayOnlyNoTurn,
    // No usable capture or playback device, or AudioEngine::start failed.
    AudioUnavailable,
    // The OS says the microphone is off limits for this app. Distinct
    // from AudioUnavailable because the fix is somewhere else entirely:
    // the device is fine, the user has to grant access.
    MicrophoneDenied,
};

// `allowP2P` is the server's `allow_p2p`; `hasRelay` is whether the built
// ICE configuration contains at least one TURN server; `audioStarted` is
// AudioEngine::start()'s result.
inline StartRefusal evaluateStart(bool allowP2P, bool hasRelay,
                                  bool audioStarted)
{
    if (!allowP2P && !hasRelay) return StartRefusal::RelayOnlyNoTurn;
    if (!audioStarted) return StartRefusal::AudioUnavailable;
    return StartRefusal::None;
}

inline QString refusalMessage(StartRefusal refusal)
{
    switch (refusal) {
    case StartRefusal::RelayOnlyNoTurn:
        return QStringLiteral(
            "Voice is unavailable: this server requires relayed connections "
            "but has no TURN server configured. Ask the server administrator "
            "to configure TURN, or to allow peer-to-peer voice.");
    case StartRefusal::AudioUnavailable:
        return QStringLiteral(
            "Voice is unavailable: no working microphone or speaker was "
            "found. Check your audio devices and that BSFChat is allowed to "
            "use the microphone, then try again.");
    case StartRefusal::MicrophoneDenied:
        // One return per platform: a preprocessor conditional inside a
        // macro argument is ill-formed and MSVC rejects it (C2121).
        //
        // iOS is checked before macOS: Qt defines Q_OS_DARWIN on both, and
        // while Q_OS_MACOS is not defined on iOS today, ordering the
        // narrower platform first means this cannot silently start
        // showing an iPhone user the System Settings path.
#if defined(Q_OS_IOS)
        return QStringLiteral(
            "Microphone access is denied for BSFChat in Settings → Privacy & "
            "Security → Microphone. Turn it on there, then join again.");
#elif defined(Q_OS_MACOS)
        return QStringLiteral(
            "Microphone access is denied for BSFChat in System Settings → "
            "Privacy & Security → Microphone. Turn it on there, then join "
            "again.");
#elif defined(Q_OS_WIN)
        return QStringLiteral(
            "Microphone access is denied for BSFChat in Settings → Privacy & "
            "security → Microphone. Turn it on there, then join again.");
#else
        return QStringLiteral(
            "Microphone access is denied for BSFChat by the system. Grant it "
            "in your privacy settings, then join again.");
#endif
    case StartRefusal::None:
        break;
    }
    return QString();
}

// ---------------------------------------------------------------------
// Microphone permission (V-H4, second half)
// ---------------------------------------------------------------------
// The OS permission state, flattened away from QPermission so the rule
// below can be tested — a unit test cannot answer a TCC prompt, and on
// a machine with no permission backend at all (which is the bug that
// made this matter) Qt reports Denied for every query.
enum class MicPermission {
    Granted,
    // The user has never been asked. The prompt is modal to the OS, not
    // to us, so the request is fired and the join continues; a denial
    // then fails AudioEngine::start and unwinds it.
    Undetermined,
    // The OS refuses. Nothing downstream can recover this.
    Denied,
    // No permission support in this build (older Qt, or a platform
    // without the concept). Proceed as before.
    Unsupported,
};

enum class MicPermissionAction {
    Proceed,
    RequestThenProceed,
    // Refuse the join outright and tell the user where the switch is.
    //
    // This is the half that was missing: rc.7 logged "microphone
    // permission is denied in system settings" and joined anyway. The
    // user appeared in the channel, their mic captured pure silence
    // (peak |sample| = 30 on a live RØDE NT-USB+), the member poll kept
    // their server-side row alive so the ghost reaper never removed
    // them, and nothing in the UI said a word. Same shape as the V-H4
    // no-device case, same answer: refuse, and unwind the join.
    Refuse,
};

inline MicPermissionAction micPermissionAction(MicPermission status)
{
    switch (status) {
    case MicPermission::Denied:        return MicPermissionAction::Refuse;
    case MicPermission::Undetermined:  return MicPermissionAction::RequestThenProceed;
    case MicPermission::Granted:
    case MicPermission::Unsupported:   break;
    }
    return MicPermissionAction::Proceed;
}

} // namespace voice
