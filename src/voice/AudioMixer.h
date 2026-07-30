#pragma once

// Summing mixer for the voice receive path.
//
// This class used to double as a per-peer FIFO that it called a
// "jitter buffer" — it wasn't one. It drained exactly one frame per
// peer per tick, so any burst permanently added latency (it could
// never drain two frames to catch up) until a hard cap silently
// dropped the oldest audio. All buffering, reordering, loss
// concealment and latency recovery now live in JitterBuffer, and this
// class does one thing: sum N mono int16 frames into one, with
// saturation.
//
// Usage per output frame:
//     mixer.begin();
//     for (peer : peers) mixer.add(peerPcm, n);
//     const auto& out = mixer.finish();
//
// Not thread-safe; it holds per-frame scratch state. Free of Qt so it
// can move to a dedicated audio thread with the rest of the pipeline.

#include <cstdint>
#include <vector>

class AudioMixer {
public:
    static constexpr int kFrameSamples = 960; // 20ms at 48kHz

    explicit AudioMixer(int frameSamples = kFrameSamples);

    // Start a new output frame; zeroes the accumulator.
    void begin();

    // Add one source's PCM. Samples beyond frameSamples() are ignored;
    // a short source is treated as zero-padded.
    void add(const int16_t* pcm, int samples);

    // Saturate the accumulator into int16 and return the mixed frame.
    // The reference is valid until the next begin().
    const std::vector<int16_t>& finish();

    int frameSamples() const { return m_frameSamples; }
    // Sources added since the last begin().
    int sourceCount() const { return m_sources; }

private:
    int m_frameSamples;
    // 64-bit accumulator: summing int16 sources can only overflow it
    // after ~2^47 peers, so the mix can never wrap regardless of how
    // many participants are in the channel. finish() then clamps
    // (saturates) rather than truncating, so a loud room distorts
    // instead of inverting phase.
    std::vector<int64_t> m_accum;
    std::vector<int16_t> m_out;
    int m_sources = 0;
};
