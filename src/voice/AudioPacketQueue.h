#pragma once

// Thread handoff for the voice receive path.
//
// The real-time audio pipeline (capture, Opus encode, jitter buffering,
// decode, mix, sink writes) runs on a dedicated audio thread — see
// AudioWorker. Received packets, however, originate elsewhere: a
// libdatachannel worker thread parses the SCTP message, PeerConnection-
// Manager hops it to the GUI thread, and VoiceEngine hands it to
// AudioEngine. Something has to carry them across into the audio
// thread.
//
// Why not just post a queued signal/slot at the worker?
// ----------------------------------------------------
// It would work, but it puts one QMetaCallEvent allocation per 20 ms
// packet per peer into the same event queue that dispatches the
// playback pump timer and the QAudioSource readyRead. That queue is
// unbounded, so a wedged audio thread grows it without limit, and a
// backlog of packet events delays the pump events behind them —
// exactly the coupling this refactor exists to remove.
//
// So instead: an explicit, bounded, mutex-guarded queue that the audio
// thread drains in one O(1) swap at the top of every pump. The lock is
// held only for a push or a deque swap, never across an Opus decode,
// so a producer can never be blocked by the audio thread doing work.
//
// Ordering
// --------
// Peer removal travels through the same queue as audio (Kind::
// RemovePeer) rather than as a side-channel call. If it didn't, a
// remove issued on the GUI thread could overtake packets already
// queued for that peer and resurrect a jitter buffer for a peer that
// has left. One FIFO means removal is always observed in the same
// position relative to that peer's audio as it was issued.
//
// Threading contract
// ------------------
// push()/clear()/size()/counters: any thread, any number of them.
// drain(): the consumer (audio) thread only — one consumer, always.

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <deque>
#include <mutex>

namespace bsfchat::voice {

class AudioPacketQueue {
public:
    enum class Kind : uint8_t {
        Audio,       // one wire frame (4-byte header + Opus) for peerId
        RemovePeer,  // destroy peerId's jitter buffer
    };

    struct Item {
        Kind kind = Kind::Audio;
        QString peerId;
        QByteArray data;
    };

    // 400 items is ~8 s of a single peer's audio, or ~1 s across eight
    // peers. Anything that has sat here that long is already far past
    // its playout instant and would be discarded by JitterBuffer::push()
    // as TooLate the moment it arrived, so the cap costs nothing real;
    // it exists purely so a wedged or stopped audio thread cannot grow
    // the queue without bound.
    static constexpr int kDefaultCapacity = 400;

    explicit AudioPacketQueue(int capacity = kDefaultCapacity);

    AudioPacketQueue(const AudioPacketQueue&) = delete;
    AudioPacketQueue& operator=(const AudioPacketQueue&) = delete;

    // Producer side. Returns false when an audio packet was dropped
    // because the queue is at capacity. Control items (RemovePeer) are
    // always accepted — dropping one would leak a jitter buffer, and
    // there can only ever be as many outstanding as there are peers.
    bool push(Kind kind, const QString& peerId, const QByteArray& data = {});

    // Consumer side (audio thread). Moves the entire backlog into `out`
    // with a deque swap, so the lock is held for O(1) and the caller
    // then processes the batch entirely outside it. `out` is cleared
    // first; reuse the same deque across calls to keep its allocation.
    void drain(std::deque<Item>& out);

    void clear();

    int size() const;
    uint64_t pushed() const;
    uint64_t dropped() const;

private:
    mutable std::mutex m_mutex;
    std::deque<Item> m_items;
    const int m_capacity;
    uint64_t m_pushed = 0;
    uint64_t m_dropped = 0;
};

} // namespace bsfchat::voice
