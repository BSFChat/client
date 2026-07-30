// Unit tests for the voice receive jitter buffer's sequencing logic:
// reordering, gap detection / concealment, late + duplicate discard,
// uint16 sequence wraparound, and latency recovery.
//
// The tests subclass JitterBuffer and override decode() with a marker
// codec: each "packet" carries its own sequence number as its payload,
// and the override records what it was asked to decode. That makes the
// playout order directly observable without depending on Opus audio
// semantics. The real decode() (which calls opus_decode, including the
// NULL-payload PLC form) is exercised by the app, not asserted on here.

#include <QtTest>

#include "voice/JitterBuffer.h"

#include <algorithm>
#include <vector>

using bsfchat::voice::JitterBuffer;

namespace {

// Sentinel recorded when the buffer asked for packet-loss concealment.
constexpr int kPLC = -1;

class RecordingBuffer : public JitterBuffer {
public:
    RecordingBuffer() : JitterBuffer(48000, 1, 960) {}

    std::vector<int> played;  // marker value per pop, or kPLC

    // Push a packet whose payload encodes its own sequence number.
    PushResult send(uint16_t seq) {
        const uint8_t payload[3] = {
            static_cast<uint8_t>(seq >> 8),
            static_cast<uint8_t>(seq & 0xFF),
            0x00,  // Opus packets are never zero-length; pad to 3 bytes
        };
        return push(seq, payload, 3);
    }

    // Pop `n` frames, discarding the PCM.
    void drain(int n) {
        std::vector<int16_t> pcm(960);
        for (int i = 0; i < n; ++i) pop(pcm.data());
    }

    // Realtime cadence: one packet in, one frame out. Keeps the buffer
    // near its target depth instead of tripping latency recovery.
    void step(uint16_t seq) {
        send(seq);
        drain(1);
    }

    // Playout with trailing concealment removed. Once the test stops
    // feeding, the buffer legitimately conceals for a few frames before
    // idling — that tail is not what any of these tests are about.
    std::vector<int> playout() const {
        std::vector<int> v = played;
        while (!v.empty() && v.back() == kPLC) v.pop_back();
        return v;
    }

protected:
    int decode(const uint8_t* payload, int len, int16_t* out) override {
        std::fill(out, out + 960, int16_t{0});
        if (!payload) {
            played.push_back(kPLC);
        } else {
            Q_ASSERT(len >= 2);
            played.push_back((int(payload[0]) << 8) | int(payload[1]));
        }
        return 960;
    }
};

// Assert that every non-concealed frame in `v` is one greater than the
// previous, treating kPLC as "one frame, whatever it was".
void checkContiguous(const std::vector<int>& v) {
    QVERIFY(!v.empty());
    QVERIFY(v.front() != kPLC);
    uint16_t expect = static_cast<uint16_t>(v.front());
    for (int got : v) {
        if (got != kPLC) QCOMPARE(got, int(expect));
        expect = static_cast<uint16_t>(expect + 1);
    }
}

} // namespace

class TestJitterBuffer : public QObject {
    Q_OBJECT

private slots:
    void seqDiffIsModular();
    void inOrderPlaysInOrder();
    void reorderedPacketsArePlayedInSequence();
    void gapTriggersConcealment();
    void wraparoundIsHandled();
    void latePacketsAreDiscarded();
    void duplicatesAreDiscarded();
    void longGapSkipsRatherThanConcealingForever();
    void starvationEventuallyGoesIdle();
    void latencyExcursionIsRecovered();
    void hugeSequenceJumpResyncs();
};

// --- modular sequence comparison -------------------------------------------

void TestJitterBuffer::seqDiffIsModular() {
    QCOMPARE(JitterBuffer::seqDiff(5, 3), 2);
    QCOMPARE(JitterBuffer::seqDiff(3, 5), -2);
    QCOMPARE(JitterBuffer::seqDiff(7, 7), 0);
    // The whole point: across the wrap, 0 is *after* 65535.
    QCOMPARE(JitterBuffer::seqDiff(0, 65535), 1);
    QCOMPARE(JitterBuffer::seqDiff(65535, 0), -1);
    QCOMPARE(JitterBuffer::seqDiff(2, 65534), 4);
    QCOMPARE(JitterBuffer::seqDiff(65534, 2), -4);
    // A naive `a < b` would get every one of these backwards.
    QVERIFY(JitterBuffer::seqDiff(1, 65530) > 0);
}

// --- happy path -------------------------------------------------------------

