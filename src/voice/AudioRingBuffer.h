#pragma once

// Single-producer / single-consumer byte ring, for handing PCM between
// a CoreAudio render/input callback and the audio thread.
//
// Why this exists
// ---------------
// AudioWorker is shaped around Qt Multimedia: capture arrives as
// QIODevice::readyRead on the thread that opened the device, and
// playback is metered by QAudioSink::bytesFree(). A VoiceProcessingIO
// audio unit is shaped the opposite way — ONE callback-driven unit that
// pulls render frames and pushes input frames from a CoreAudio
// real-time thread that we do not own and must never block.
//
// This ring is the adapter between the two shapes, and it is the only
// thing the two threads share. The contract is strict and matches what
// a real-time audio callback can actually promise:
//
//   * EXACTLY ONE producer and EXACTLY ONE consumer per instance. The
//     capture ring is written by the input callback and read by the
//     audio thread; the playback ring is written by the audio thread
//     and read by the render callback. Neither is ever swapped around.
//   * No locks, no allocation and no system calls on either side, so
//     the CoreAudio thread can use it without risking a priority
//     inversion against the audio thread.
//   * reset() and the constructor are NOT thread safe. Both are called
//     with the unit stopped, which is the only moment there is no
//     other side.
//
// The capacity is rounded up to a power of two so the wrap is a mask
// rather than a modulo. Indices are free-running and never wrapped
// themselves, which is what makes "empty" and "full" distinguishable
// without wasting a slot; they are unsigned, so the difference stays
// correct across the (astronomically distant) wrap of the counter
// itself.
//
// Short reads and short writes are the normal case and are not errors:
// an overrun means the audio thread was late draining capture, an
// underrun means it was late filling playback. Both are counted so the
// log can say which one happened rather than just "audio was bad".

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace bsfchat::voice {

class AudioRingBuffer {
public:
    // `capacityBytes` is a minimum; the real capacity is the next power
    // of two at or above it, and at least 64 bytes.
    explicit AudioRingBuffer(int capacityBytes = 0) { resize(capacityBytes); }

    // Not thread safe. Only with both sides stopped.
    void resize(int capacityBytes)
    {
        uint32_t cap = 64;
        while (cap < static_cast<uint32_t>(capacityBytes > 0 ? capacityBytes : 0))
            cap <<= 1;
        m_buf.assign(cap, 0);
        m_mask = cap - 1;
        reset();
    }

    // Not thread safe. Only with both sides stopped.
    void reset()
    {
        m_write.store(0, std::memory_order_relaxed);
        m_read.store(0, std::memory_order_relaxed);
        m_overruns.store(0, std::memory_order_relaxed);
        m_underruns.store(0, std::memory_order_relaxed);
    }

    int capacity() const { return static_cast<int>(m_buf.size()); }

    // Readable bytes. Safe from either side; from the producer's side it
    // is a lower bound on what the consumer will find, and vice versa,
    // which is the direction each caller needs.
    int used() const
    {
        const uint32_t w = m_write.load(std::memory_order_acquire);
        const uint32_t r = m_read.load(std::memory_order_acquire);
        return static_cast<int>(w - r);
    }

    int free() const { return capacity() - used(); }

    // Producer side. Returns the number of bytes accepted, which is less
    // than `bytes` when the consumer has fallen behind. The overflow is
    // DROPPED rather than overwriting unread data: for capture that
    // discards the newest audio, which is the right end to lose — the
    // alternative reorders the stream.
    int write(const char* src, int bytes)
    {
        if (bytes <= 0) return 0;
        const int room = free();
        int n = bytes;
        if (n > room) {
            n = room;
            m_overruns.fetch_add(1, std::memory_order_relaxed);
        }
        if (n <= 0) return 0;

        const uint32_t w = m_write.load(std::memory_order_relaxed);
        const uint32_t start = w & m_mask;
        const int firstChunk = std::min<int>(n, capacity() - static_cast<int>(start));
        std::memcpy(m_buf.data() + start, src, static_cast<size_t>(firstChunk));
        if (n > firstChunk) {
            std::memcpy(m_buf.data(), src + firstChunk,
                        static_cast<size_t>(n - firstChunk));
        }
        // Release: the copy above must be visible before the consumer
        // can see the new index.
        m_write.store(w + static_cast<uint32_t>(n), std::memory_order_release);
        return n;
    }

    // Producer side. Same, with zeros — used when the unit needs a
    // render frame and the mixer has not produced one.
    int writeSilence(int bytes)
    {
        if (bytes <= 0) return 0;
        const int room = free();
        int n = std::min(bytes, room);
        if (n <= 0) return 0;
        const uint32_t w = m_write.load(std::memory_order_relaxed);
        const uint32_t start = w & m_mask;
        const int firstChunk = std::min<int>(n, capacity() - static_cast<int>(start));
        std::memset(m_buf.data() + start, 0, static_cast<size_t>(firstChunk));
        if (n > firstChunk)
            std::memset(m_buf.data(), 0, static_cast<size_t>(n - firstChunk));
        m_write.store(w + static_cast<uint32_t>(n), std::memory_order_release);
        return n;
    }

    // Consumer side. Returns the number of bytes delivered, which is
    // less than `bytes` when the producer has not kept up. The caller
    // decides what a short read means: for the render callback it means
    // fill the rest with silence and count an underrun.
    int read(char* dst, int bytes)
    {
        if (bytes <= 0) return 0;
        const int available = used();
        const int n = std::min(bytes, available);
        if (n <= 0) return 0;

        const uint32_t r = m_read.load(std::memory_order_relaxed);
        const uint32_t start = r & m_mask;
        const int firstChunk = std::min<int>(n, capacity() - static_cast<int>(start));
        std::memcpy(dst, m_buf.data() + start, static_cast<size_t>(firstChunk));
        if (n > firstChunk) {
            std::memcpy(dst + firstChunk, m_buf.data(),
                        static_cast<size_t>(n - firstChunk));
        }
        m_read.store(r + static_cast<uint32_t>(n), std::memory_order_release);
        return n;
    }

    // Consumer side. Throws away everything currently readable and
    // returns how much that was.
    //
    // This is what a consumer uses instead of reset() when the producer
    // is still live — closing capture while the audio unit stays up for
    // the echo canceller, for instance. reset() rewinds BOTH indices and
    // is only valid with both sides stopped; this only moves the index
    // this side owns, so it is safe against a concurrent writer.
    int discardAll()
    {
        const uint32_t w = m_write.load(std::memory_order_acquire);
        const uint32_t r = m_read.load(std::memory_order_relaxed);
        const int n = static_cast<int>(w - r);
        if (n > 0) m_read.store(w, std::memory_order_release);
        return n;
    }

    // Consumer side. Counts a render underrun; purely diagnostic.
    void noteUnderrun() { m_underruns.fetch_add(1, std::memory_order_relaxed); }

    uint32_t overruns() const { return m_overruns.load(std::memory_order_relaxed); }
    uint32_t underruns() const { return m_underruns.load(std::memory_order_relaxed); }

private:
    std::vector<char> m_buf;
    uint32_t m_mask = 0;
    std::atomic<uint32_t> m_write{0};
    std::atomic<uint32_t> m_read{0};
    std::atomic<uint32_t> m_overruns{0};
    std::atomic<uint32_t> m_underruns{0};
};

} // namespace bsfchat::voice
