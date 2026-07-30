#include "voice/AudioPacketQueue.h"

namespace bsfchat::voice {

AudioPacketQueue::AudioPacketQueue(int capacity)
    : m_capacity(capacity > 0 ? capacity : kDefaultCapacity)
{
}

bool AudioPacketQueue::push(Kind kind, const QString& peerId,
                            const QByteArray& data) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (kind == Kind::Audio
        && static_cast<int>(m_items.size()) >= m_capacity) {
        // Reject the newest rather than evicting the oldest. Both are
        // equally "wrong" for audio at this depth (the whole backlog is
        // already stale), but rejecting the newest cannot displace a
        // queued RemovePeer, which must never be lost.
        m_dropped++;
        return false;
    }
    m_items.push_back(Item{kind, peerId, data});
    m_pushed++;
    return true;
}

void AudioPacketQueue::drain(std::deque<Item>& out) {
    out.clear();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_items.swap(out);
}

void AudioPacketQueue::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_items.clear();
}

int AudioPacketQueue::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<int>(m_items.size());
}

uint64_t AudioPacketQueue::pushed() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pushed;
}

uint64_t AudioPacketQueue::dropped() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_dropped;
}

} // namespace bsfchat::voice
