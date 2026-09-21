#pragma once

#include <QtGlobal>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <utility>

// Receive-side video PLAYOUT buffer: holds decoded pictures for a short,
// adaptive delay and releases them on the cadence their SENDER captured
// them at, instead of the cadence the network happened to deliver them.
//
// Why this exists
// ---------------
// "Streams are really good quality, but very choppy." Until this class,
// a decoded frame went to the screen the moment the decoder produced it,
// so every irregularity between the sender's capture clock and our
// decoder became visible motion judder:
//
//   * the sender's RTP pacer (PacedRtpSender) spreads a large IDR over
//     several frame intervals and queues the P-frames behind it, so the
//     frames after every keyframe arrive LATE and then IN A CLUMP;
//   * two GUI-thread hops on the way in (libdatachannel -> manager ->
//     engine) inherit whatever the message list or QML was doing;
//   * decode time varies (an IDR decodes several times slower than a P).
//
// Each of those shows as "freeze, then fast-forward". The loopback
// probe (tests/probe_video_cadence.cpp, numbers in its header) measured
// the first alone at ~165 ms per keyframe for a 30 fps share — with no
// network in the way at all. Decode time, by contrast, is 1-3 ms.
//
// The fix is the one every RTC stack uses: present by MEDIA time. Each
// frame carries its capture time (the RTP timestamp, which the sender
// derives from EncodedFrame::captureTimeUs; the lossless tier's header
// carries the same clock in ms). Frame i is shown at
//
//     due_i = mediaTime_i + offset
//
// so consecutive frames are shown exactly as far apart as they were
// captured, and `offset` is chosen just large enough that frames are
// (almost) never late for it.
//
// Choosing the offset
// -------------------
// With d_i = arrival_i - mediaTime_i (network transit plus the unknown
// clock offset between the two machines — never needs separating):
//
//   baseline B = windowed MIN of d  — the fastest a frame has made it
//                                     through recently;
//   jitter   J = windowed MAX of d, minus B — how much later than the
//                                     fastest recent frame the slowest
//                                     recent one was;
//   target   T = clamp(J + margin, 0, ceiling);
//   offset     = B + T.
//
// ADAPTIVE UNDER A USER CEILING, not a fixed delay. A fixed target set
// high enough for a bad Wi-Fi evening costs that latency on a clean LAN
// all day, and a fixed target set low is useless on the bad evening. The
// adaptive target sits at a few milliseconds when arrival is regular and
// grows only as far as measured jitter requires, never past what the
// user said smoothness is worth to them (Settings::videoSmoothingMs).
// Zero is not handled here at all: VideoReceivePipeline bypasses this
// class entirely, so "Off" is the pre-buffer code path, byte for byte.
//
// Grow instantly, shrink slowly. The frame whose lateness raised J is
// scheduled against the NEW offset — it is on time, and the hitch the
// viewer sees is the single pause while the buffer deepens, instead of
// the freeze-then-clump that repeats at every keyframe today. Shrinking
// is slewed (kSlewRatio): playback runs at most 5% fast to shed delay,
// which is invisible, rather than jumping ahead, which is a skip.
//
// Window length (kWindowUs = 12 s) is deliberately LONGER than the
// default keyframe interval (10 s, Settings::screenShareKeyframeSec).
// The periodic IDR is the most regular jitter source there is; with a
// shorter memory the buffer would forget it between keyframes and show
// every one of them as a hitch — the audio JitterBuffer's 5 s shrink
// interval is right for Opus frames and would be wrong here.
//
// Loss, keyframes and gaps
// ------------------------
// This buffer never WAITS for anything. Scheduling is by time, not by
// sequence: a frame that was lost upstream simply has no entry, the
// previous picture stays on screen for its slot, and the next frame is
// shown at its own due time. It cannot stall on a hole — the failure the
// audio buffer has to reason about (waiting on a sequence number that
// will never come) does not exist here by construction.
//
// Recovery from loss is NOT this class's job and must not be duplicated
// here: VideoReceivePipeline drops loss-suspect access units, waits for
// a keyframe and re-requests one (throttled, kKfRequestMinIntervalMs)
// over the reliable control channel; the sender also answers RTCP PLI.
// Frames that do not decode never reach push().
//
// A frame that arrives after its due time (jitter beyond the ceiling) is
// shown as soon as possible; if a NEWER frame is also already due, the
// older one is skipped — the viewer has already waited for it, and
// showing a stale picture late only makes the freeze longer.
//
// Clock drift
// -----------
// Sender and receiver clocks run at slightly different rates, so d_i
// creeps by tens of microseconds per second. A buffer anchored once
// would slowly fill (receiver fast) or drain into constant lateness
// (sender fast). B is a WINDOWED minimum, so it follows the drift: a
// falling d sets a new minimum immediately; a rising one lets old minima
// age out of the window. Either way the offset tracks it within one
// window, so the delay stays bounded forever (pinned by the drift test).
// A jump too large to be drift (kResyncUs — the sender restarted its
// capture clock, or a machine slept) re-anchors from scratch.
//
// Threading: none. Plain value type, touched by one thread (the
// pipeline's owning, GUI, thread). Times are microseconds from any
// monotonic clock the caller likes, which is what lets the tests drive
// it with a fake one.
template <typename Payload>
class VideoPlayoutBuffer {
public:
    // How long the window remembers a fast frame (baseline) and a slow
    // one (jitter). See "Window length" above.
    static constexpr qint64 kWindowUs = 12'000'000;
    // Headroom above measured jitter: the presentation timer's wake-up
    // error plus the render loop picking the frame up on its next
    // vsync. Small on purpose — it is paid on every frame.
    static constexpr qint64 kMarginUs = 5'000;
    // Until the windows hold real data, assume modest jitter. A share's
    // opening seconds are the burstiest part of it (the first IDR is the
    // largest access unit it will ever send, paced out behind the
    // P-frames that follow), and a first impression of stutter is the
    // report this class exists to answer. Shed after kStartupUs if the
    // measurements do not justify it.
    static constexpr qint64 kInitialTargetUs = 40'000;
    static constexpr qint64 kStartupUs = 3'000'000;
    // Fastest permitted catch-up while shrinking: 5% faster than real
    // time. At 30 fps that is 1.7 ms per frame — well under the 16.7 ms
    // a 60 Hz display quantises to anyway.
    static constexpr double kSlewRatio = 0.05;
    // Transit that moves by more than this at once is not jitter or
    // drift; the timeline was restarted. Far above any ceiling the
    // setting allows.
    static constexpr qint64 kResyncUs = 2'000'000;
    // Hard cap on held pictures. Decoded frames are big (a 1080p NV12
    // picture is 3 MB, 4K is 12 MB), and some decoders hand out frames
    // from a bounded pool. 400 ms at 60 fps is 24; past that the oldest
    // goes, whatever the timing maths says.
    static constexpr int kMaxQueued = 24;