void TestJitterBuffer::inOrderPlaysInOrder() {
    RecordingBuffer jb;
    QVERIFY(jb.isValid());

    for (uint16_t s = 0; s < 3; ++s) jb.send(s);   // prebuffer
    for (uint16_t s = 3; s < 20; ++s) jb.step(s);  // realtime cadence
    jb.drain(3);                                   // flush the buffer

    const auto v = jb.playout();
    QCOMPARE(int(v.size()), 20);
    QCOMPARE(v.front(), 0);
    checkContiguous(v);
    QCOMPARE(jb.stats().concealed, uint64_t(0));
    QCOMPARE(jb.stats().decoded, uint64_t(20));
}

void TestJitterBuffer::reorderedPacketsArePlayedInSequence() {
    RecordingBuffer jb;
    // Arrive badly out of order, all within the reorder window and all
    // before their playout instant.
    jb.send(0);
    jb.send(3);
    jb.send(1);
    for (uint16_t s : {2, 6, 4, 5, 7}) jb.step(s);
    jb.drain(3);

    QCOMPARE(jb.stats().reordered, uint64_t(2)); // 1 and 4 went backwards

    // Every packet arrived in time, so playout must be a clean 0..7
    // with no concealment despite the scrambled arrival order.
    const auto v = jb.playout();
    QCOMPARE(int(v.size()), 8);
    QCOMPARE(v.front(), 0);
    checkContiguous(v);
    QCOMPARE(jb.stats().concealed, uint64_t(0));
    QCOMPARE(jb.stats().late, uint64_t(0));
}

// --- loss -------------------------------------------------------------------

void TestJitterBuffer::gapTriggersConcealment() {
    RecordingBuffer jb;
    for (uint16_t s : {0, 1, 2}) jb.send(s);
    // 3 is never sent.
    for (uint16_t s : {4, 5, 6, 7}) jb.step(s);
    jb.drain(4);

    const auto v = jb.playout();
    QCOMPARE(jb.stats().concealed, uint64_t(1));

    // The concealed frame must sit exactly where 3 should have been —
    // that is what keeps the Opus decoder in sync rather than feeding
    // it 4 where it expected 3.
    const auto it = std::find(v.begin(), v.end(), kPLC);
    QVERIFY2(it != v.end(), "expected a PLC frame for the missing packet");
    QVERIFY(it != v.begin());
    QCOMPARE(*(it - 1), 2);
    QVERIFY(it + 1 != v.end());
    QCOMPARE(*(it + 1), 4);
    checkContiguous(v);
}

// --- wraparound -------------------------------------------------------------

void TestJitterBuffer::wraparoundIsHandled() {
    RecordingBuffer jb;
    // Straddle the 65535 -> 0 boundary, and reorder across it so a
    // naive integer comparison would both mis-sort and mis-classify
    // 0 as "before" 65534.
    jb.send(65533);
    jb.send(65535);
    jb.send(65534);
    for (uint16_t s : {uint16_t(1), uint16_t(0), uint16_t(2), uint16_t(3)}) {
        jb.step(s);
    }
    jb.drain(3);

    QCOMPARE(jb.stats().late, uint64_t(0));       // nothing may be judged late
    QCOMPARE(jb.stats().concealed, uint64_t(0));  // nothing may look lost
    QCOMPARE(jb.stats().resyncs, uint64_t(0));    // no false discontinuity

    const auto v = jb.playout();
    QCOMPARE(int(v.size()), 7);
    QCOMPARE(v.front(), 65533);
    checkContiguous(v);  // wraps through 0 via uint16 arithmetic
    QVERIFY(std::find(v.begin(), v.end(), 0) != v.end());
    QCOMPARE(v.back(), 3);
}

// --- discard policy ---------------------------------------------------------

void TestJitterBuffer::latePacketsAreDiscarded() {
    RecordingBuffer jb;
    for (uint16_t s : {0, 1, 2, 3, 4, 5}) jb.send(s);
    jb.drain(6);  // playout has advanced past the early sequences

    const size_t before = jb.played.size();
    QCOMPARE(jb.send(1), JitterBuffer::PushResult::TooLate);
    QCOMPARE(jb.stats().late, uint64_t(1));

    jb.drain(1);
    // The stale packet must never reach the decoder.
    for (size_t i = before; i < jb.played.size(); ++i) {
        QVERIFY(jb.played[i] != 1);
    }
    // ...and a late arrival is the signal that the playout delay was
    // too shallow, so the target must adapt upward.
    QVERIFY(jb.targetDepth() > JitterBuffer::kInitialTargetFrames);
}

