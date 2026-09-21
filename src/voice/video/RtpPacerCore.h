#pragma once

#include <QtGlobal>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <utility>

// The token-bucket arithmetic behind PacedRtpSender (PeerConnectionManager
// .cpp), with no clock, no thread and no libdatachannel types, so the
// schedule it produces can be asserted from a unit test with a fake clock
// and synthetic packets. PacedRtpSender owns one of these, feeds it
// steady_clock time, and calls drain() both when the packetizer hands it
// packets (outgoing()) AND on its own tick while anything is queued.
//
// ---- Why the tick matters (2026-09-21, "sharp but very choppy") -------
//
// The first pacer called the equivalent of drain() only from outgoing(),
// i.e. once per encoded access unit, and the bucket holds at most
// kMaxBurstSeconds of budget. So per frame interval it could release at
// most 50 ms worth of ceiling: throughput = ceiling × fps × 0.05, below
// the ceiling at every frame rate under 20, and whatever an IDR left
// queued drained one capture tick at a time. The receive-side worker's
// loopback probe measured it: at 30 fps / 4 Mbps every 150 KB IDR landed
// ~165 ms late with P-frames clumped behind it; at 5 fps 86 of 110 frames
// never arrived, because the backlog cap discarded what the pacer never
// got round to sending. test_video_send_rate drives this class both ways
// ("outgoing-only", the old schedule, and "ticked", the new one) and
// asserts the difference.
//
// `Item` is the packet type (rtc::message_ptr in production, anything in
// tests). Times are microseconds on any monotonic clock.
template <typename Item>
class RtpPacerCore {
public:
    // A pacer that can throttle below this would be a bug generator, not
    // a smoother.
    static constexpr int kMinCeilingKbps = 1000;
    // Bucket depth: how much may leave back to back. 50 ms of ceiling is
    // ~30 full packets at 6 Mbps — a P-frame leaves in one go, an IDR is
    // spread out.
    static constexpr double kMaxBurstSeconds = 0.05;
    // Hard bound on queued-but-unsent bytes, in seconds of budget. Must
    // comfortably hold a whole keyframe. A 1080p screen IDR is routinely
    // 150-400 KB (field: 133 KB and 293 KB at 7.7 Mbps), and at that
    // bitrate the old 0.25 s cap was ~250 KB — so the leading packets of
    // every large IDR were dropped as "oldest", the viewer could never
    // assemble a complete keyframe, asked for another, and lost that one
    // the same way: a share that never appears. The cap still bounds
    // latency on a misconfigured-high bitrate; it must never bite on a
    // single access unit.
    static constexpr double kMaxBacklogSeconds = 1.0;
    static constexpr double kMinBacklogBytes = 4.0 * 1024 * 1024;

    void setCeilingKbps(int kbps) {
        m_bytesPerSecond = double(std::max(kbps, kMinCeilingKbps)) * 1000.0 / 8.0;
    }
    double bytesPerSecond() const { return m_bytesPerSecond; }

    // Queue packets. Returns how many queued packets the backlog cap had
    // to discard (oldest first) to make room.
    quint64 push(qint64 nowUs, Item item, size_t bytes) {
        if (m_queue.empty()) m_headSinceUs = nowUs;
        m_backlogBytes += bytes;
        m_queue.push_back({std::move(item), bytes});
        const size_t cap = size_t(std::max(m_bytesPerSecond * kMaxBacklogSeconds,
                                           kMinBacklogBytes));
        quint64 dropped = 0;
        while (m_backlogBytes > cap && !m_queue.empty()) {
            m_backlogBytes -= m_queue.front().bytes;
            m_queue.pop_front();
            ++dropped;
        }
        if (dropped) m_headSinceUs = nowUs;
        m_dropped += dropped;
        return dropped;
    }

    // Refill the bucket for the time since the last call and send, in
    // order, while it allows (one packet may take it negative — the same
    // allowance the stock libdatachannel handler has). `send(Item&&)`
    // returns false to abandon the queue (the track closed).
    template <typename SendFn>
    void drain(qint64 nowUs, SendFn&& send) {
        if (m_started) {
            const double elapsed = double(std::max<qint64>(0, nowUs - m_lastUs)) / 1e6;
            m_budget = std::min(m_budget + elapsed * m_bytesPerSecond,
                                m_bytesPerSecond * kMaxBurstSeconds);
        } else {
            // First burst starts with a full bucket: a share's opening
            // IDR must not be held back behind an empty budget.
            m_budget = m_bytesPerSecond * kMaxBurstSeconds;
            m_started = true;
        }
        m_lastUs = nowUs;
        while (!m_queue.empty() && m_budget > 0) {
            Entry e = std::move(m_queue.front());
            m_queue.pop_front();
            m_backlogBytes -= e.bytes;
            m_budget -= double(e.bytes);
            m_headSinceUs = nowUs;
            if (!send(std::move(e.item))) { clear(); return; }
        }
    }

    void clear() {
        m_queue.clear();
        m_backlogBytes = 0;
    }

    bool empty() const { return m_queue.empty(); }
    size_t backlogBytes() const { return m_backlogBytes; }
    quint64 dropped() const { return m_dropped; }
    // How long the packet now at the head has been waiting (0 if empty).
    qint64 headWaitUs(qint64 nowUs) const {
        return m_queue.empty() ? 0 : nowUs - m_headSinceUs;
    }

private:
    struct Entry { Item item; size_t bytes; };
    std::deque<Entry> m_queue;
    double m_bytesPerSecond = kMinCeilingKbps * 1000.0 / 8.0;
    double m_budget = 0.0;
    size_t m_backlogBytes = 0;
    quint64 m_dropped = 0;
    qint64 m_lastUs = 0;
    qint64 m_headSinceUs = 0;
    bool m_started = false;
};