    struct Stats {
        quint64 pushed = 0;
        quint64 presented = 0;
        quint64 skippedLate = 0;   // superseded by a newer due frame
        quint64 overflowed = 0;    // dropped by kMaxQueued
        quint64 resyncs = 0;
    };

    // The user's ceiling on added delay, in µs. Applies to frames pushed
    // from now on; lowering it also pulls the current offset down at
    // once (the user asked for less latency — not in 2 s, now).
    void setMaxDelayUs(qint64 us) { m_maxDelayUs = std::max<qint64>(0, us); }
    qint64 maxDelayUs() const { return m_maxDelayUs; }

    void push(Payload payload, qint64 mediaUs, qint64 nowUs) {
        ++m_stats.pushed;
        const qint64 d = nowUs - mediaUs;

        if (!m_anchored || std::llabs(d - baseline()) > kResyncUs
            || mediaUs + kResyncUs < m_lastMediaUs) {
            resync(nowUs);
        }

        // Baseline: windowed minimum of d (monotonic deque, front = min).
        while (!m_minWin.empty() && m_minWin.back().second >= d)
            m_minWin.pop_back();
        m_minWin.emplace_back(nowUs, d);
        while (m_minWin.front().first < nowUs - kWindowUs)
            m_minWin.pop_front();
        const qint64 b = m_minWin.front().second;

        // Jitter: spread between the slowest and fastest recent frame.
        // Kept as a windowed MAX of d rather than of (d - B) so that a
        // path that got permanently slower is not counted twice when
        // its old, fast minima age out of the baseline window.
        while (!m_maxWin.empty() && m_maxWin.back().second <= d)
            m_maxWin.pop_back();
        m_maxWin.emplace_back(nowUs, d);
        while (m_maxWin.front().first < nowUs - kWindowUs)
            m_maxWin.pop_front();
        m_jitterUs = m_maxWin.front().second - b;

        qint64 target = m_jitterUs + kMarginUs;
        if (nowUs < m_startupUntilUs)
            target = std::max(target, kInitialTargetUs);
        target = std::clamp<qint64>(target, 0, m_maxDelayUs);
        m_targetUs = target;

        const qint64 desired = b + target;
        if (!m_haveOffset || desired >= m_offsetUs) {
            m_offsetUs = desired;             // grow at once
            m_haveOffset = true;
        } else {
            // Shrink by at most kSlewRatio of the media time elapsed
            // since the previous frame.
            const qint64 dt = std::max<qint64>(0, mediaUs - m_lastMediaUs);
            const qint64 step = qint64(double(dt) * kSlewRatio);
            m_offsetUs = std::max(desired, m_offsetUs - step);
        }
        // Never hold more than the ceiling above the baseline, whatever
        // the slew is still working off. This one clamp IS the latency
        // promise: since d >= B, due - now = offset - d <= ceiling for
        // every frame. (A second, per-frame clamp used to sit below; it
        // could never bite, so it went — delayNeverExceedsTheCeiling
        // fails without this line.)
        m_offsetUs = std::min(m_offsetUs, b + m_maxDelayUs);
        m_lastMediaUs = mediaUs;

        qint64 due = mediaUs + m_offsetUs;
        // Presentation order is arrival order (decode order == display
        // order for every codec configuration we send: no B-frames).
        if (!m_queue.empty()) due = std::max(due, m_queue.back().dueUs);

        m_queue.push_back({std::move(payload), due, nowUs});
        while (int(m_queue.size()) > kMaxQueued) {
            m_queue.pop_front();
            ++m_stats.overflowed;
        }
    }

