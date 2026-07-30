// Unit tests for AudioPacketQueue — the thread handoff that carries
// received voice packets from the network side into the audio thread.
//
// The whole reason this class exists is that JitterBuffer is not
// internally synchronised: it is touched only on the audio thread, and
// packets reach it through this queue. So the properties worth testing
// are exactly the ones AudioWorker relies on:
//
//   * nothing is lost below capacity,
//   * each producer's items come out in the order that producer pushed
//     them (per-peer sequence order is what the jitter buffer's
//     reordering window is sized against),
//   * a RemovePeer command cannot overtake, or be overtaken by, that
//     peer's audio,
//   * the capacity cap drops audio rather than growing without bound,
//     and never drops a command,
//   * all of the above hold while a consumer drains concurrently.
//
// Determinism note
// ----------------
// Interleaving *between* producer threads is genuinely nondeterministic
// and is deliberately not asserted on. What is asserted is per-producer
// FIFO order, exact totals, and payload integrity — all of which are
// guaranteed properties, not timing accidents. The concurrent tests are
// sized so that no drop is possible (capacity >= total items) and bound
// their wait with a deadline; a deadline expiry is a genuine hang, not
// flakiness.

#include <QtTest>
#include <QThread>
#include <QDeadlineTimer>

#include "voice/AudioPacketQueue.h"
#include "voice/JitterBuffer.h"

#include <atomic>
#include <deque>
#include <map>
#include <vector>

using bsfchat::voice::AudioPacketQueue;
using bsfchat::voice::JitterBuffer;

namespace {

constexpr int kProducers = 4;
constexpr int kItemsPerProducer = 5000;
constexpr int kTotalItems = kProducers * kItemsPerProducer;
// Frames pushed in the JitterBuffer integration test.
constexpr int kJbFrames = 4000;

// Generous: the work itself is a few million mutex acquisitions, which
// takes well under a second. Anything approaching this is a deadlock.
constexpr int kDeadlineMs = 30000;

QString peerName(int producer) {
    return QStringLiteral("@peer%1:example.org").arg(producer);
}

// A wire frame carrying `seq` in the 4-byte header, plus a payload that
// redundantly encodes it so corruption is detectable.
QByteArray makeFrame(uint16_t seq, int producer) {
    QByteArray f(8, '\0');
    f[0] = static_cast<char>((seq >> 8) & 0xFF);
    f[1] = static_cast<char>(seq & 0xFF);
    f[2] = static_cast<char>(((seq * 20) >> 8) & 0xFF);
    f[3] = static_cast<char>((seq * 20) & 0xFF);
    // Opus payload area, used here as a checkable marker.
    f[4] = static_cast<char>(0x7C);
    f[5] = static_cast<char>(producer & 0xFF);
    f[6] = static_cast<char>((seq >> 8) & 0xFF);
    f[7] = static_cast<char>(seq & 0xFF);
    return f;
}

// Pushes kItemsPerProducer frames for one peer, in order.
class Producer : public QThread {
public:
    Producer(AudioPacketQueue* q, int id) : m_q(q), m_id(id) {}
    void run() override {
        const QString peer = peerName(m_id);
        for (int i = 0; i < kItemsPerProducer; ++i) {
            m_q->push(AudioPacketQueue::Kind::Audio, peer,
                      makeFrame(static_cast<uint16_t>(i), m_id));
        }
    }
private:
    AudioPacketQueue* m_q;
    int m_id;
};

// Single producer for the JitterBuffer integration test.
class JbProducer : public QThread {
public:
    explicit JbProducer(AudioPacketQueue* q) : m_q(q) {}
    void run() override {
        for (int i = 0; i < kJbFrames; ++i)
            m_q->push(AudioPacketQueue::Kind::Audio, peerName(0),
                      makeFrame(static_cast<uint16_t>(i), 0));
    }
private:
    AudioPacketQueue* m_q;
};

} // namespace

class TestAudioHandoff : public QObject {
    Q_OBJECT
private slots:
    void emptyQueueDrainsToNothing();
    void preservesOrderSingleThreaded();
    void removePeerCannotOvertakeAudio();
    void capacityDropsAudioButNeverCommands();
    void clearDiscardsBacklog();
    void concurrentProducersLoseNothingAndKeepOrder();
    void concurrentDrainFeedsJitterBufferWithoutLoss();
};

