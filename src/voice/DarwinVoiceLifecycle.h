#pragma once

// What to DO about an AVAudioSession event, as a state machine that can
// be tested without a phone.
//
// IosAudioSession.h has defined the events since the voice branch, and
// docs/ios-voice.md §3 has the required behaviour in a table, but
// nothing consumed either: setEventHandler() was left unwired on
// purpose, because the recovery policy is a decision rather than a
// mechanism. This header is that decision written down.
//
// The three rules the table comes down to:
//
//  1. AN INTERRUPTION IS NOT A DISCONNECT. A phone call stops our audio
//     units; it does not stop the call. The peer connections, the
//     signalling and the roster all stay up, and only the devices go
//     away. Tearing the call down here would make "someone rang me" a
//     reason to be dropped from voice.
//
//  2. NEVER RESUME AUDIO BEHIND THE USER'S BACK. If the OS does not
//     grant resumption (AVAudioSessionInterruptionOptionShouldResume
//     absent) the user may still be on that phone call, and silently
//     reopening the microphone is the worst thing this code could do.
//     The state machine parks in AwaitingUserResume and waits to be
//     told. The same rule is why RouteChanged does nothing at all while
//     we are parked: a Bluetooth device connecting during a phone call
//     must not restart our capture.
//
//  3. UNPLUGGING HEADPHONES PAUSES. That is the iOS convention every
//     other app follows, and AVAudioSessionRouteChangeReasonOldDeviceUnavailable
//     is how you detect it. Falling back to the speaker instead means a
//     private conversation is suddenly on speakerphone in a train
//     carriage — which is a privacy failure, not a routing preference.
//
// Backgrounding is deliberately absent from this machine. The correct
// behaviour for an app with UIBackgroundModes: audio is to do NOTHING
// when it backgrounds — the call must survive the screen locking, which
// matches the deliberate Android policy in main.cpp. A state machine
// with a transition for it would invite someone to add one.
//
// Pure: no Qt Multimedia, no CoreAudio, no AudioWorker. It takes events
// and returns actions, and the caller (AudioEngine, on the GUI thread)
// performs them.

#include "voice/IosAudioSession.h"

namespace bsfchat::voice {

enum class VoiceAudioState {
    // No voice session. The only state from which onStart() does
    // anything.
    Stopped,
    // Devices open (or expected to be), audio flowing.
    Running,
    // The OS took the session. Our units are ALREADY stopped by the OS
    // by the time we hear about it; what remains is to stop pretending.
    Interrupted,
    // The interruption ended without a grant to resume, or the route
    // went away under us. Waiting for the user to ask.
    AwaitingUserResume,
};

enum class VoiceAudioAction {
    None,
    // Open the devices. Only from onStart().
    Open,
    // Close the devices and keep everything else — encoder, jitter
    // buffers, peer connections — alive.
    Suspend,
    // Reopen the devices on the existing pipeline.
    Resume,
    // Everything in the process that touches CoreAudio is invalid.
    // Tear the whole pipeline down and build it again, session
    // included. Partial recovery from a media-services reset is worse
    // than none.
    Rebuild,
    // The route changed under a live session. Re-run the device policy;
    // do not touch anything else.
    ReevaluateRoute,
    // Stop for good.
    Close,
};

class DarwinVoiceLifecycle {
public:
    VoiceAudioState state() const { return m_state; }

    // Whether the devices should be open right now. The UI reads this
    // to decide whether to claim the user is being heard.
    bool audioLive() const { return m_state == VoiceAudioState::Running; }

    // Whether the user has to do something to get audio back. Distinct
    // from !audioLive(): an Interrupted session comes back by itself
    // when the phone call ends, and offering a button for that would be
    // offering a button that presses itself.
    bool needsUserResume() const
    {
        return m_state == VoiceAudioState::AwaitingUserResume;
    }

    // Why we are down, for the banner. A literal; never owned, never
    // translated here.
    const char* reason() const { return m_reason; }

    VoiceAudioAction onStart()
    {
        if (m_state != VoiceAudioState::Stopped) return VoiceAudioAction::None;
        m_state = VoiceAudioState::Running;
        m_reason = "";
        return VoiceAudioAction::Open;
    }

