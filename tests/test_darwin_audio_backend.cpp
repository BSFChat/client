// Unit tests for the parts of the Darwin echo-cancellation work that can
// be decided without a sound card.
//
// WHAT IS NOT TESTED HERE, AND WHY
// --------------------------------
// The audio unit itself. There is no honest unit test for
// kAudioUnitSubType_VoiceProcessingIO: proving that echo cancellation
// works needs a loudspeaker and a microphone in the same room, and
// proving that the CoreAudio callbacks are correct needs CoreAudio to
// call them. A test that instantiated the unit would raise a microphone
// permission prompt on whatever machine ran it and would then assert
// almost nothing. So DarwinVpioBackend.mm is covered by review and by
// device testing, and this file does not pretend otherwise.
//
// What IS decidable without hardware is everything the .mm delegates to:
//
//   * the format plan — what to do when the unit hands back 44.1 kHz, or
//     stereo, or float, or something absurd;
//   * the ring buffer that adapts a real-time callback to a 10 ms pump,
//     including what it does when either side falls behind;
//   * the interruption state machine — the phone-call, headphone-unplug
//     and media-services-reset cases, which are exactly the ones nobody
//     will test by hand twice;
//   * which backend gets chosen, including the fallback paths.
//
// All four are pure headers with no Qt Multimedia and no CoreAudio in
// them, which is what makes this runnable on a headless CI box.

#include <QtTest>

#include "voice/AudioBackendSelect.h"
#include "voice/AudioRingBuffer.h"
#include "voice/DarwinVoiceLifecycle.h"
#include "voice/IosAudioSession.h"
#include "voice/VpioFormat.h"

#include <vector>

using namespace bsfchat::voice;
using SessionEvent = bsfchat::ios_audio::SessionEvent;

namespace {

PipelineAudioFormat pipeline()
{
    // AudioWorker's constants: 48 kHz, mono, 20 ms frames.
    return PipelineAudioFormat{48000.0, 1, 960};
}

StreamAudioFormat unitFmt(double rate, int channels,
                          SampleType type = SampleType::Int16)
{
    return StreamAudioFormat{rate, channels, type};
}

} // namespace

class DarwinAudioBackendTest : public QObject {
    Q_OBJECT

private slots:
    // ---- format negotiation ----

    // The expected case on both platforms: we ask the unit for the
    // pipeline's own format and it takes it. Nothing must be inserted —
    // an identity converter is still a copy and still latency.
    void unitAcceptingOurFormatNeedsNoConversion()
    {
        const FormatPlan p = planConversion(unitFmt(48000.0, 1), pipeline());
        QCOMPARE(p.action, FormatAction::Direct);
        QCOMPARE(p.resampleRatio, 1.0);
        QCOMPARE(p.unitFramesPerPipelineFrame, 960);
        QVERIFY(p.usable());
    }

    // A Mac whose device is locked to 44.1 kHz by another application.
    void fortyFourOneNeedsAConverter()
    {
        const FormatPlan p = planConversion(unitFmt(44100.0, 1), pipeline());
        QCOMPARE(p.action, FormatAction::Convert);
        QVERIFY(p.usable());
        QVERIFY(qFuzzyCompare(p.resampleRatio, 44100.0 / 48000.0));
        // One 20 ms pipeline frame is 882 unit frames, plus the phase
        // slack the comment in VpioFormat.h justifies.
        QCOMPARE(p.unitFramesPerPipelineFrame, 883);
    }

    // The case a speakerphone-echo fix has to survive, because plugging
    // in a headset is the first thing a user does about echo: a
    // Bluetooth HFP route drags the session down to 16 kHz.
    void bluetoothNarrowbandIsSupported()
    {
        const FormatPlan p = planConversion(unitFmt(16000.0, 1), pipeline());
        QCOMPARE(p.action, FormatAction::Convert);
        QVERIFY(p.usable());
        // Fewer unit frames than pipeline frames, so the scratch sizing
        // must not assume growth in one direction.
        QVERIFY(p.unitFramesPerPipelineFrame < 960);
        QVERIFY(p.unitFramesPerPipelineFrame >= 320);
    }

    void stereoInputNeedsAConverter()
    {
        const FormatPlan p = planConversion(unitFmt(48000.0, 2), pipeline());
        QCOMPARE(p.action, FormatAction::Convert);
        QVERIFY(p.usable());
        // Same rate, so the frame counts line up even though the
        // channel count does not.
        QCOMPARE(p.resampleRatio, 1.0);
    }

