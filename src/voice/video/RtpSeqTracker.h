#pragma once

#include <cstdint>

// RTP sequence-number tracking with a small reorder window (S-4).
//
// The receive path treats a sequence gap as "this access unit is
// incomplete": the unit is dropped undecoded and the stream waits for a
// keyframe, because decoders error-conceal broken references, report
// success, and paint compounding corruption. That verdict is right for
// real loss and wrong for REORDERING, which any path with more than one
// route produces routinely — and the cost of being wrong is a dropped
// frame plus a keyframe request (an IDR is 10-20x a P-frame), which is
// exactly the feedback loop that makes a lossy WiFi link look "patchy".
//
// So a gap is no longer a verdict, it is a QUESTION held open for a
// couple of packets (or ~10 ms): if the missing sequence numbers turn
// up late, nothing was lost and nothing is dropped. If they do not, the
// verdict is the same as before, delayed by two packets.
//
// NOT a NACK implementation: nothing is retransmitted, and this holds
// no packets back. Retransmission is deferred pending the LiveKit
// decision (libwebrtc brings NACK/RTX for free if that lands).
//
// Deliberately free of Qt and libdatachannel so the whole state machine
// can be driven from a unit test with synthetic sequences.
class RtpSeqTracker {
public:
    struct Config {
        // Subsequent packets to wait before confirming a gap as loss.
        int reorderPackets = 2;
        // ...or this long, whichever comes first.
        int64_t reorderMs = 10;
        // A gap wider than this is not reordering by any plausible
        // mechanism — confirm immediately rather than track it.
        int maxTrackedMissing = 8;
    };

    RtpSeqTracker() = default;
    explicit RtpSeqTracker(const Config& cfg) : m_cfg(cfg) {
        if (m_cfg.maxTrackedMissing > kMaxMissing)
            m_cfg.maxTrackedMissing = kMaxMissing;
        if (m_cfg.maxTrackedMissing < 1) m_cfg.maxTrackedMissing = 1;
    }

    // Feed one received packet. Returns true exactly once per gap, at
    // the moment that gap is CONFIRMED lost.
    bool observe(uint16_t seq, int64_t nowMs) {
        if (!m_hasLast) {
            m_lastSeq = seq;
            m_hasLast = true;
            return false;
        }
        // Serial-number arithmetic: ahead == 0 is the next expected
        // packet; ahead in (0, 2^15) means packets went missing;
        // ahead >= 2^15 is an older packet arriving late.
        const uint16_t ahead = uint16_t(seq - uint16_t(m_lastSeq + 1));

        if (ahead >= 0x8000) {
            // Late arrival. If it fills a hole we are holding open,
            // that hole was reordering, not loss.
            const bool filled = eraseMissing(seq);
            if (m_gapOpen) {
                if (m_missingCount == 0) {
                    m_gapOpen = false;      // fully explained
                    return false;
                }
                if (!filled) ++m_packetsSinceGap;
                return confirmIfExpired(nowMs);
            }
            return false;
        }

        if (ahead == 0) {
            m_lastSeq = seq;
            if (!m_gapOpen) return false;
            ++m_packetsSinceGap;
            return confirmIfExpired(nowMs);
        }

        // A gap. A second gap while one is still open is more disorder
        // than reordering explains — stop waiting.
        if (m_gapOpen) {
            m_lastSeq = seq;
            closeGap();
            return true;
        }
        if (int(ahead) > m_cfg.maxTrackedMissing) {
            m_lastSeq = seq;
            closeGap();
            return true;
        }
        m_missingCount = 0;
        for (uint16_t i = 0; i < ahead; ++i)
            m_missing[m_missingCount++] = uint16_t(m_lastSeq + 1 + i);
        m_gapOpen = true;
        m_gapOpenedMs = nowMs;
        m_packetsSinceGap = 0;
        m_lastSeq = seq;
        return confirmIfExpired(nowMs);
    }

    // Confirms a gap whose window has expired without further packets.
    // Call once per delivered batch so a gap at the tail of a burst is
    // not held until the next one.
    bool poll(int64_t nowMs) {
        if (!m_gapOpen) return false;
        return confirmIfExpired(nowMs);
    }

    bool hasPendingGap() const { return m_gapOpen; }

private:
    bool confirmIfExpired(int64_t nowMs) {
        if (!m_gapOpen) return false;
        if (m_packetsSinceGap >= m_cfg.reorderPackets
            || nowMs - m_gapOpenedMs >= m_cfg.reorderMs) {
            closeGap();
            return true;
        }
        return false;
    }

    void closeGap() {
        m_gapOpen = false;
        m_missingCount = 0;
        m_packetsSinceGap = 0;
    }

    bool eraseMissing(uint16_t seq) {
        for (int i = 0; i < m_missingCount; ++i) {
            if (m_missing[i] != seq) continue;
            m_missing[i] = m_missing[m_missingCount - 1];
            --m_missingCount;
            return true;
        }
        return false;
    }

    static constexpr int kMaxMissing = 8;

    Config m_cfg;
    uint16_t m_lastSeq = 0;
    bool m_hasLast = false;
    bool m_gapOpen = false;
    uint16_t m_missing[kMaxMissing] = {};
    int m_missingCount = 0;
    int64_t m_gapOpenedMs = 0;
    int m_packetsSinceGap = 0;
};