    VoiceAudioAction onStop()
    {
        if (m_state == VoiceAudioState::Stopped) return VoiceAudioAction::None;
        m_state = VoiceAudioState::Stopped;
        m_reason = "";
        return VoiceAudioAction::Close;
    }

    // The user tapped "resume". Only meaningful while parked.
    VoiceAudioAction onUserResumeRequested()
    {
        if (m_state != VoiceAudioState::AwaitingUserResume)
            return VoiceAudioAction::None;
        m_state = VoiceAudioState::Running;
        m_reason = "";
        return VoiceAudioAction::Resume;
    }

    // The caller tried to bring the devices back and could not — on
    // iOS, typically because AVAudioSession refused to activate, which
    // means the user is still on the phone call that interrupted us.
    // Park rather than retry: a loop of failing activations is a loop of
    // log lines and a flat battery.
    void onResumeFailed()
    {
        m_state = VoiceAudioState::AwaitingUserResume;
        m_reason = "Paused — audio could not be restarted, tap to try again";
    }

    VoiceAudioAction onEvent(bsfchat::ios_audio::SessionEvent event)
    {
        using E = bsfchat::ios_audio::SessionEvent;

        // Nothing that happens to the system session matters while we
        // have no session of our own. Notably including a media
        // services reset: there is nothing of ours to rebuild.
        if (m_state == VoiceAudioState::Stopped) return VoiceAudioAction::None;

        switch (event) {
        case E::InterruptionBegan:
            if (m_state == VoiceAudioState::Interrupted)
                return VoiceAudioAction::None;
            m_state = VoiceAudioState::Interrupted;
            m_reason = "Paused — another app is using the microphone";
            // The OS stopped the units already; Suspend is what makes
            // our own bookkeeping agree with reality, and it is what
            // turns the "you are transmitting" indicator off.
            return VoiceAudioAction::Suspend;

        case E::InterruptionEndedShouldResume:
            // Only from Interrupted. An "ended" that arrives while we
            // are parked was answered by the user's decision not to
            // resume, and must not override it.
            if (m_state != VoiceAudioState::Interrupted)
                return VoiceAudioAction::None;
            m_state = VoiceAudioState::Running;
            m_reason = "";
            return VoiceAudioAction::Resume;

        case E::InterruptionEndedNoResume:
            if (m_state != VoiceAudioState::Interrupted)
                return VoiceAudioAction::None;
            m_state = VoiceAudioState::AwaitingUserResume;
            m_reason = "Paused — tap to resume microphone and audio";
            // Already suspended by InterruptionBegan. Nothing to do
            // beyond changing what the UI says.
            return VoiceAudioAction::None;

        case E::RouteChanged:
            // Rule 2: not while parked or interrupted.
            if (m_state != VoiceAudioState::Running) return VoiceAudioAction::None;
            return VoiceAudioAction::ReevaluateRoute;

        case E::RouteChangedDeviceLost:
            // Rule 3. Headphones out, or the Bluetooth headset walked
            // out of range.
            if (m_state != VoiceAudioState::Running) return VoiceAudioAction::None;
            m_state = VoiceAudioState::AwaitingUserResume;
            m_reason = "Paused — your headset was disconnected";
            return VoiceAudioAction::Suspend;

        case E::MediaServicesReset:
            // Every audio object in the process is invalid, including
            // the ones belonging to a session we had parked. Rebuild
            // only if the user had audio; otherwise stay parked, but
            // note that what we are parked on no longer exists.
            if (m_state == VoiceAudioState::Running) {
                m_reason = "";
                return VoiceAudioAction::Rebuild;
            }
            m_state = VoiceAudioState::AwaitingUserResume;
            m_reason = "Paused — audio was reset by the system, tap to resume";
            return VoiceAudioAction::Suspend;
        }
        return VoiceAudioAction::None;
    }

private:
    VoiceAudioState m_state = VoiceAudioState::Stopped;
    const char* m_reason = "";
};

} // namespace bsfchat::voice
