#include "voice/JitterBuffer.h"

#include <algorithm>
#include <cstring>

namespace bsfchat::voice {

JitterBuffer::JitterBuffer(int sampleRate, int channels, int frameSamples)
    : m_sampleRate(sampleRate)
    , m_channels(channels)
    , m_frameSamples(frameSamples)
{
    int err = OPUS_OK;
    m_decoder = opus_decoder_create(m_sampleRate, m_channels, &err);
    if (err != OPUS_OK) {
        if (m_decoder) {
            opus_decoder_destroy(m_decoder);
            m_decoder = nullptr;
        }
    }
    m_scratch.resize(static_cast<size_t>(m_frameSamples) * m_channels, 0);
}

JitterBuffer::~JitterBuffer() {
    if (m_decoder) {
        opus_decoder_destroy(m_decoder);
        m_decoder = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Ingest
// ---------------------------------------------------------------------------

JitterBuffer::PushResult JitterBuffer::pushPacket(const char* frame, int len) {
    if (!frame || len <= kHeaderBytes) return PushResult::Invalid;
    const auto* b = reinterpret_cast<const uint8_t*>(frame);
    // Big-endian uint16 sequence. Bytes 2..3 are the timestamp delta,
    // which is fully derivable from the sequence at a fixed 20ms frame
    // duration, so we don't use it — it stays on the wire for
    // compatibility and for a future variable-frame-size encoder.
    const uint16_t seq = static_cast<uint16_t>((b[0] << 8) | b[1]);
    return push(seq, b + kHeaderBytes, len - kHeaderBytes);
}

JitterBuffer::PushResult JitterBuffer::push(uint16_t seq,
                                            const uint8_t* payload,
                                            int len) {
    if (!m_decoder || !payload || len <= 0) return PushResult::Invalid;

    m_stats.received++;

    PushResult result = PushResult::Accepted;

    if (!m_synced) {
        // First packet of a talk spurt (or after an idle timeout):
        // anchor the playout cursor here and refill the buffer.
        hardReset(seq);
        m_synced = true;
        result = PushResult::Resynced;
    } else {
        const int d = seqDiff(seq, m_playoutSeq);
        if (d < 0) {
            // Arrived after we already played (or concealed) its slot.
            // Nothing useful to do with it; a deeper buffer would have
            // caught it, so nudge the target up.
            m_stats.late++;
            growTarget();
            return PushResult::TooLate;
        }
        if (d >= kCapacity) {
            // Sequence jumped clean out of the reorder window: peer
            // restarted its encoder, or we were starved for over a
            // second. Re-acquire rather than concealing a huge gap.
            m_stats.resyncs++;
            hardReset(seq);
            result = PushResult::Resynced;
        }
    }

    Slot& s = slotFor(seq);
    if (s.filled) {
        if (s.seq == seq) {
            m_stats.duplicates++;
            return PushResult::Duplicate;
        }
        // Stale entry from a previous epoch. The window invariant
        // (every buffered seq is within [playout, playout+kCapacity))
        // makes this unreachable in practice, but overwriting is the
        // safe response — the slot already counts toward m_filled, so
        // the count stays correct.
    } else {
        m_filled++;
    }

    const bool wasEmpty = (m_filled == 1 && !s.filled);
    s.filled = true;
    s.seq = seq;
    s.payload.assign(payload, payload + len);

    if (wasEmpty || seqDiff(seq, m_highestSeq) > 0) m_highestSeq = seq;

    if (m_haveArrival && seqDiff(seq, m_lastArrivalSeq) < 0) m_stats.reordered++;
    m_lastArrivalSeq = seq;
    m_haveArrival = true;

    return result;
}

// ---------------------------------------------------------------------------
// Playout
// ---------------------------------------------------------------------------

int JitterBuffer::spanAhead() const {
    if (m_filled == 0) return 0;
    const int d = seqDiff(m_highestSeq, m_playoutSeq);
    return d < 0 ? 0 : d + 1;
}

bool JitterBuffer::nextBufferedSeq(uint16_t& out) const {
    if (m_filled == 0) return false;
    for (int i = 0; i < kCapacity; ++i) {
        const uint16_t seq = static_cast<uint16_t>(m_playoutSeq + i);
        const Slot& s = m_slots[static_cast<size_t>(seq) % kCapacity];
        if (s.filled && s.seq == seq) {
            out = seq;
            return true;
        }
    }
    return false;
}

JitterBuffer::PopResult JitterBuffer::pop(int16_t* out) {
    const size_t frameValues = static_cast<size_t>(m_frameSamples) * m_channels;
    auto silence = [&]() {
        std::memset(out, 0, frameValues * sizeof(int16_t));
        return PopResult::Silence;
    };

    if (!out) return PopResult::Silence;
    if (!m_decoder || !m_synced) return silence();

    if (m_prebuffering) {
        if (m_filled < m_targetDepth) return silence();
        m_prebuffering = false;
    }

    // ---- Latency recovery -------------------------------------------------
    // A burst (or a scheduling stall on the sender) leaves the buffer
    // deeper than it needs to be, and a strict one-frame-per-tick
    // drain would hold that latency forever. Shed at most one frame
    // per pop while we're over budget: that catches up at 2x realtime
    // and spreads the discontinuity instead of dumping it all at once.
    if (spanAhead() > m_targetDepth + kDepthSlackFrames) {
        Slot& drop = slotFor(m_playoutSeq);
        if (drop.filled && drop.seq == m_playoutSeq) {
            // Decode into scratch and throw the audio away rather than
            // skipping the packet outright — keeps the Opus decoder's
            // internal state continuous across the drop.
            decode(drop.payload.data(),
                   static_cast<int>(drop.payload.size()),
                   m_scratch.data());
            releaseSlot(drop);
        }
        m_playoutSeq++;
        m_stats.dropped++;
    }

    // ---- The frame we owe the sound card ----------------------------------
    Slot& s = slotFor(m_playoutSeq);
    if (s.filled && s.seq == m_playoutSeq) {
        const int n = decode(s.payload.data(),
                             static_cast<int>(s.payload.size()),
                             out);
        releaseSlot(s);
        m_playoutSeq++;
        if (n < 0) {
            m_stats.decodeErrors++;
            m_consecutiveConceal = 0;
            return silence();
        }
        if (static_cast<size_t>(n) * m_channels < frameValues) {
            // Short frame (shouldn't happen with a 20ms encoder, but a
            // malformed packet can produce one) — pad with silence so
            // the sink always gets a whole frame.
            std::memset(out + static_cast<size_t>(n) * m_channels, 0,
                        (frameValues - static_cast<size_t>(n) * m_channels)
                            * sizeof(int16_t));
        }
        m_consecutiveConceal = 0;
        m_stats.decoded++;
        noteCleanFrame();
        return PopResult::Decoded;
    }

    // ---- Gap --------------------------------------------------------------
    uint16_t nextSeq = 0;
    if (nextBufferedSeq(nextSeq)) {
        // We hold a later packet, so this one is genuinely lost (or is
        // still in flight and will be discarded as late). Ask Opus to
        // conceal exactly one frame — this is what keeps the decoder in
        // sync; skipping the call is what produces clicks.
        const int n = decode(nullptr, 0, out);
        if (n < 0) {
            m_stats.decodeErrors++;
            std::memset(out, 0, frameValues * sizeof(int16_t));
        }
        m_stats.concealed++;
        m_consecutiveConceal++;
        m_cleanRun = 0;

        const int gap = seqDiff(nextSeq, m_playoutSeq);
        if (gap > kMaxConsecutiveConceal) {
            // Long hole. Concealing all of it would just play a second
            // of synthetic mush and pin that latency in place — jump
            // the cursor to the next real packet instead.
            m_stats.skipped += static_cast<uint64_t>(gap - 1);
            m_playoutSeq = nextSeq;
        } else {
            m_playoutSeq++;
        }
        return PopResult::Concealed;
    }

    // ---- Starved ----------------------------------------------------------
    m_stats.starved++;
    m_cleanRun = 0;
    if (m_consecutiveConceal < kMaxConsecutiveConceal) {
        const int n = decode(nullptr, 0, out);
        if (n < 0) {
            m_stats.decodeErrors++;
            std::memset(out, 0, frameValues * sizeof(int16_t));
        }
        m_stats.concealed++;
        m_consecutiveConceal++;
        m_playoutSeq++;
        // Deliberately NOT growTarget() here. An empty buffer is the
        // normal end of a talk spurt (the peer muted or stopped
        // sending), and inflating the playout delay every time someone
        // stops talking ratchets the target to its ceiling within a
        // few seconds of ordinary conversation. The honest "the buffer
        // was too shallow" signal is a packet arriving after its
        // playout instant, which push() already reacts to.
        return PopResult::Concealed;
    }

    // Nothing has arrived for 100ms. Stop advancing the cursor and go
    // idle: if we kept incrementing it, a peer that resumes talking
    // after a pause would be re-acquired at whatever offset the stall
    // happened to leave behind, and we'd hold that as permanent
    // latency. The next packet re-anchors playout from scratch.
    goIdle();
    return silence();
}

int JitterBuffer::decode(const uint8_t* payload, int len, int16_t* out) {
    if (!m_decoder) return OPUS_INVALID_STATE;
    // payload == nullptr, len == 0 is the documented Opus packet-loss
    // concealment invocation. frameSamples must be exactly the
    // duration of the missing audio and a multiple of 2.5ms — 960 @
    // 48kHz is 20ms, which satisfies both.
    return opus_decode(m_decoder, payload, static_cast<opus_int32>(len),
                       out, m_frameSamples, /*decode_fec=*/0);
}

// ---------------------------------------------------------------------------
// Bookkeeping
// ---------------------------------------------------------------------------

void JitterBuffer::releaseSlot(Slot& s) {
    if (!s.filled) return;
    s.filled = false;
    s.payload.clear(); // keeps capacity — steady state is allocation-free
    if (m_filled > 0) m_filled--;
}

void JitterBuffer::clearSlots() {
    for (auto& s : m_slots) {
        s.filled = false;
        s.payload.clear();
    }
    m_filled = 0;
}

void JitterBuffer::hardReset(uint16_t seq) {
    clearSlots();
    m_playoutSeq = seq;
    m_highestSeq = seq;
    m_prebuffering = true;
    m_consecutiveConceal = 0;
    m_cleanRun = 0;
    m_haveArrival = false;
    resetDecoder();
}

void JitterBuffer::goIdle() {
    clearSlots();
    m_synced = false;
    m_prebuffering = true;
    m_consecutiveConceal = 0;
    m_cleanRun = 0;
    m_haveArrival = false;
    resetDecoder();
}

void JitterBuffer::resetDecoder() {
    if (m_decoder) opus_decoder_ctl(m_decoder, OPUS_RESET_STATE);
}

void JitterBuffer::growTarget() {
    if (m_targetDepth < kMaxTargetFrames) m_targetDepth++;
    m_cleanRun = 0;
}

void JitterBuffer::noteCleanFrame() {
    if (++m_cleanRun >= kShrinkIntervalFrames) {
        m_cleanRun = 0;
        if (m_targetDepth > kMinTargetFrames) m_targetDepth--;
    }
}

} // namespace bsfchat::voice