    void floatOutputNeedsAConverter()
    {
        const FormatPlan p =
            planConversion(unitFmt(48000.0, 1, SampleType::Float32), pipeline());
        QCOMPARE(p.action, FormatAction::Convert);
        QVERIFY(p.usable());
    }

    // The three ways the negotiation refuses. Each one has to send the
    // caller back to the Qt path rather than into a converter built on a
    // guess: a working call without echo cancellation beats a broken one
    // with it.
    void absurdFormatsAreRefused()
    {
        QCOMPARE(planConversion(unitFmt(0.0, 1), pipeline()).action,
                 FormatAction::Unsupported);
        QCOMPARE(planConversion(unitFmt(48000.0, 0), pipeline()).action,
                 FormatAction::Unsupported);
        QCOMPARE(planConversion(unitFmt(48000.0, 64), pipeline()).action,
                 FormatAction::Unsupported);
        QCOMPARE(
            planConversion(unitFmt(48000.0, 1, SampleType::Other), pipeline())
                .action,
            FormatAction::Unsupported);
        // 4 kHz against 48 kHz is 12x, past the ratio cap.
        QCOMPARE(planConversion(unitFmt(4000.0, 1), pipeline()).action,
                 FormatAction::Unsupported);
        // 384 kHz is 8x and inside the cap; 768 kHz is not.
        QVERIFY(planConversion(unitFmt(384000.0, 1), pipeline()).usable());
        QVERIFY(!planConversion(unitFmt(768000.0, 1), pipeline()).usable());
    }

    void ringCapacityCoversTheRequestedDepth()
    {
        const PipelineAudioFormat p = pipeline();
        QCOMPARE(p.frameBytes(), 1920);
        QCOMPARE(ringCapacityBytes(p, 8), 1920 * 8);
        // Never below two frames, whatever is asked for: a one-frame
        // ring is a ring that is either empty or full.
        QCOMPARE(ringCapacityBytes(p, 0), 1920 * 2);
        QCOMPARE(ringCapacityBytes(p, 1), 1920 * 2);
    }

    // ---- ring buffer ----

    void ringRoundTripsBytesInOrder()
    {
        AudioRingBuffer ring(1024);
        QVERIFY(ring.capacity() >= 1024);
        std::vector<char> in(300);
        for (int i = 0; i < 300; ++i) in[size_t(i)] = char(i & 0x7F);

        QCOMPARE(ring.write(in.data(), 300), 300);
        QCOMPARE(ring.used(), 300);

        std::vector<char> out(300, 0);
        QCOMPARE(ring.read(out.data(), 300), 300);
        QCOMPARE(ring.used(), 0);
        QVERIFY(std::equal(in.begin(), in.end(), out.begin()));
    }

    // The wrap is the only interesting case in a ring, so drive the
    // indices past the end and check the data still comes out in order.
    void ringSurvivesWrapping()
    {
        AudioRingBuffer ring(64);
        const int cap = ring.capacity();
        std::vector<char> block(24);
        std::vector<char> out(24);
        char seed = 0;
        // Several times round the buffer.
        for (int pass = 0; pass < 20; ++pass) {
            for (char& c : block) c = seed++;
            QCOMPARE(ring.write(block.data(), 24), 24);
            QCOMPARE(ring.read(out.data(), 24), 24);
            QVERIFY(std::equal(block.begin(), block.end(), out.begin()));
        }
        QCOMPARE(ring.used(), 0);
        QVERIFY(cap >= 64);
    }

    // The audio thread was late draining capture. The newest audio is
    // dropped rather than overwriting unread data, because overwriting
    // reorders the stream.
    void ringOverrunIsShortAndCounted()
    {
        AudioRingBuffer ring(64);
        std::vector<char> big(200, 'x');
        const int accepted = ring.write(big.data(), 200);
        QCOMPARE(accepted, ring.capacity());
        QCOMPARE(ring.overruns(), 1u);
        QCOMPARE(ring.used(), ring.capacity());
        QCOMPARE(ring.write(big.data(), 1), 0);
        QCOMPARE(ring.overruns(), 2u);
    }

