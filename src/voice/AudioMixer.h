#pragma once

// Summing mixer for the voice receive path.
//
// This class used to double as a per-peer FIFO that it called a
// "jitter buffer" — it wasn't one. It drained exactly one frame per
// peer per tick, so any burst permanently added latency (it could
// never drain two frames to catch up) until a hard cap silently
// dropped the oldest audio. All buffering, reordering, loss
// concealment and latency recovery now live in JitterBuffer, and this
// class does one thing: sum N mono frames into one output frame.
//
// Levels (2026-09-21)
// -------------------
// The sum is still straight addition — no divide-by-N, which would
// make every voice quieter the more people joined. What changed is
// what happens at full scale. It used to be std::clamp to +/-32767:
// fine while raw, unnormalised speech sat 30 dB under it, harsh
// flat-topped clipping once anything reached it. Now that capture is
// normalised (SpeechAgc on the sender) and the output can be boosted to
// +12 dB, full scale is reached routinely, so the mix is accumulated in
// float, scaled by the per-source and master gains, and brought under
// -1 dBFS by a look-ahead PeakLimiter before the int16 conversion. See
// voice/VoiceGain.h for why the limiter's bound holds by construction.
//
// Every gain here is given as a (start, end) pair and ramped across the
// frame, so a volume slider dragged mid-call is a smooth fade rather
// than a zipper of 20 ms steps.
//
// Usage per output frame:
//     mixer.begin();
//     for (peer : peers) mixer.add(peerPcm, n, gainFrom, gainTo);
//     const auto& out = mixer.finish(masterFrom, masterTo);
//
// Not thread-safe; it holds per-frame scratch state and the limiter's
// delay line. Free of Qt, allocation-free after construction.

#include "voice/VoiceGain.h"

#include <cstdint>
#include <vector>

class AudioMixer {
public:
    static constexpr int kFrameSamples = 960; // 20ms at 48kHz

    explicit AudioMixer(int frameSamples = kFrameSamples);

    // Start a new output frame; zeroes the accumulator.
    void begin();

    // Add one source's PCM, scaled by a gain ramped from `gainFrom` to
    // `gainTo` across the frame. Samples beyond frameSamples() are
    // ignored; a short source is treated as zero-padded.
    void add(const int16_t* pcm, int samples, float gainFrom, float gainTo);
    // Constant gain, and unity.
    void add(const int16_t* pcm, int samples, float gain) {
        add(pcm, samples, gain, gain);
    }
    void add(const int16_t* pcm, int samples) { add(pcm, samples, 1.0f, 1.0f); }

    // Apply the master gain (ramped), limit, and convert to int16. The
    // reference is valid until the next begin(). Always runs the limiter,
    // even with no sources, so the tail of the previous frame still in
    // its 2 ms delay line is played out rather than dropped.
    const std::vector<int16_t>& finish(float masterFrom = 1.0f,
                                       float masterTo = 1.0f);

    int frameSamples() const { return m_frameSamples; }
    // Sources added since the last begin().
    int sourceCount() const { return m_sources; }
    // How hard the limiter worked on the last finish(): 1.0 = untouched.
    float lastLimiterMinGain() const { return m_limiter.lastBlockMinGain(); }

    // Drops the limiter's delay line. For a restart of the output
    // device, where the tail belongs to a stream that is gone.
    void resetLimiter() { m_limiter.reset(); }

private:
    int m_frameSamples;
    std::vector<float> m_accum;
    std::vector<int16_t> m_out;
    bsfchat::voice::PeakLimiter m_limiter;
    int m_sources = 0;
};
