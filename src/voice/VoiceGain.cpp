#include "voice/VoiceGain.h"

#include <algorithm>
#include <cmath>

namespace bsfchat::voice {

float dbToLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}

float linearToDb(float lin) {
    if (!(lin > 1e-6f)) return -120.0f;
    return 20.0f * std::log10(lin);
}

void applyGainRamp(float* buf, int n, float from, float to) {
    if (!buf || n <= 0) return;
    if (from == to) {
        if (to == 1.0f) return;
        for (int i = 0; i < n; ++i) buf[i] *= to;
        return;
    }
    const float step = (to - from) / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
        buf[i] *= from + step * static_cast<float>(i + 1);
    }
}

// ---------------------------------------------------------------------
// PeakLimiter
// ---------------------------------------------------------------------

PeakLimiter::PeakLimiter(float ceiling, float releaseMs, int sampleRate)
    : m_ceiling(ceiling > 0.0f ? ceiling : kDefaultCeiling)
{
    const float releaseSamples =
        std::max(1.0f, releaseMs * static_cast<float>(sampleRate) / 1000.0f);
    m_releaseCoef = 1.0f - std::exp(-1.0f / releaseSamples);
    reset();
}

void PeakLimiter::reset() {
    m_delay.fill(0.0f);
    m_minHistory.fill(1.0f);
    m_minSum = kLookahead;
    m_pos = 0;
    m_dqHead = 0;
    m_dqCount = 0;
    m_sampleIndex = 0;
    m_env = 1.0f;
    m_blockMinGain = 1.0f;
}

void PeakLimiter::process(float* buf, int n) {
    m_blockMinGain = 1.0f;
    if (!buf || n <= 0) return;
    constexpr int L = kLookahead;

    for (int i = 0; i < n; ++i) {
        const float x = buf[i];
        const float a = std::fabs(x);
        const float need = a > m_ceiling ? m_ceiling / a : 1.0f;

        // Sliding minimum of `need` over the last L inputs. Pop from the
        // back everything no smaller than the newcomer (it can never be
        // the minimum again), push, then retire the front once it has
        // left the window.
        while (m_dqCount > 0) {
            const int back = (m_dqHead + m_dqCount - 1) % L;
            if (m_dqVal[static_cast<size_t>(back)] < need) break;
            --m_dqCount;
        }
        {
            const int slot = (m_dqHead + m_dqCount) % L;
            m_dqVal[static_cast<size_t>(slot)] = need;
            m_dqIdx[static_cast<size_t>(slot)] = m_sampleIndex;
            ++m_dqCount;
        }
        while (m_dqIdx[static_cast<size_t>(m_dqHead)] <= m_sampleIndex - L) {
            m_dqHead = (m_dqHead + 1) % L;
            --m_dqCount;
        }
        const float windowMin = m_dqVal[static_cast<size_t>(m_dqHead)];

        // Boxcar average of the window minima over the last L samples.
        m_minSum += static_cast<double>(windowMin)
                    - static_cast<double>(m_minHistory[static_cast<size_t>(m_pos)]);
        m_minHistory[static_cast<size_t>(m_pos)] = windowMin;
        float avg = static_cast<float>(m_minSum / L);

        // Release follower. Takes any fall immediately (so it is never
        // above `avg`, which is what preserves the bound) and recovers
        // exponentially.
        if (avg < m_env) m_env = avg;
        else m_env += (avg - m_env) * m_releaseCoef;
        if (m_env < m_blockMinGain) m_blockMinGain = m_env;

        // Delay line: slot m_pos+1 still holds the sample written L-1
        // steps ago.
        m_delay[static_cast<size_t>(m_pos)] = x;
        const float delayed = m_delay[static_cast<size_t>((m_pos + 1) % L)];
        buf[i] = delayed * m_env;

        ++m_sampleIndex;
        m_pos = (m_pos + 1) % L;
        if (m_pos == 0) {
            // Recompute the running sum exactly once per L samples so
            // float round-off in the incremental update cannot drift over
            // an hours-long call. O(1) amortised.
            double s = 0.0;
            for (float v : m_minHistory) s += v;
            m_minSum = s;
        }
    }
}

// ---------------------------------------------------------------------
// SpeechAgc
// ---------------------------------------------------------------------

SpeechAgc::SpeechAgc() : SpeechAgc(Config{}) {}

SpeechAgc::SpeechAgc(const Config& cfg) : m_cfg(cfg) {
    m_frameSec = static_cast<float>(kGainFrameSamples) / kGainSampleRate;
    m_levelAlpha = 1.0f - std::exp(-(m_frameSec * 1000.0f) / m_cfg.levelTauMs);
    reset();
}

