#include "voice/SoundGenerator.h"

#include <QBuffer>
#include <QDataStream>
#include <cmath>
#include <vector>

namespace SoundGenerator {

namespace {

// Write a WAV header + PCM data into a QByteArray
QByteArray makeWav(const std::vector<int16_t>& samples, int sampleRate) {
    QByteArray wav;
    QDataStream out(&wav, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);

    int channels = 1;
    int bitsPerSample = 16;
    int dataSize = static_cast<int>(samples.size() * sizeof(int16_t));
    int byteRate = sampleRate * channels * bitsPerSample / 8;
    int blockAlign = channels * bitsPerSample / 8;

    // RIFF header
    out.writeRawData("RIFF", 4);
    out << static_cast<int32_t>(36 + dataSize);
    out.writeRawData("WAVE", 4);

    // fmt chunk
    out.writeRawData("fmt ", 4);
    out << static_cast<int32_t>(16);        // chunk size
    out << static_cast<int16_t>(1);         // PCM format
    out << static_cast<int16_t>(channels);
    out << static_cast<int32_t>(sampleRate);
    out << static_cast<int32_t>(byteRate);
    out << static_cast<int16_t>(blockAlign);
    out << static_cast<int16_t>(bitsPerSample);

    // data chunk
    out.writeRawData("data", 4);
    out << static_cast<int32_t>(dataSize);
    for (int16_t s : samples) {
        out << s;
    }

    return wav;
}

// Generate a sine wave with fade in/out
std::vector<int16_t> generateSineSegment(float freq, int durationMs, int sampleRate, float volume) {
    int numSamples = sampleRate * durationMs / 1000;
    std::vector<int16_t> samples(numSamples);

    int fadeLen = std::min(numSamples / 4, sampleRate * 10 / 1000); // 10ms fade or 1/4 of duration

    for (int i = 0; i < numSamples; ++i) {
        float t = static_cast<float>(i) / sampleRate;
        float sample = std::sin(2.0f * M_PI * freq * t) * volume;

        // Fade in
        if (i < fadeLen) {
            sample *= static_cast<float>(i) / fadeLen;
        }
        // Fade out
        if (i > numSamples - fadeLen) {
            sample *= static_cast<float>(numSamples - i) / fadeLen;
        }

        samples[i] = static_cast<int16_t>(sample * 32767.0f);
    }

    return samples;
}

} // namespace

QByteArray generateTone(float freq1, int duration1Ms,
                         float freq2, int duration2Ms,
                         int sampleRate, float volume) {
    auto seg1 = generateSineSegment(freq1, duration1Ms, sampleRate, volume);
    auto seg2 = generateSineSegment(freq2, duration2Ms, sampleRate, volume);

    // Small gap between tones (20ms silence)
    int gapSamples = sampleRate * 20 / 1000;
    std::vector<int16_t> combined;
    combined.reserve(seg1.size() + gapSamples + seg2.size());
    combined.insert(combined.end(), seg1.begin(), seg1.end());
    combined.resize(combined.size() + gapSamples, 0);
    combined.insert(combined.end(), seg2.begin(), seg2.end());

    return makeWav(combined, sampleRate);
}

QByteArray generateJoinSound() {
    // Ascending two-tone: E5 (659Hz) → G5 (784Hz), pleasant and bright
    return generateTone(659.0f, 80, 784.0f, 100, 48000, 0.25f);
}

QByteArray generateLeaveSound() {
    // Descending two-tone: G5 (784Hz) → E5 (659Hz)
    return generateTone(784.0f, 80, 659.0f, 100, 48000, 0.2f);
}

QByteArray generateMuteSound() {
    // Single short click: 1kHz, 30ms
    auto samples = generateSineSegment(1000.0f, 30, 48000, 0.15f);
    return makeWav(samples, 48000);
}

} // namespace SoundGenerator
