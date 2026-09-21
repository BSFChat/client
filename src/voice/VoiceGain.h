#pragma once

// Level control for the voice pipeline: a speech AGC for the capture
// side and a look-ahead peak limiter for both directions.
//
// Why this exists (2026-09-21)
// ----------------------------
// "Audio volume is quiet for both of us testing at the minute, are we
// not offering 100% output volume?" We were: the playback QAudioSink
// ran at unity and the mixer summed peers without any divide-by-N. The
// quiet was upstream of all of that. The desktop capture path was a bare
// QAudioSource feeding opus_encode() — no automatic gain control, no
// limiter, nothing — so every sender arrived at whatever level their
// microphone happened to deliver, and a USB or laptop mic at speaking
// distance delivers conversational speech somewhere around -30 to
// -45 dBFS RMS. Every conferencing stack lifts that toward a common
// target; ours transmitted it raw. The owner's log shows the capture
// noise floor of his RØDE NT-USB+ at a peak |sample| of 5-94 out of
// 32767 (-76 to -51 dBFS peak) across two dozen joins, i.e. a clean,
// unamplified signal with nothing between it and the encoder.
// (Android was the exception: enterVoiceMode() puts the device into
// MODE_IN_COMMUNICATION, and the platform's voice processing applies
// its own gain control. See the Android note on SpeechAgc below.)
//
// Why self-contained rather than the WebRTC audio processing module
// ------------------------------------------------------------------
// The APM is the standard answer and would also bring echo
// cancellation, which this file does NOT provide. It was not adopted
// here because every route to it costs far more than this problem:
//
//   * The only copy already in the tree is inside the vendored LiveKit
//     SDK (livekit::AudioProcessingModule, deps/livekit-*/include). That
//     SDK is OFF by default (BSFCHAT_ENABLE_LIVEKIT), vendored for
//     macOS arm64 only, has no Android or iOS build at all, sits behind
//     a process-wide FFI singleton (livekit::initialize()), and is
//     ~23 MB of dylibs whose bundling cmake/LiveKit.cmake exists to keep
//     from repeating the aom.dll incident. Tying mesh voice to it would
//     make every desktop build depend on a transport the September plan
//     deliberately deferred.
//   * Standalone, the maintained extraction is freedesktop's
//     webrtc-audio-processing: Meson-only (every dependency here is
//     built from source through CMake FetchContent), requires
//     abseil-cpp, and would need a hand-maintained CMake port or an
//     ExternalProject Meson build for five targets including the
//     Android NDK and iOS toolchains. AEC3 alone is well over a hundred
//     translation units.
//   * Half-integrating it — AGC through the APM without feeding it the
//     far-end reference — would buy the dependency without the one
//     feature that justifies it.
//
// So: a small, deterministic, fully unit-tested AGC and limiter here,
// and echo cancellation stays an open gap. Anyone on speakers without
// headphones is still heard by the others with their own voice echoed
// back. Closing that needs an AEC fed with the exact far-end signal
// written to the sink (AudioWorker's mixed frame, before the device) and
// time-aligned against the capture — which is the APM's
// ProcessReverseStream()/ProcessStream() contract, and the reason to
// revisit the dependency question, most likely together with the
// LiveKit decision since libwebrtc brings AEC3 with it.
//
// Real-time rules
// ---------------
// Everything below runs on the audio thread once per 20 ms frame. No
// allocation after construction (every buffer is a fixed std::array),
// no locks, no logging, no Qt. Deterministic: the same input always
// produces the same output, which is what makes the tests meaningful.
//
// Signals are float, normalised so int16 full scale is 1.0 (the
// conversion helpers are at the bottom).

#include <array>
#include <cstdint>

