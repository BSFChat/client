#pragma once

#include <QtGlobal>

#include <cstdint>

// Turns a wrapping 32-bit media clock (the RTP timestamp at 90 kHz, or
// the lossless header's millisecond field) into monotonic microseconds.
// Serial-number arithmetic like JitterBuffer::seqDiff: a plain
// difference on the raw uint32 goes wrong across the wrap, which for
// 90 kHz RTP is 13 h 15 min into a call — long enough that no test would
// ever have caught it and short enough that a streaming evening does.
// Small backward steps (reordered frames) come out as small negative
// deltas rather than a 13-hour jump.
class MediaClockUnwrapper {
public:
    explicit MediaClockUnwrapper(qint64 ticksPerSecond)
        : m_ticksPerSecond(ticksPerSecond) {}

    qint64 toUs(uint32_t ts) {
        if (!m_have) {
            m_have = true;
            m_last = ts;
            m_ticks = 0;
        } else {
            m_ticks += qint64(int32_t(ts - m_last));
            m_last = ts;
        }
        // Offset so the first value is comfortably positive — callers
        // use negative to mean "no timestamp".
        return kBaseUs + m_ticks * 1'000'000 / m_ticksPerSecond;
    }

    static constexpr qint64 kBaseUs = qint64(1) << 40;

private:
    qint64 m_ticksPerSecond;
    bool m_have = false;
    uint32_t m_last = 0;
    qint64 m_ticks = 0;
};
