#include "voice/AudioMixer.h"

#include <algorithm>
#include <cstring>

AudioMixer::AudioMixer(int frameSamples)
    : m_frameSamples(frameSamples > 0 ? frameSamples : kFrameSamples)
{
    m_accum.assign(static_cast<size_t>(m_frameSamples), 0);
    m_out.assign(static_cast<size_t>(m_frameSamples), 0);
}

void AudioMixer::begin() {
    std::fill(m_accum.begin(), m_accum.end(), int64_t{0});
    m_sources = 0;
}

void AudioMixer::add(const int16_t* pcm, int samples) {
    if (!pcm || samples <= 0) return;
    const int n = std::min(samples, m_frameSamples);
    for (int i = 0; i < n; ++i) {
        m_accum[static_cast<size_t>(i)] += static_cast<int64_t>(pcm[i]);
    }
    m_sources++;
}

const std::vector<int16_t>& AudioMixer::finish() {
    if (m_sources == 0) {
        std::memset(m_out.data(), 0, m_out.size() * sizeof(int16_t));
        return m_out;
    }
    constexpr int64_t kMin = -32768;
    constexpr int64_t kMax = 32767;
    for (int i = 0; i < m_frameSamples; ++i) {
        m_out[static_cast<size_t>(i)] =
            static_cast<int16_t>(std::clamp(m_accum[static_cast<size_t>(i)],
                                            kMin, kMax));
    }
    return m_out;
}
