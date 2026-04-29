// Android audio mode + routing helpers called around voice join/leave.
//
// Android splits audio into playback "modes": MODE_NORMAL for music,
// MODE_IN_COMMUNICATION for VoIP. In MODE_NORMAL voice playback runs
// through the earpiece (too quiet for a speakerphone use-case) and
// echo cancellation isn't engaged; switching to MODE_IN_COMMUNICATION
// + explicitly enabling speakerphone gives us the expected "call"
// routing + hardware AEC/NS on most devices.
//
// We also request audio focus (AUDIOFOCUS_GAIN) so the OS pauses the
// user's music and re-raises it when we release. Without this, a
// user joining voice while Spotify is playing would hear both.
//
// Everything here is a no-op off Android so callers don't need
// `#ifdef Q_OS_ANDROID` around their invocations.
#pragma once

namespace bsfchat::audio_routing {

// Enter VoIP mode: set MODE_IN_COMMUNICATION, route to speakerphone,
// request AUDIOFOCUS_GAIN. Idempotent — calling twice is a cheap
// no-op after the first success.
void enterVoiceMode();

// Revert to MODE_NORMAL, drop speakerphone, abandon audio focus.
// Must be called from the same thread as enterVoiceMode().
void exitVoiceMode();

} // namespace bsfchat::audio_routing