namespace bsfchat::voice {

inline constexpr int kGainSampleRate = 48000;
inline constexpr int kGainFrameSamples = 960;  // 20 ms, AudioWorker::kFrameSamples

float dbToLinear(float db);
float linearToDb(float lin);  // floors at -120 dB for zero input

// Linear ramp from `from` to `to` across the buffer, so a gain that
// changes between frames never lands as a step (a step in gain is a
// click; a 20 ms ramp is inaudible). Sample i gets
// from + (to - from) * (i + 1) / n, so the last sample is exactly `to`
// and the next frame's ramp starts where this one ended.
void applyGainRamp(float* buf, int n, float from, float to);

// ---------------------------------------------------------------------
// PeakLimiter
// ---------------------------------------------------------------------
//
// A look-ahead brickwall limiter: |output| never exceeds the ceiling,
// by construction rather than by clamping, and a signal that never
// approaches the ceiling passes through bit-exact (merely delayed).
//
// It replaces the mixer's old std::clamp to +/-32767. A hard clamp is
// harmless while nothing reaches full scale, and until now almost
// nothing did — raw speech sat 30 dB below it. With capture now
// normalised and an output boost of up to +12 dB on offer, full scale is
// reached routinely, and a hard clamp there is flat-topped square-wave
// distortion, the harshest-sounding failure digital audio has.
//
// How the guarantee works: for every input sample the gain it would
// need is g = min(1, ceiling/|x|). The envelope is the minimum of g over
// a window of L samples, then averaged over another L samples, and the
// audio is delayed by L-1. Every term of that average is a window
// minimum that contains the sample being output, so the average — and
// therefore the applied gain — is at most the gain that sample needs.
// The averaging turns the gain dip into a smooth L-sample ramp instead
// of a step, and the release follower only ever makes the gain smaller,
// so it cannot break the bound either.
//
// Values, and why:
//   * L = 96 samples (2 ms). Long enough that the attack ramp is a
//     smooth fade rather than a click even on a full-scale transient;
//     short enough that the extra mouth-to-ear delay (1.98 ms) is
//     invisible next to the 40-200 ms jitter buffer and 100 ms sink.
//   * Ceiling -1 dBFS. Opus, like every lossy codec, can overshoot the
//     peaks of its input by a fraction of a dB when it reconstructs, so
//     the capture side keeps 1 dB of headroom for the far end's decoder.
//     The playback side uses the same value so that the final int16
//     conversion can never clip either.
//   * Release 80 ms. Slow enough that the gain does not follow the
//     individual cycles of a low voice (a 100 Hz fundamental is 10 ms per
//     cycle — a release in that range is audible as distortion), fast
//     enough that one plosive does not duck the following syllable.
class PeakLimiter {
public:
    static constexpr int kLookahead = 96;
    static constexpr float kDefaultCeiling = 0.891250938f;  // -1 dBFS
    static constexpr float kDefaultReleaseMs = 80.0f;

    explicit PeakLimiter(float ceiling = kDefaultCeiling,
                         float releaseMs = kDefaultReleaseMs,
                         int sampleRate = kGainSampleRate);

    void reset();

    // In place. Output lags input by latencySamples().
    void process(float* buf, int n);

    static constexpr int latencySamples() { return kLookahead - 1; }
    float ceiling() const { return m_ceiling; }
    // Smallest gain applied during the most recent process() call; 1.0
    // means the limiter did not touch that block.
    float lastBlockMinGain() const { return m_blockMinGain; }

private:
    float m_ceiling;
    float m_releaseCoef;

    // Delay line for the audio, and the history of per-sample window
    // minima that the boxcar average runs over. Both L long.
    std::array<float, kLookahead> m_delay{};
    std::array<float, kLookahead> m_minHistory{};
    double m_minSum = kLookahead;
    int m_pos = 0;

    // Monotonic deque (ascending values) over the last L required gains,
    // as two fixed ring arrays: O(1) amortised sliding minimum with no
    // allocation.
    std::array<float, kLookahead> m_dqVal{};
    std::array<int64_t, kLookahead> m_dqIdx{};
    int m_dqHead = 0;
    int m_dqCount = 0;
    int64_t m_sampleIndex = 0;

