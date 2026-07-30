#pragma once

// Per-peer receive jitter buffer for the real-time Opus audio path.
//
// The audio data channel is deliberately unreliable and unordered
// (rtc::Reliability{ unordered = true, maxRetransmits = 0 } — see
// PeerConnectionManager). That is the right transport for realtime
// voice, but it means the receive side is responsible for:
//
//   * reordering packets that SCTP delivered out of sequence,
//   * detecting gaps and driving Opus packet-loss concealment so the
//     decoder stays in sync instead of clicking,
//   * absorbing network jitter behind a small playout delay,
//   * discarding packets that arrive after their playout instant,
//   * recovering the latency it accumulated during a burst.
//
// Design notes
// ------------
// Decoding happens at *playout* time, not at arrival time. That is
// what makes PLC correct: opus_decode(dec, NULL, 0, ...) has to be
// interleaved with the real frames in playout order, which is only
// knowable once the buffer has decided what the next frame is.
//
// Sequence numbers are 16-bit and wrap. Every comparison in here goes
// through seqDiff(), which is modular (RFC 3550 style) — a plain `<`
// on the raw uint16 is wrong across the 65535 -> 0 boundary and is the
// classic way this code breaks once a call runs past ~22 minutes.
//
// Threading
// ---------
// This class is deliberately free of Qt and of any GUI-thread
// dependency, and it is NOT internally synchronised: push() and pop()
// must be called from the same thread, or serialised by the caller.
//
// The ownership rule, which is not negotiable: **a JitterBuffer
// instance is touched only on the audio thread.** AudioWorker owns the
// per-peer map and is the sole caller of push()/pop(); it also creates
// and destroys them. The network side never sees a JitterBuffer at all
// — received packets are handed across the thread boundary through
// AudioPacketQueue and drained into push() at the top of each playback
// pump, immediately before the pops that consume them.
//
// That is why there is no mutex in here, and why adding one would be
// the wrong fix. pop() runs 50 times a second per peer and does an
// Opus decode inside; locking it would put the network thread in
// contention with the hard-realtime path for no benefit, when the
// alternative is one O(1) deque swap per pump. If you find yourself
// wanting to call push() from another thread, push to the queue
// instead.

#include <opus.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bsfchat::voice {

class JitterBuffer {
public:
    // Wire header written by AudioEngine::onMicDataReady():
    //   [0..1] big-endian uint16 sequence number
    //   [2..3] big-endian uint16 timestamp delta (seq * 20ms)
    // followed by the raw Opus packet.
    static constexpr int kHeaderBytes = 4;

    // Reorder window, in frames. 64 * 20ms = 1.28s — far wider than
    // any reordering worth waiting for, but it bounds the slot array
    // and gives the "stream jumped, resync" test a sane threshold.
    static constexpr int kCapacity = 64;

    // Playout depth bounds, in 20ms frames.
    static constexpr int kMinTargetFrames = 2;   // 40ms
    static constexpr int kMaxTargetFrames = 10;  // 200ms
    static constexpr int kInitialTargetFrames = 3; // 60ms

    // Extra frames tolerated above target before we start draining.
    static constexpr int kDepthSlackFrames = 3;

    // Longest run of consecutive concealed frames. Opus PLC is good
    // for a couple of frames and fades to noise well before this.
    static constexpr int kMaxConsecutiveConceal = 5; // 100ms

    // Clean pops required before the target depth is allowed to
    // shrink by one frame (250 * 20ms = 5s).
    static constexpr int kShrinkIntervalFrames = 250;

    enum class PushResult {
        Accepted,   // stored, will be played
        Duplicate,  // same sequence already buffered
        TooLate,    // playout has already passed this sequence
        Resynced,   // stream discontinuity; buffer was reset around it
        Invalid,    // malformed packet / no decoder
    };

    enum class PopResult {
        Decoded,    // a real packet was decoded
        Concealed,  // Opus PLC filled a gap
        Silence,    // prebuffering, idle, or decode failure
    };