void SpeechAgc::reset() {
    m_floorWindow.fill(0.0f);
    m_floorCount = 0;
    m_floorPos = 0;
    m_validFrames = 0;
    m_floorDb = -120.0f;
    m_levelPow = 0.0;
    m_levelDb = -120.0f;
    m_haveLevel = false;
    m_gainDb = 0.0f;
    m_appliedGain = 1.0f;
    m_lastSpeech = false;
    m_rawHistogram.fill(0);
    m_speechFrames = 0;
}

void SpeechAgc::processFrame(float* buf, int n) {
    if (!buf || n <= 0) return;

    double acc = 0.0;
    for (int i = 0; i < n; ++i) acc += static_cast<double>(buf[i]) * buf[i];
    const double meanSq = acc / n;
    const float rmsDb = meanSq > 1e-12
        ? static_cast<float>(10.0 * std::log10(meanSq)) : -120.0f;

    m_lastSpeech = false;
    if (rmsDb > kDigitalSilenceDbfs) {
        m_floorWindow[static_cast<size_t>(m_floorPos)] = rmsDb;
        m_floorPos = (m_floorPos + 1) % kFloorWindowFrames;
        if (m_floorCount < kFloorWindowFrames) ++m_floorCount;
        float mn = m_floorWindow[0];
        for (int i = 1; i < m_floorCount; ++i)
            mn = std::min(mn, m_floorWindow[static_cast<size_t>(i)]);
        m_floorDb = mn;
        if (m_validFrames < m_cfg.warmupFrames) ++m_validFrames;

        const bool warmedUp = m_validFrames >= m_cfg.warmupFrames;
        m_lastSpeech = warmedUp
            && rmsDb >= m_floorDb + m_cfg.speechMarginDb
            && rmsDb >= m_cfg.absoluteGateDbfs;
    }

    // The most gain the noise floor tolerates: enough to put the floor
    // at the noise ceiling, never less than unity (a noisy room is not a
    // reason to make the speech quieter), never more than maxGain.
    const float noiseAllowed = std::clamp(
        m_cfg.noiseCeilingDbfs - m_floorDb, 0.0f, m_cfg.maxGainDb);

    if (m_lastSpeech) {
        const double p = meanSq;
        if (!m_haveLevel) { m_levelPow = p; m_haveLevel = true; }
        else m_levelPow += (p - m_levelPow) * m_levelAlpha;
        m_levelDb = static_cast<float>(10.0 * std::log10(std::max(m_levelPow, 1e-12)));

        const float desired = std::clamp(m_cfg.targetDbfs - m_levelDb,
                                         m_cfg.minGainDb, noiseAllowed);
        const float delta = desired - m_gainDb;
        if (delta > 0.0f) {
            const float rate = delta > m_cfg.fastLaneDeficitDb
                ? m_cfg.gainUpFastDbPerSec : m_cfg.gainUpDbPerSec;
            m_gainDb += std::min(delta, rate * m_frameSec);
        } else {
            m_gainDb -= std::min(-delta, m_cfg.gainDownDbPerSec * m_frameSec);
        }

        const int bin = std::clamp(static_cast<int>(std::lround(rmsDb)), -100, 0);
        ++m_rawHistogram[static_cast<size_t>(bin + 100)];
        ++m_speechFrames;
    } else if (m_gainDb > noiseAllowed) {
        // Hold, except that the gain may never sit above what the
        // current floor tolerates. Moves at the gain-down rate, so a fan
        // switching on is a fade, not a jump.
        m_gainDb -= std::min(m_gainDb - noiseAllowed,
                             m_cfg.gainDownDbPerSec * m_frameSec);
    }

    const float target = dbToLinear(m_gainDb);
    applyGainRamp(buf, n, m_appliedGain, target);
    m_appliedGain = target;
}

float SpeechAgc::rawSpeechPercentileDbfs(float fraction) const {
    if (m_speechFrames <= 0) return -120.0f;
    const auto want = static_cast<int64_t>(
        std::ceil(std::clamp(fraction, 0.0f, 1.0f) * static_cast<float>(m_speechFrames)));
    int64_t seen = 0;
    for (int i = 0; i < static_cast<int>(m_rawHistogram.size()); ++i) {
        seen += m_rawHistogram[static_cast<size_t>(i)];
        if (seen >= std::max<int64_t>(want, 1)) return static_cast<float>(i - 100);
    }
    return 0.0f;
}

// ---------------------------------------------------------------------
// Conversion
// ---------------------------------------------------------------------

void int16ToFloat(const int16_t* in, float* out, int n) {
    for (int i = 0; i < n; ++i) out[i] = static_cast<float>(in[i]) * (1.0f / 32768.0f);
}

void floatToInt16(const float* in, int16_t* out, int n) {
    for (int i = 0; i < n; ++i) {
        const float v = std::nearbyint(in[i] * 32768.0f);
        out[i] = static_cast<int16_t>(std::clamp(v, -32768.0f, 32767.0f));
    }
}

} // namespace bsfchat::voice
