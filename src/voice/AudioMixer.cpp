#include "voice/AudioMixer.h"
#include <algorithm>
#include <cstring>

void AudioMixer::addFrame(const QString& peerId, const std::vector<int16_t>& pcm) {
    auto& buf = m_peers[peerId];
    buf.frames.push_back(pcm);
    while (buf.frames.size() > kMaxBufferedFrames) {
        buf.frames.pop_front();
    }
}

void AudioMixer::removePeer(const QString& peerId) {
    m_peers.remove(peerId);
}

std::vector<int16_t> AudioMixer::mix() {
    std::vector<int16_t> mixed(kFrameSamples, 0);

    if (m_peers.isEmpty()) return mixed;

    std::vector<int32_t> accum(kFrameSamples, 0);
    bool hasData = false;

    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
        auto& buf = it.value();
        if (buf.frames.empty()) continue;

        hasData = true;
        const auto& frame = buf.frames.front();
        size_t samples = std::min(frame.size(), static_cast<size_t>(kFrameSamples));
        for (size_t i = 0; i < samples; ++i) {
            accum[i] += frame[i];
        }
        buf.frames.pop_front();
    }

    if (hasData) {
        for (int i = 0; i < kFrameSamples; ++i) {
            mixed[i] = static_cast<int16_t>(std::clamp(accum[i], (int32_t)-32768, (int32_t)32767));
        }
    }

    return mixed;
}
