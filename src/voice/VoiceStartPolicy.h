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

namespace voice {

enum class StartRefusal {
    None,
    // Relay-only policy with no TURN server: libdatachannel would
    // discard host and srflx candidates and gather nothing at all.
    RelayOnlyNoTurn,
    // No usable capture or playback device, or AudioEngine::start failed.
    AudioUnavailable,
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
    case StartRefusal::None:
        break;
    }
    return QString();
}

} // namespace voice
