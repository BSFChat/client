#pragma once

#include <QMap>
#include <QString>
#include <deque>
#include <vector>
#include <cstdint>

class AudioMixer {
public:
    static constexpr int kFrameSamples = 960; // 20ms at 48kHz
    static constexpr size_t kMaxBufferedFrames = 10; // 200ms jitter buffer

    void addFrame(const QString& peerId, const std::vector<int16_t>& pcm);
    void removePeer(const QString& peerId);
    std::vector<int16_t> mix();
    bool hasPeers() const { return !m_peers.isEmpty(); }

private:
    struct PeerBuffer {
        std::deque<std::vector<int16_t>> frames;
    };
    QMap<QString, PeerBuffer> m_peers;
};