    // The render callback wanted more than the pump had produced. A
    // short read, not a blocking wait and not stale data.
    void ringUnderrunIsAShortRead()
    {
        AudioRingBuffer ring(256);
        std::vector<char> in(40, 'a');
        ring.write(in.data(), 40);
        std::vector<char> out(100, 0);
        QCOMPARE(ring.read(out.data(), 100), 40);
        QCOMPARE(ring.read(out.data(), 100), 0);
    }

    void ringSilenceWritesZeros()
    {
        AudioRingBuffer ring(128);
        std::vector<char> out(16, 'z');
        QCOMPARE(ring.writeSilence(16), 16);
        QCOMPARE(ring.read(out.data(), 16), 16);
        for (char c : out) QCOMPARE(c, char(0));
    }

    void ringResetEmptiesIt()
    {
        AudioRingBuffer ring(128);
        std::vector<char> in(64, 'q');
        ring.write(in.data(), 64);
        ring.reset();
        QCOMPARE(ring.used(), 0);
        QCOMPARE(ring.free(), ring.capacity());
        QCOMPARE(ring.overruns(), 0u);
    }

    // ---- interruption lifecycle ----

    void nothingHappensBeforeTheCallStarts()
    {
        DarwinVoiceLifecycle lc;
        QCOMPARE(lc.state(), VoiceAudioState::Stopped);
        // A media services reset with no session of ours to rebuild.
        QCOMPARE(lc.onEvent(SessionEvent::MediaServicesReset),
                 VoiceAudioAction::None);
        QCOMPARE(lc.onEvent(SessionEvent::InterruptionBegan),
                 VoiceAudioAction::None);
        QCOMPARE(lc.state(), VoiceAudioState::Stopped);
    }

    // The headline case: a phone call arrives mid-voice and then ends.
    // The call must survive it, and the audio must come back by itself.
    void phoneCallSuspendsAndResumes()
    {
        DarwinVoiceLifecycle lc;
        QCOMPARE(lc.onStart(), VoiceAudioAction::Open);
        QVERIFY(lc.audioLive());

        QCOMPARE(lc.onEvent(SessionEvent::InterruptionBegan),
                 VoiceAudioAction::Suspend);
        QCOMPARE(lc.state(), VoiceAudioState::Interrupted);
        QVERIFY(!lc.audioLive());
        // Not a button: this one comes back on its own, and offering a
        // "resume" affordance for it would be offering a button that
        // presses itself.
        QVERIFY(!lc.needsUserResume());
        QVERIFY(QString::fromLatin1(lc.reason()).length() > 0);

        QCOMPARE(lc.onEvent(SessionEvent::InterruptionEndedShouldResume),
                 VoiceAudioAction::Resume);
        QVERIFY(lc.audioLive());
        QCOMPARE(QString::fromLatin1(lc.reason()), QString());
    }

    // The OS did not grant resumption — the user may still be on that
    // phone call. Reopening the microphone here would be the worst
    // thing this code could do.
    void noResumeGrantParksAndWaitsForTheUser()
    {
        DarwinVoiceLifecycle lc;
        lc.onStart();
        lc.onEvent(SessionEvent::InterruptionBegan);
        QCOMPARE(lc.onEvent(SessionEvent::InterruptionEndedNoResume),
                 VoiceAudioAction::None);
        QCOMPARE(lc.state(), VoiceAudioState::AwaitingUserResume);
        QVERIFY(lc.needsUserResume());
        QVERIFY(!lc.audioLive());

        // And nothing that arrives afterwards may un-park it except the
        // user. A Bluetooth device connecting during a phone call must
        // not restart our capture.
        QCOMPARE(lc.onEvent(SessionEvent::RouteChanged), VoiceAudioAction::None);
        QCOMPARE(lc.onEvent(SessionEvent::InterruptionEndedShouldResume),
                 VoiceAudioAction::None);
        QVERIFY(lc.needsUserResume());

        QCOMPARE(lc.onUserResumeRequested(), VoiceAudioAction::Resume);
        QVERIFY(lc.audioLive());
    }

    void aFailedResumeParksRatherThanRetrying()
    {
        DarwinVoiceLifecycle lc;
        lc.onStart();
        lc.onEvent(SessionEvent::InterruptionBegan);
        lc.onEvent(SessionEvent::InterruptionEndedShouldResume);
        QVERIFY(lc.audioLive());
        lc.onResumeFailed();
        QVERIFY(lc.needsUserResume());
        QVERIFY(!lc.audioLive());
    }

