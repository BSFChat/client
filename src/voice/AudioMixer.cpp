#include "voice/AudioMixer.h"

#include <algorithm>

AudioMixer::AudioMixer(int frameSamples)
    : m_frameSamples(frameSamples > 0 ? frameSamples : kFrameSamples)
{
    m_accum.assign(static_cast<size_t>(m_frameSamples), 0.0f);
    m_out.assign(static_cast<size_t>(m_frameSamples), 0);
}

void AudioMixer::begin() {
    std::fill(m_accum.begin(), m_accum.end(), 0.0f);
    m_sources = 0;
}

void AudioMixer::add(const int16_t* pcm, int samples, float gainFrom,
                     float gainTo) {
    if (!pcm || samples <= 0) return;
    m_sources++;
    // A muted source still counts as a source; it just adds nothing.
    if (gainFrom == 0.0f && gainTo == 0.0f) return;
    const int n = std::min(samples, m_frameSamples);
    const float scale = 1.0f / 32768.0f;
    const float step = (gainTo - gainFrom) / static_cast<float>(m_frameSamples);
    for (int i = 0; i < n; ++i) {
        const float g = gainFrom + step * static_cast<float>(i + 1);
        m_accum[static_cast<size_t>(i)] += static_cast<float>(pcm[i]) * scale * g;
    }
}

const std::vector<int16_t>& AudioMixer::finish(float masterFrom,
                                               float masterTo) {
    bsfchat::voice::applyGainRamp(m_accum.data(), m_frameSamples,
                                  masterFrom, masterTo);
    m_limiter.process(m_accum.data(), m_frameSamples);
    bsfchat::voice::floatToInt16(m_accum.data(), m_out.data(), m_frameSamples);
    return m_out;
}
