// AVAudioSession configuration for voice. See IosAudioSession.h for why
// this exists and what it deliberately does NOT do (echo cancellation).
//
// Compiled (2026-09-23, -DBSFCHAT_ENABLE_VOICE=ON for arm64-iphoneos) and
// RUN on a real iPhone 16 Pro Max the same day: voice calls carry audio in
// both directions and the microphone permission prompt appears at first
// join, which means the category/mode/activation here are doing their job.
//
// What is still unobserved is everything on the recovery path. Nothing in
// this file's interruption, route-change or media-services-reset handling
// has been seen to fire — a simulator cannot take a phone call, and the
// device session did not have one. The events are now CONSUMED (they were
// not before: see AudioEngine and voice/DarwinVoiceLifecycle.h), so the
// policy is testable; the notifications that drive it are not.
//
// Written without ARC. This project does not pass -fobjc-arc (nothing in
// CMakeLists.txt or the CI workflow sets CLANG_ENABLE_OBJC_ARC, and
// MacCameraCapturer.mm calls -release directly, which ARC forbids), so
// object ownership here goes through CFBridgingRetain/CFBridgingRelease —
// the one spelling Foundation defines under BOTH regimes, and the same
// one video/LatencyCriticalActivity.mm already uses. If ARC is ever turned
// on this file keeps working unchanged.

#include "voice/IosAudioSession.h"

#include <QLoggingCategory>
#include <QString>

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

// Default level matches the rest of src/voice/: quiet unless something
// is wrong, with the detail available via QT_LOGGING_RULES.
Q_LOGGING_CATEGORY(logIosAudio, "bsfchat.voice.session", QtWarningMsg)