    struct Stats {
        uint64_t received = 0;
        uint64_t duplicates = 0;
        uint64_t late = 0;       // arrived after their playout instant
        uint64_t reordered = 0;  // arrived out of order (but in time)
        uint64_t decoded = 0;
        uint64_t concealed = 0;
        uint64_t skipped = 0;    // frames jumped over on a long gap
        uint64_t dropped = 0;    // frames dropped to recover latency
        uint64_t starved = 0;    // pops with nothing buffered at all
        uint64_t resyncs = 0;    // stream discontinuities
        uint64_t decodeErrors = 0;
    };

    explicit JitterBuffer(int sampleRate = 48000,
                          int channels = 1,
                          int frameSamples = 960);
    virtual ~JitterBuffer();

    JitterBuffer(const JitterBuffer&) = delete;
    JitterBuffer& operator=(const JitterBuffer&) = delete;

    bool isValid() const { return m_decoder != nullptr; }

    // Feed one received packet. `payload`/`len` is the bare Opus
    // packet — the 4-byte header must already be stripped.
    PushResult push(uint16_t seq, const uint8_t* payload, int len);

    // Convenience wrapper that parses the 4-byte wire header.
    PushResult pushPacket(const char* frame, int len);

    // Produce exactly frameSamples() samples of mono PCM into `out`.
    // Always writes the full frame (silence when there is nothing to
    // play), so callers never have to branch on the return value for
    // correctness — it is informational.
    PopResult pop(int16_t* out);

    int frameSamples() const { return m_frameSamples; }
    int channels() const { return m_channels; }
    const Stats& stats() const { return m_stats; }

    // Current adaptive playout target, in frames.
    int targetDepth() const { return m_targetDepth; }
    // Packets currently held.
    int bufferedFrames() const { return m_filled; }
    // Distance from the playout cursor to the newest buffered packet,
    // in frames — i.e. how much latency the buffer is holding. 0 when
    // empty.
    int spanAhead() const;
    bool isSynced() const { return m_synced; }

    // Modular ("serial number arithmetic") comparison of two 16-bit
    // sequence numbers. Returns >0 if `a` is after `b`, <0 if before,
    // 0 if equal. Correct across the 65535 -> 0 wrap.
    static int seqDiff(uint16_t a, uint16_t b) {
        return static_cast<int>(static_cast<int16_t>(
            static_cast<uint16_t>(a - b)));
    }

protected:
    // Decode one frame into `out`. `payload == nullptr` requests
    // packet-loss concealment for exactly one frame. Returns the
    // number of samples produced, or a negative Opus error code.
    // Virtual purely so tests can drive the sequencing logic with a
    // marker codec instead of real Opus.
    virtual int decode(const uint8_t* payload, int len, int16_t* out);

private:
    struct Slot {
        bool filled = false;
        uint16_t seq = 0;
        std::vector<uint8_t> payload;
    };

    Slot& slotFor(uint16_t seq) {
        return m_slots[static_cast<size_t>(seq) % kCapacity];
    }

    // Find the next buffered sequence at or after the playout cursor.
    // Returns false when nothing is buffered.
    bool nextBufferedSeq(uint16_t& out) const;

    void releaseSlot(Slot& s);
    void clearSlots();
    // Forget everything and re-acquire the stream at `seq`.
    void hardReset(uint16_t seq);
    // Stop playing; the next arriving packet re-acquires the stream.
    void goIdle();
    void resetDecoder();

    void growTarget();
    void noteCleanFrame();

    int m_sampleRate;
    int m_channels;
    int m_frameSamples;

    OpusDecoder* m_decoder = nullptr;

    std::array<Slot, kCapacity> m_slots;
    int m_filled = 0;

    bool m_synced = false;        // have we acquired a playout cursor?
    bool m_prebuffering = true;   // filling up before playout starts
    uint16_t m_playoutSeq = 0;    // next sequence to play
    uint16_t m_highestSeq = 0;    // newest buffered sequence
    uint16_t m_lastArrivalSeq = 0;
    bool m_haveArrival = false;

    int m_targetDepth = kInitialTargetFrames;
    int m_consecutiveConceal = 0;
    int m_cleanRun = 0;

    std::vector<int16_t> m_scratch; // discard buffer for drained frames
    Stats m_stats;
};

} // namespace bsfchat::voice