    // An ordinary route change — a speaker override, a new device
    // becoming the default — re-evaluates and carries on.
    void ordinaryRouteChangeReevaluates()
    {
        DarwinVoiceLifecycle lc;
        lc.onStart();
        QCOMPARE(lc.onEvent(SessionEvent::RouteChanged),
                 VoiceAudioAction::ReevaluateRoute);
        QVERIFY(lc.audioLive());
    }

    // Headphones pulled out. iOS convention — and a privacy
    // requirement — is to PAUSE, not to fall back to the built-in
    // speaker with a private conversation on it.
    void losingTheRouteDevicePauses()
    {
        DarwinVoiceLifecycle lc;
        lc.onStart();
        QCOMPARE(lc.onEvent(SessionEvent::RouteChangedDeviceLost),
                 VoiceAudioAction::Suspend);
        QCOMPARE(lc.state(), VoiceAudioState::AwaitingUserResume);
        QVERIFY(lc.needsUserResume());
    }

    void mediaServicesResetRebuildsALiveSession()
    {
        DarwinVoiceLifecycle lc;
        lc.onStart();
        QCOMPARE(lc.onEvent(SessionEvent::MediaServicesReset),
                 VoiceAudioAction::Rebuild);
        // Still Running: the rebuild is expected to succeed, and the
        // caller reports back through onResumeFailed() if it does not.
        QVERIFY(lc.audioLive());
    }

    // A reset while parked must not silently resume — but what we were
    // parked on no longer exists either, so the state has to be
    // refreshed rather than left alone.
    void mediaServicesResetWhileParkedStaysParked()
    {
        DarwinVoiceLifecycle lc;
        lc.onStart();
        lc.onEvent(SessionEvent::InterruptionBegan);
        lc.onEvent(SessionEvent::InterruptionEndedNoResume);
        QCOMPARE(lc.onEvent(SessionEvent::MediaServicesReset),
                 VoiceAudioAction::Suspend);
        QVERIFY(lc.needsUserResume());
    }

    void stoppingFromAnyStateClosesOnce()
    {
        DarwinVoiceLifecycle lc;
        lc.onStart();
        lc.onEvent(SessionEvent::InterruptionBegan);
        QCOMPARE(lc.onStop(), VoiceAudioAction::Close);
        QCOMPARE(lc.state(), VoiceAudioState::Stopped);
        QCOMPARE(lc.onStop(), VoiceAudioAction::None);
    }

    void repeatedInterruptionsDoNotRepeatTheSuspend()
    {
        DarwinVoiceLifecycle lc;
        lc.onStart();
        QCOMPARE(lc.onEvent(SessionEvent::InterruptionBegan),
                 VoiceAudioAction::Suspend);
        QCOMPARE(lc.onEvent(SessionEvent::InterruptionBegan),
                 VoiceAudioAction::None);
    }

    // ---- route-change filtering (the restart loop) ----
    //
    // The device defect of 2026-09-25: ten audio-unit rebuilds in eight
    // seconds, no audio captured for the whole session, and a frozen UI
    // on leaving the call. One cause — a rebuild moved the session's
    // route, the route notification asked for a rebuild, repeat.

    // The first link in the loop. enterVoiceMode() sets the category,
    // and activating the unit changes it again; forwarding that as "the
    // route changed" is what started it.
    void ourOwnCategoryChangeIsSwallowed()
    {
        SessionEvent e = SessionEvent::InterruptionBegan;
        QVERIFY(!bsfchat::ios_audio::routeChangeToEvent(
            static_cast<unsigned long>(
                bsfchat::ios_audio::RouteChangeReason::CategoryChange),
            e));
        // Untouched: the caller must not read it.
        QCOMPARE(e, SessionEvent::InterruptionBegan);
    }

    // Apple's own description is that the route did NOT change.
    void routeConfigurationChangeIsSwallowed()
    {
        SessionEvent e{};
        QVERIFY(!bsfchat::ios_audio::routeChangeToEvent(
            static_cast<unsigned long>(
                bsfchat::ios_audio::RouteChangeReason::RouteConfigurationChange),
            e));
    }