namespace bsfchat::ios_audio {
namespace {

// Observer tokens from -addObserverForName:object:queue:usingBlock:, held
// as retained CFTypeRefs (see the ARC note at the top).
void* g_interruptionToken = nullptr;
void* g_routeChangeToken = nullptr;
void* g_resetToken = nullptr;

EventHandler g_handler = nullptr;
bool g_active = false;

void emitEvent(SessionEvent event)
{
    // The blocks below are registered against [NSOperationQueue mainQueue],
    // so this already runs on the main thread, which is the Qt main thread
    // under Qt for iOS. The handler contract in the header depends on that
    // and on nothing else.
    if (g_handler) g_handler(event);
}

void installObservers()
{
    if (g_interruptionToken) return;

    NSNotificationCenter* centre = [NSNotificationCenter defaultCenter];
    NSOperationQueue* main = [NSOperationQueue mainQueue];

    g_interruptionToken = const_cast<void*>(CFBridgingRetain(
        [centre addObserverForName:AVAudioSessionInterruptionNotification
                            object:nil
                             queue:main
                        usingBlock:^(NSNotification* note) {
            NSNumber* type = note.userInfo[AVAudioSessionInterruptionTypeKey];
            if (!type) return;
            if (type.unsignedIntegerValue == AVAudioSessionInterruptionTypeBegan) {
                // The OS has ALREADY stopped our audio units by the time
                // this arrives. Nothing to tear down at the CoreAudio
                // level; what matters is that the app stops pretending it
                // is still capturing.
                qCInfo(logIosAudio) << "interruption began";
                emitEvent(SessionEvent::InterruptionBegan);
                return;
            }
            NSNumber* options = note.userInfo[AVAudioSessionInterruptionOptionKey];
            const bool shouldResume =
                options
                && (options.unsignedIntegerValue
                    & AVAudioSessionInterruptionOptionShouldResume) != 0;
            qCInfo(logIosAudio) << "interruption ended, shouldResume=" << shouldResume;
            emitEvent(shouldResume ? SessionEvent::InterruptionEndedShouldResume
                                   : SessionEvent::InterruptionEndedNoResume);
        }]));

    g_routeChangeToken = const_cast<void*>(CFBridgingRetain(
        [centre addObserverForName:AVAudioSessionRouteChangeNotification
                            object:nil
                             queue:main
                        usingBlock:^(NSNotification* note) {
            NSNumber* reason = note.userInfo[AVAudioSessionRouteChangeReasonKey];
            const NSUInteger code = reason ? reason.unsignedIntegerValue : 0;
            // routeChangeToEvent() decides, and it swallows the two
            // reasons our own audio-unit activity provokes. Before it
            // existed this block forwarded everything, which meant
            // enterVoiceMode()'s own category change came back to us as
            // "the route changed" and drove an unbounded rebuild loop.
            // See the comment on routeChangeToEvent() in the header.
            SessionEvent event;
            if (!routeChangeToEvent(code, event)) {
                qCDebug(logIosAudio) << "route notification ignored, reason=" << code;
                return;
            }
            qCInfo(logIosAudio) << "route changed, reason=" << code;
            emitEvent(event);
        }]));

    g_resetToken = const_cast<void*>(CFBridgingRetain(
        [centre addObserverForName:AVAudioSessionMediaServicesWereResetNotification
                            object:nil
                             queue:main
                        usingBlock:^(NSNotification*) {
            // Everything in the process that touches audio is now invalid.
            qCWarning(logIosAudio) << "media services were reset";
            g_active = false;
            emitEvent(SessionEvent::MediaServicesReset);
        }]));
}

} // namespace

bool enterVoiceMode()
{
    @autoreleasepool {
        installObservers();
        if (g_active) return true;

        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError* error = nil;

        // playAndRecord + voiceChat + defaultToSpeaker. See the header for
        // why each of the three is load-bearing.
        //
        // No explicit Bluetooth option is passed: mode voiceChat implicitly
        // enables HFP routing, and the explicit constant for it
        // (AVAudioSessionCategoryOptionAllowBluetooth) is deprecated in
        // recent SDKs in favour of a renamed one, so naming it here would
        // trade a real behaviour for a deprecation warning and an SDK
        // version dependency.
        if (![session setCategory:AVAudioSessionCategoryPlayAndRecord
                             mode:AVAudioSessionModeVoiceChat
                          options:AVAudioSessionCategoryOptionDefaultToSpeaker
                            error:&error]) {
            qCWarning(logIosAudio) << "setCategory failed:"
                                  << QString::fromNSString(error.localizedDescription);
            return false;
        }

        // Both are REQUESTS, not settings — iOS grants what the hardware
        // and the current route allow and silently gives you something
        // else otherwise. AudioWorker is hard-wired to 48 kHz mono
        // (AudioWorker.h kSampleRate) and Qt's Darwin backend will insert
        // an AudioConverter if the session runs at another rate, so a
        // refusal costs CPU and latency rather than correctness. Logged
        // rather than treated as fatal for exactly that reason.
        if (![session setPreferredSampleRate:48000.0 error:&error]) {
            qCInfo(logIosAudio) << "preferred sample rate refused:"
                               << QString::fromNSString(error.localizedDescription);
        }
        // 20 ms, matching AudioWorker's kFrameSamples (960 @ 48 kHz), so
        // the backend's callback cadence lines up with one Opus frame.
        if (![session setPreferredIOBufferDuration:0.02 error:&error]) {
            qCInfo(logIosAudio) << "preferred IO buffer duration refused:"
                               << QString::fromNSString(error.localizedDescription);
        }

        if (![session setActive:YES error:&error]) {
            qCWarning(logIosAudio) << "setActive:YES failed:"
                                  << QString::fromNSString(error.localizedDescription);
            return false;
        }

        g_active = true;
        qCInfo(logIosAudio) << "session active; rate=" << session.sampleRate
                           << "ioBuffer=" << session.IOBufferDuration
                           << "inputs=" << session.isInputAvailable;
        return true;
    }
}

void exitVoiceMode()
{
    @autoreleasepool {
        if (!g_active) return;
        g_active = false;

        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError* error = nil;
        // NotifyOthersOnDeactivation is what un-ducks / resumes the music
        // app the join interrupted. Without it the user's podcast stays
        // paused after the call ends.
        if (![session setActive:NO
                    withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                          error:&error]) {
            qCWarning(logIosAudio) << "setActive:NO failed:"
                                  << QString::fromNSString(error.localizedDescription);
        }

        // Observers stay installed on purpose: MediaServicesWereReset is
        // worth hearing about whether or not a call is up, and reinstalling
        // them per join is a leak waiting to happen.
    }
}

void setEventHandler(EventHandler handler)
{
    g_handler = handler;
    installObservers();
}

bool isActive()
{
    return g_active;
}

} // namespace bsfchat::ios_audio