void TestJitterBuffer::duplicatesAreDiscarded() {
    RecordingBuffer jb;
    jb.send(0);
    jb.send(1);
    QCOMPARE(jb.send(1), JitterBuffer::PushResult::Duplicate);
    QCOMPARE(jb.send(1), JitterBuffer::PushResult::Duplicate);
    QCOMPARE(jb.stats().duplicates, uint64_t(2));
    QCOMPARE(jb.bufferedFrames(), 2);

    jb.send(2);
    jb.drain(4);
    // 1 must be played exactly once.
    QCOMPARE(std::count(jb.played.begin(), jb.played.end(), 1), 1L);
}

// --- gap / starvation bounds ------------------------------------------------

void TestJitterBuffer::longGapSkipsRatherThanConcealingForever() {
    RecordingBuffer jb;
    for (uint16_t s : {0, 1, 2}) jb.send(s);
    // Big hole, then audio resumes well inside the reorder window.
    for (uint16_t s : {30, 31, 32}) jb.send(s);
    jb.drain(8);

    const auto v = jb.playout();
    const long plc = std::count(v.begin(), v.end(), kPLC);
    QVERIFY2(plc <= JitterBuffer::kMaxConsecutiveConceal,
             "a 27-frame hole must be skipped, not concealed frame by frame");
    QVERIFY(jb.stats().skipped > 0);
    QVERIFY(std::find(v.begin(), v.end(), 30) != v.end());
    QCOMPARE(v.back(), 32);
}

void TestJitterBuffer::starvationEventuallyGoesIdle() {
    RecordingBuffer jb;
    for (uint16_t s : {0, 1, 2, 3}) jb.send(s);
    jb.drain(4);
    QVERIFY(jb.isSynced());

    // Peer goes quiet. Concealment must be bounded, then the buffer
    // must un-sync so the next talk spurt re-anchors instead of
    // inheriting the stall as permanent latency.
    jb.drain(30);
    QVERIFY(!jb.isSynced());
    QCOMPARE(jb.bufferedFrames(), 0);
    QCOMPARE(jb.stats().concealed,
             uint64_t(JitterBuffer::kMaxConsecutiveConceal));
    // A silent peer is not evidence of jitter — the target must not
    // have ratcheted up just because someone stopped talking.
    QCOMPARE(jb.targetDepth(), JitterBuffer::kInitialTargetFrames);

    // Resuming at an arbitrary later sequence must be clean.
    const size_t mark = jb.played.size();
    for (uint16_t s : {900, 901, 902, 903, 904}) jb.send(s);
    jb.drain(5);
    QVERIFY(jb.played.size() > mark);
    QCOMPARE(jb.played[mark], 900);
}

// --- latency recovery -------------------------------------------------------

void TestJitterBuffer::latencyExcursionIsRecovered() {
    RecordingBuffer jb;
    // A burst dumps 40 frames (800ms) in at once. A one-frame-per-tick
    // FIFO would carry that 800ms of latency forever.
    for (uint16_t s = 0; s < 40; ++s) jb.send(s);
    QVERIFY(jb.spanAhead() >= 40);

    jb.drain(40);

    QVERIFY2(jb.spanAhead() <= JitterBuffer::kMaxTargetFrames
                                   + JitterBuffer::kDepthSlackFrames,
             "buffer must drain faster than realtime to shed excess latency");
    QVERIFY2(jb.stats().dropped > 0, "expected frames to be shed");
}

void TestJitterBuffer::hugeSequenceJumpResyncs() {
    RecordingBuffer jb;
    for (uint16_t s : {0, 1, 2, 3}) jb.send(s);
    jb.drain(4);

    const uint64_t resyncsBefore = jb.stats().resyncs;
    // Beyond the reorder window: peer restarted its encoder.
    jb.send(5000);
    QVERIFY(jb.stats().resyncs > resyncsBefore);
    QCOMPARE(jb.bufferedFrames(), 1);

    for (uint16_t s = 5001; s < 5006; ++s) jb.send(s);
    const size_t mark = jb.played.size();
    jb.drain(6);
    QVERIFY(jb.played.size() > mark);
    QCOMPARE(jb.played[mark], 5000);
}

QTEST_APPLESS_MAIN(TestJitterBuffer)
#include "test_jitter_buffer.moc"
