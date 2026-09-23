// AVAudioSession configuration for voice, called around voice join/leave.
//
// This is the iOS counterpart of AndroidAudioRouting.h, and it exists for
// the same reason: on both mobile platforms the OS has a global notion of
// "this app is in a call", and the capture device behaves completely
// differently depending on whether you have said so BEFORE opening it.
//
// On iOS the mechanism is AVAudioSession. An app that never touches it runs
// in the default category, which on iOS is SoloAmbient — a PLAYBACK-ONLY
// category. Recording is simply not permitted in it, so QAudioSource opens
// and delivers silence (or fails outright), and playback is muted by the
// ring/silent switch and killed by the screen lock. Everything about voice
// on iOS is downstream of getting this one call right:
//
//   category  playAndRecord   — the only category that records and plays
//   mode      voiceChat       — tells the OS this is two-way conversational
//                               audio: it picks the call-tuned signal path,
//                               implicitly allows Bluetooth HFP routing, and
//                               is the flag CallKit-less VoIP apps are
//                               expected to set
//   option    defaultToSpeaker— without it playAndRecord routes to the
//                               EARPIECE, which sounds broken for a
//                               Discord-shaped app (same trap as Android's
//                               MODE_NORMAL earpiece default)
//
// ---------------------------------------------------------------------
// WHAT THIS DOES NOT GIVE YOU: ECHO CANCELLATION
// ---------------------------------------------------------------------
// Setting mode voiceChat does NOT by itself engage Apple's acoustic echo
// canceller. The AEC lives in the voice-processing I/O audio unit
// (kAudioUnitSubType_VoiceProcessingIO), and you only get it if the audio
// unit doing the capture is that one.
//
// Qt's iOS backend does not use it. Verified against the Qt 6.10.3 iOS
// build shipped in this project — QtMultimedia's only AudioComponent
// description is, byte for byte:
//
//     61756f75 72696f63 6170706c    'auou' 'rioc' 'appl'
//     (kAudioUnitType_Output, kAudioUnitSubType_RemoteIO, Apple)
//
// RemoteIO is the raw path: no AEC, no noise suppression, no AGC. So an
// iOS build that goes through QAudioSource has the SAME speakerphone echo
// problem the desktop build has (see the open-gap note in VoiceGain.h),
// and on a phone held at arm's length it is much worse. Closing it needs
// a native capture path built on VoiceProcessingIO, which is a separate
// and much larger piece of work — docs/ios-voice.md, section 4.
//
// This file is deliberately only the session layer, because the session
// layer is correct and useful on its own and is a prerequisite for the
// native path too.
//
// Everything here is a no-op off iOS so callers don't need
// `#ifdef Q_OS_IOS` around their invocations — same rule as
// AndroidAudioRouting.h.
#pragma once

#include <QtGlobal>

namespace bsfchat::ios_audio {

// What the OS did to us while a call was up. Delivered to the handler
// registered with setEventHandler(), ALWAYS on the Qt main thread.
enum class SessionEvent {
    // A phone call, Siri, an alarm or another app took the session. Our
    // audio units have already been stopped by the OS; capture and
    // playback are dead until Resumed.
    InterruptionBegan,
    // The interruption ended AND the OS says we may resume. Note that an
    // interruption can end WITHOUT this (the user answered a call and
    // stayed in it, or another app kept the session) — in that case
    // nothing arrives and the call stays down until the user acts.
    InterruptionEndedShouldResume,
    // The interruption ended but the OS did not grant resumption.
    InterruptionEndedNoResume,
    // Headphones unplugged, Bluetooth connected/disconnected, speaker
    // override changed. The session is still ours; the devices behind it
    // changed. On iOS this does NOT surface as a QMediaDevices change —
    // Qt enumerates audio devices through AVCaptureDevice, which reports
    // one built-in microphone and nothing else — so this notification is
    // the ONLY route-change signal an iOS build gets.
    RouteChanged,
    // The media server died and restarted. Every audio object in the
    // process is now invalid; the session must be reconfigured from
    // scratch and every unit reopened. Rare, but it does happen.
    MediaServicesReset,
};

// Configure and activate the session for a voice call. Call this BEFORE
// opening QAudioSource/QAudioSink — the category and mode determine the
// signal path the unit is built on, and changing them afterwards does not
// re-plumb an already-open unit (same rule as Android's MODE_IN_COMMUNICATION).
//
// Idempotent. Returns false if the session could not be configured or
// activated, in which case the caller must NOT proceed with the join —
// on iOS a failed activation means guaranteed silence, not degraded audio.
#if defined(Q_OS_IOS)
bool enterVoiceMode();
#else
inline bool enterVoiceMode() { return true; }
#endif

// Deactivate the session and hand the route back to whatever was playing
// before (music, podcast). Idempotent. Must be called from the same thread
// as enterVoiceMode().
#if defined(Q_OS_IOS)
void exitVoiceMode();
#else
inline void exitVoiceMode() {}
#endif

// Register the callback that receives SessionEvents. Pass nullptr to clear.
// Set this up ONCE at startup, not per join: interruption notifications can
// arrive between enterVoiceMode() and the first frame.
//
// The handler runs on the Qt main thread. It must hop to the audio thread
// itself for anything touching AudioWorker.
using EventHandler = void (*)(SessionEvent);
#if defined(Q_OS_IOS)
void setEventHandler(EventHandler handler);
#else
inline void setEventHandler(EventHandler) {}
#endif

// True once enterVoiceMode() has succeeded and exitVoiceMode() has not run.
#if defined(Q_OS_IOS)
bool isActive();
#else
inline bool isActive() { return false; }
#endif

} // namespace bsfchat::ios_audio