    float m_env = 1.0f;          // release follower, the applied gain
    float m_blockMinGain = 1.0f;
};

// ---------------------------------------------------------------------
// SpeechAgc
// ---------------------------------------------------------------------
//
// Frame-based automatic gain control for the outgoing microphone
// signal. Lifts (or trims) speech toward a common loudness so that each
// sender arrives at roughly the same level regardless of their mic, and
// does it without ever pumping the noise floor up between words.
//
// Per 20 ms frame:
//   1. Measure the frame's RMS (dBFS).
//   2. Track the noise floor as the minimum frame level over the last
//      1.5 s ("minimum statistics"). Speech is full of short dips between
//      syllables and words, so the minimum over a window that long is the
//      background, not the voice. Exactly-zero frames (a TCC-denied or
//      still-warming device) are not evidence of anything and are
//      skipped.
//   3. Call the frame speech only if it is at least 10 dB above that
//      floor AND above -65 dBFS absolute. Everything else — silence,
//      breaths, fan hum, the room between words — is NOT speech.
//   4. On speech frames only, update a smoothed speech level (power
//      average, 200 ms time constant) and move the gain toward
//      target - level at a bounded rate.
//   5. On non-speech frames HOLD the gain. This is the rule that stops
//      the AGC turning the noise floor into hiss: an AGC that chases
//      "whatever is there" raises its gain in every pause until the
//      background is at target loudness, and the listener hears the
//      hiss swell up between sentences and duck when you speak.
//   6. Independently of speech, never let the gain lift the measured
//      noise floor above -40 dBFS. If a fan starts or the user moves to a
//      noisier room the ceiling falls and the gain follows it down, even
//      in pauses.
//   7. Apply the new gain as a ramp from the previous frame's gain.
//
// Values, and why:
//   * Target -18 dBFS RMS for speech. Conversational speech has a crest
//     factor of roughly 12-18 dB, so -18 RMS puts ordinary peaks at
//     about -6..0 dBFS: as loud as it can be while leaving the limiter
//     to shave only the occasional plosive or laugh. This is in the
//     range broadcast and conferencing loudness targets land in for
//     speech-only programme.
//   * Gain range -10 dB .. +30 dB. +30 lifts a -48 dBFS speaker (a
//     laptop mic across a desk) to target; beyond that the source is
//     almost certainly the wrong device, and the noise floor would come
//     up with it. -10 tames a mic that is gained hot at the interface;
//     peaks beyond that are the limiter's job.
//   * Gain up 4 dB/s, or 12 dB/s while more than 6 dB short of target;
//     gain down 12 dB/s. Up is slow because it is the direction that
//     pumps: at 4 dB/s the gain moves less than 1 dB across a syllable.
//     The fast lane exists so a quiet speaker is not inaudible for their
//     first sentence — from the -40 dBFS case it converges in roughly
//     3 s of speech. Down is faster because being too loud is worse
//     than being too quiet, and the limiter covers the transient in the
//     meantime.
//   * 0.5 s warm-up. No decisions until 25 non-zero frames have been
//     seen, so the floor estimate is real before anything is lifted.
//
// Android: the platform's voice-communication processing (engaged by
// enterVoiceMode()) already runs its own AGC. Two AGCs in series fight —
// each one's gain change is the other's input level change — and the
// result pumps. AudioWorker therefore does not run this class on
// Android (see kPlatformVoiceProcessing there); the limiter still runs,
// because a limiter cannot pump.
class SpeechAgc {
public:
    struct Config {
        float targetDbfs = -18.0f;
        float maxGainDb = 30.0f;
        float minGainDb = -10.0f;
        float noiseCeilingDbfs = -40.0f;
        float speechMarginDb = 10.0f;
        float absoluteGateDbfs = -65.0f;
        float gainUpDbPerSec = 4.0f;
        float gainUpFastDbPerSec = 12.0f;
        float fastLaneDeficitDb = 6.0f;
        float gainDownDbPerSec = 12.0f;
        float levelTauMs = 200.0f;
        int warmupFrames = 25;
    };

    static constexpr int kFloorWindowFrames = 75;  // 1.5 s of 20 ms frames
    static constexpr float kDigitalSilenceDbfs = -100.0f;

    SpeechAgc();
    explicit SpeechAgc(const Config& cfg);

    void reset();

    // One frame, in place. `n` is normally kGainFrameSamples; the rate
    // constants assume 20 ms frames.
    void processFrame(float* buf, int n);

    float gainDb() const { return m_gainDb; }
    float noiseFloorDbfs() const { return m_floorDb; }
    float speechLevelDbfs() const { return m_levelDb; }
    bool lastFrameWasSpeech() const { return m_lastSpeech; }

    // ---- Diagnostics (read off the per-frame path) ----
    //
    // Raw, pre-gain level of every speech frame, binned by whole dB from
    // -100 to 0 dBFS. Fixed storage, so collecting it costs an increment.
    // This is how the next call answers "how quiet was the mic really",
    // which the log could not answer before: the only level it recorded
    // was the peak of the first five frames after join, i.e. the room.
    int64_t speechFrames() const { return m_speechFrames; }
    // Level below which `fraction` of the speech frames fall; -120 when
    // no speech has been seen.
    float rawSpeechPercentileDbfs(float fraction) const;

private:
    Config m_cfg;
    float m_frameSec = 0.02f;
    float m_levelAlpha = 0.0f;

    std::array<float, kFloorWindowFrames> m_floorWindow{};
    int m_floorCount = 0;
    int m_floorPos = 0;
    int m_validFrames = 0;

    float m_floorDb = -120.0f;
    double m_levelPow = 0.0;
    float m_levelDb = -120.0f;
    bool m_haveLevel = false;
    float m_gainDb = 0.0f;
    float m_appliedGain = 1.0f;  // linear gain the last frame ended on
    bool m_lastSpeech = false;

    std::array<int32_t, 101> m_rawHistogram{};
    int64_t m_speechFrames = 0;
};

// ---------------------------------------------------------------------
// int16 <-> float
// ---------------------------------------------------------------------
void int16ToFloat(const int16_t* in, float* out, int n);
// Rounds and saturates. With the limiter in front this never actually
// saturates; the clamp is there so that a future change upstream of it
// degrades to distortion rather than to wrapped (sign-flipped) samples.
void floatToInt16(const float* in, int16_t* out, int n);

} // namespace bsfchat::voice