void TestAudioHandoff::emptyQueueDrainsToNothing() {
    AudioPacketQueue q;
    std::deque<AudioPacketQueue::Item> out;
    out.push_back({});  // drain() must clear whatever was there
    q.drain(out);
    QVERIFY(out.empty());
    QCOMPARE(q.size(), 0);
    QCOMPARE(q.pushed(), uint64_t(0));
    QCOMPARE(q.dropped(), uint64_t(0));
}

void TestAudioHandoff::preservesOrderSingleThreaded() {
    AudioPacketQueue q;
    for (int i = 0; i < 100; ++i)
        QVERIFY(q.push(AudioPacketQueue::Kind::Audio, peerName(0),
                       makeFrame(static_cast<uint16_t>(i), 0)));
    QCOMPARE(q.size(), 100);

    std::deque<AudioPacketQueue::Item> out;
    q.drain(out);
    QCOMPARE(int(out.size()), 100);
    QCOMPARE(q.size(), 0);  // drain empties it
    for (int i = 0; i < 100; ++i) {
        QCOMPARE(out[size_t(i)].peerId, peerName(0));
        QCOMPARE(out[size_t(i)].data, makeFrame(static_cast<uint16_t>(i), 0));
    }
}

void TestAudioHandoff::removePeerCannotOvertakeAudio() {
    // The bug this rules out: a peer leaves, the GUI thread issues the
    // removal, and it is applied *before* packets that were already
    // queued for that peer — which then recreate the jitter buffer for
    // a peer that is gone.
    AudioPacketQueue q;
    const QString peer = peerName(0);
    for (int i = 0; i < 5; ++i)
        q.push(AudioPacketQueue::Kind::Audio, peer, makeFrame(uint16_t(i), 0));
    q.push(AudioPacketQueue::Kind::RemovePeer, peer);
    for (int i = 5; i < 10; ++i)
        q.push(AudioPacketQueue::Kind::Audio, peer, makeFrame(uint16_t(i), 0));

    std::deque<AudioPacketQueue::Item> out;
    q.drain(out);
    QCOMPARE(int(out.size()), 11);
    for (int i = 0; i < 5; ++i)
        QCOMPARE(out[size_t(i)].kind, AudioPacketQueue::Kind::Audio);
    QCOMPARE(out[5].kind, AudioPacketQueue::Kind::RemovePeer);
    for (int i = 6; i < 11; ++i)
        QCOMPARE(out[size_t(i)].kind, AudioPacketQueue::Kind::Audio);
}

void TestAudioHandoff::capacityDropsAudioButNeverCommands() {
    constexpr int kCap = 8;
    AudioPacketQueue q(kCap);
    for (int i = 0; i < kCap; ++i)
        QVERIFY(q.push(AudioPacketQueue::Kind::Audio, peerName(0),
                       makeFrame(uint16_t(i), 0)));
    QCOMPARE(q.size(), kCap);
    QCOMPARE(q.dropped(), uint64_t(0));

    // Full: further audio is rejected and counted, not silently kept.
    QVERIFY(!q.push(AudioPacketQueue::Kind::Audio, peerName(0),
                    makeFrame(uint16_t(kCap), 0)));
    QCOMPARE(q.size(), kCap);
    QCOMPARE(q.dropped(), uint64_t(1));
    QCOMPARE(q.pushed(), uint64_t(kCap));

    // A command must get through regardless — dropping one leaks a
    // JitterBuffer and its Opus decoder.
    QVERIFY(q.push(AudioPacketQueue::Kind::RemovePeer, peerName(0)));
    QCOMPARE(q.size(), kCap + 1);
    QCOMPARE(q.dropped(), uint64_t(1));

    // And the surviving items are still the *oldest* kCap, in order,
    // followed by the command.
    std::deque<AudioPacketQueue::Item> out;
    q.drain(out);
    QCOMPARE(int(out.size()), kCap + 1);
    for (int i = 0; i < kCap; ++i)
        QCOMPARE(out[size_t(i)].data, makeFrame(uint16_t(i), 0));
    QCOMPARE(out[kCap].kind, AudioPacketQueue::Kind::RemovePeer);
}

void TestAudioHandoff::clearDiscardsBacklog() {
    AudioPacketQueue q;
    for (int i = 0; i < 20; ++i)
        q.push(AudioPacketQueue::Kind::Audio, peerName(0), makeFrame(uint16_t(i), 0));
    q.clear();
    QCOMPARE(q.size(), 0);
    // Counters are cumulative, not reset by clear().
    QCOMPARE(q.pushed(), uint64_t(20));
    std::deque<AudioPacketQueue::Item> out;
    q.drain(out);
    QVERIFY(out.empty());
}