    // When the next held frame is due, or nullopt when nothing is held.
    std::optional<qint64> nextDueUs() const {
        if (m_queue.empty()) return std::nullopt;
        return m_queue.front().dueUs;
    }

    // The frame to show now, if any is due. When several are (the timer
    // woke late), the newest is returned and the rest are skipped.
    std::optional<Payload> popDue(qint64 nowUs) {
        if (m_queue.empty() || m_queue.front().dueUs > nowUs)
            return std::nullopt;
        while (m_queue.size() > 1 && m_queue[1].dueUs <= nowUs) {
            m_queue.pop_front();
            ++m_stats.skippedLate;
        }
        Entry e = std::move(m_queue.front());
        m_queue.pop_front();
        ++m_stats.presented;
        m_lastHeldUs = nowUs - e.arrivalUs;
        return std::move(e.payload);
    }

    // Forget timing and held frames (stream restarted, decoder rebuilt).
    void clear() {
        m_queue.clear();
        m_anchored = false;
        m_haveOffset = false;
        m_minWin.clear();
        m_maxWin.clear();
    }

    int queued() const { return int(m_queue.size()); }
    // Current adaptive target (µs of delay above the fastest frame).
    qint64 targetUs() const { return m_targetUs; }
    // Measured jitter: how much later than the fastest recent frame the
    // slowest recent one arrived.
    qint64 jitterUs() const { return m_jitterUs; }
    // How long the most recently presented frame was held after it
    // arrived — the latency this buffer is actually adding right now.
    qint64 lastHeldUs() const { return m_lastHeldUs; }
    const Stats& stats() const { return m_stats; }

private:
    struct Entry {
        Payload payload;
        qint64 dueUs = 0;
        qint64 arrivalUs = 0;
    };

    qint64 baseline() const {
        return m_minWin.empty() ? 0 : m_minWin.front().second;
    }

    void resync(qint64 nowUs) {
        if (m_anchored) ++m_stats.resyncs;
        m_anchored = true;
        m_haveOffset = false;
        m_minWin.clear();
        m_maxWin.clear();
        m_startupUntilUs = nowUs + kStartupUs;
        m_lastMediaUs = std::numeric_limits<qint64>::min() / 2;
        // Whatever is still held belongs to the old timeline and cannot
        // be ordered against the new one. Make it due now: the newest
        // is shown on the next pop, the rest are skipped.
        for (auto& e : m_queue) e.dueUs = nowUs;
    }

    qint64 m_maxDelayUs = 0;
    std::deque<Entry> m_queue;
    std::deque<std::pair<qint64, qint64>> m_minWin;   // (arrival, d)
    std::deque<std::pair<qint64, qint64>> m_maxWin;   // (arrival, d)
    bool m_anchored = false;
    bool m_haveOffset = false;
    qint64 m_offsetUs = 0;
    qint64 m_lastMediaUs = 0;
    qint64 m_startupUntilUs = 0;
    qint64 m_targetUs = 0;
    qint64 m_jitterUs = 0;
    qint64 m_lastHeldUs = 0;
    Stats m_stats;
};