    void realRouteChangesStillGetThrough()
    {
        using R = bsfchat::ios_audio::RouteChangeReason;
        for (R r : {R::Unknown, R::NewDeviceAvailable, R::Override,
                    R::WakeFromSleep, R::NoSuitableRouteForCategory}) {
            SessionEvent e{};
            QVERIFY(bsfchat::ios_audio::routeChangeToEvent(
                static_cast<unsigned long>(r), e));
            QCOMPARE(e, SessionEvent::RouteChanged);
        }
        SessionEvent lost{};
        QVERIFY(bsfchat::ios_audio::routeChangeToEvent(
            static_cast<unsigned long>(R::OldDeviceUnavailable), lost));
        QCOMPARE(lost, SessionEvent::RouteChangedDeviceLost);
    }

    // A reason a future iOS invents must not be swallowed silently — the
    // handler's response is now cheap and idempotent, so passing it on
    // is the safe default.
    void anUnknownReasonIsPassedOn()
    {
        SessionEvent e{};
        QVERIFY(bsfchat::ios_audio::routeChangeToEvent(9999, e));
        QCOMPARE(e, SessionEvent::RouteChanged);
    }

    // ---- rebuild limiter ----

    void rebuildLimiterAllowsAHumanPaceOfChanges()
    {
        RebuildLimiter lim(3, 10000);
        // Plugging in headphones, then a headset an hour later.
        QVERIFY(lim.allow(0));
        QVERIFY(lim.allow(60000));
        QVERIFY(lim.allow(3600000));
        QVERIFY(!lim.exhausted());
    }

    // The observed failure, at its observed rate: one rebuild every
    // ~700 ms. The fourth inside the window must be refused, and it must
    // stay refused rather than recovering when the window rolls — a
    // backend that needed three rebuilds in ten seconds is not one to
    // keep.
    void rebuildLimiterStopsTheObservedLoop()
    {
        RebuildLimiter lim(3, 10000);
        QVERIFY(lim.allow(0));
        QVERIFY(lim.allow(700));
        QVERIFY(lim.allow(1400));
        QVERIFY(!lim.allow(2100));
        QVERIFY(lim.exhausted());
        // Still refused long after the window would have rolled.
        QVERIFY(!lim.allow(60000));
        QVERIFY(!lim.allow(600000));
    }

    void rebuildLimiterResetsForANewSession()
    {
        RebuildLimiter lim(3, 10000);
        for (int i = 0; i < 5; ++i) lim.allow(i * 100);
        QVERIFY(lim.exhausted());
        lim.reset();
        QVERIFY(!lim.exhausted());
        QVERIFY(lim.allow(0));
    }

    // Exactly at the boundary the window rolls and the count starts
    // again, so a slow trickle of genuine changes never exhausts it.
    void rebuildLimiterWindowRolls()
    {
        RebuildLimiter lim(2, 1000);
        QVERIFY(lim.allow(0));
        QVERIFY(lim.allow(500));
        // 1001 ms is outside the window opened at 0.
        QVERIFY(lim.allow(1001));
        QVERIFY(lim.allow(1200));
        QVERIFY(!lim.allow(1300));
    }

    // ---- backend selection ----

    void nonAppleAlwaysGetsQt()
    {
        const BackendSelection s = selectAudioBackend(false, true, false);
        QCOMPARE(s.kind, AudioBackendKind::Qt);
    }

    void appleDefaultsToVpio()
    {
        const BackendSelection s = selectAudioBackend(true, true, false);
        QCOMPARE(s.kind, AudioBackendKind::DarwinVpio);
    }

    void theUserSettingTurnsItOff()
    {
        const BackendSelection s = selectAudioBackend(true, false, false);
        QCOMPARE(s.kind, AudioBackendKind::Qt);
    }

    // Once a VPIO unit has failed to start, this run stays on Qt. The
    // alternative is an audible stutter on every rejoin for a unit that
    // will not get better by being asked again.
    void aFailureThisRunStaysDemoted()
    {
        const BackendSelection s = selectAudioBackend(true, true, true);
        QCOMPARE(s.kind, AudioBackendKind::Qt);
    }

    // Every branch says why, because the one log line explaining a
    // session's audio character is the whole point of carrying the
    // string around.
    void everySelectionExplainsItself()
    {
        for (bool compiled : {false, true}) {
            for (bool wants : {false, true}) {
                for (bool demoted : {false, true}) {
                    const BackendSelection s =
                        selectAudioBackend(compiled, wants, demoted);
                    QVERIFY(s.reason != nullptr);
                    QVERIFY(QString::fromLatin1(s.reason).length() > 0);
                }
            }
        }
    }
};

QTEST_APPLESS_MAIN(DarwinAudioBackendTest)
#include "test_darwin_audio_backend.moc"