void TestAudioHandoff::concurrentProducersLoseNothingAndKeepOrder() {
    // Four producers push concurrently while this thread drains in a
    // tight loop — the same shape as N libdatachannel peers feeding the
    // audio thread's pump. Capacity is set above the total so a drop is
    // impossible and "nothing lost" is an exact assertion.
    AudioPacketQueue q(kTotalItems + 1);

    std::vector<Producer*> producers;
    producers.reserve(kProducers);
    for (int i = 0; i < kProducers; ++i) producers.push_back(new Producer(&q, i));

    // Per-producer expectation of the next sequence number. Any
    // reordering or duplication within one producer's stream trips this.
    std::map<QString, int> nextExpected;
    for (int i = 0; i < kProducers; ++i) nextExpected[peerName(i)] = 0;

    for (auto* p : producers) p->start();

    int received = 0;
    std::deque<AudioPacketQueue::Item> out;
    QDeadlineTimer deadline(kDeadlineMs);
    while (received < kTotalItems && !deadline.hasExpired()) {
        q.drain(out);
        for (const auto& item : out) {
            QCOMPARE(item.kind, AudioPacketQueue::Kind::Audio);
            auto it = nextExpected.find(item.peerId);
            QVERIFY2(it != nextExpected.end(), "unknown peer id came out of the queue");
            const int expect = it->second;
            // Payload integrity: producer id and sequence survived the
            // trip, and this peer's stream is still strictly in order.
            QCOMPARE(item.data, makeFrame(static_cast<uint16_t>(expect),
                                          item.peerId.mid(5, 1).toInt()));
            it->second = expect + 1;
            ++received;
        }
        out.clear();
    }

    for (auto* p : producers) {
        QVERIFY2(p->wait(kDeadlineMs), "producer thread failed to finish");
        delete p;
    }

    QCOMPARE(received, kTotalItems);
    QCOMPARE(q.dropped(), uint64_t(0));
    QCOMPARE(q.pushed(), uint64_t(kTotalItems));
    QCOMPARE(q.size(), 0);
    for (int i = 0; i < kProducers; ++i)
        QCOMPARE(nextExpected[peerName(i)], kItemsPerProducer);
}

void TestAudioHandoff::concurrentDrainFeedsJitterBufferWithoutLoss() {
    // End-to-end in the shape AudioWorker actually uses: a producer
    // thread pushes wire frames, and this thread — standing in for the
    // audio thread — drains them into a JitterBuffer it exclusively
    // owns, popping as it goes. The JitterBuffer sees only one thread,
    // which is the invariant under test.
    //
    // The assertions are on push-side counters (received, duplicates)
    // precisely because those are unaffected by timing: the producer
    // will usually outrun the pops and drive resyncs and late discards,
    // which is realistic and not an error. What must hold regardless of
    // interleaving is that every frame arrived exactly once.
    AudioPacketQueue q(kJbFrames + 1);
    JitterBuffer jb(48000, 1, 960);
    QVERIFY2(jb.isValid(), "Opus decoder could not be created");

    JbProducer producer(&q);
    producer.start();

    std::vector<int16_t> pcm(960);
    std::deque<AudioPacketQueue::Item> out;
    int ingested = 0;
    int popped = 0;
    QDeadlineTimer deadline(kDeadlineMs);
    while (ingested < kJbFrames && !deadline.hasExpired()) {
        q.drain(out);
        for (const auto& item : out) {
            QVERIFY(item.data.size() > JitterBuffer::kHeaderBytes);
            jb.pushPacket(item.data.constData(), int(item.data.size()));
            ++ingested;
        }
        out.clear();
        // Pop at roughly the rate we ingest, as the pump does. pop()
        // always writes a full frame, so this can never block or fail.
        jb.pop(pcm.data());
        ++popped;
    }

    QVERIFY2(producer.wait(kDeadlineMs), "producer thread failed to finish");
    QCOMPARE(ingested, kJbFrames);
    QCOMPARE(q.dropped(), uint64_t(0));
    // Every frame the producer pushed reached the buffer exactly once.
    QCOMPARE(jb.stats().received, uint64_t(kJbFrames));
    QCOMPARE(jb.stats().duplicates, uint64_t(0));
    QVERIFY(popped > 0);
}

QTEST_APPLESS_MAIN(TestAudioHandoff)
#include "test_audio_handoff.moc"
