// Voice level control: SpeechAgc, PeakLimiter, the mixer that now ends
// in the limiter, the volume taper and its migration — plus one Opus
// round trip that measures whether the codec and jitter buffer change
// level at all.
//
// The report this answers (2026-09-21): "Audio volume is quiet for both
// of us testing at the minute, are we not offering 100% output volume?"
// Output was already at unity. The quiet was the capture side: raw mic
// level straight into Opus, with no AGC. These tests pin the fix.
//
// PURE DSP. Synthetic buffers in, samples out. Nothing here may open an
// audio device: on a dev Mac every new binary that touches the mic is a
// fresh TCC permission prompt on the owner's screen.
//
// The "speech" is synthetic and deterministic: a 140 Hz harmonic series
// (the rough spectrum of a voiced vowel) under a 4 Hz syllabic envelope,
// in 1.5 s phrases separated by 0.5 s pauses, over white noise from a
// fixed-seed LCG. That is enough structure to exercise what matters —
// frame levels that swing ~15 dB between syllables, and pauses at the
// noise floor — and it is identical on every run.

#include <QtTest>

#include "core/AudioVolume.h"
#include "voice/AudioMixer.h"
#include "voice/JitterBuffer.h"
#include "voice/VoiceGain.h"

#include <opus.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace bsfchat::voice;

namespace {

constexpr int kFs = 48000;
constexpr int kN = 960;  // one 20 ms frame
constexpr double kPi = 3.14159265358979323846;

struct Lcg {
    uint32_t s = 0x12345678u;
    // Uniform in [-1, 1).
    float next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(static_cast<int32_t>(s)) / 2147483648.0f;
    }
};

double rmsDb(const float* x, size_t n) {
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) acc += double(x[i]) * x[i];
    if (acc <= 0.0) return -120.0;
    return 10.0 * std::log10(acc / double(n));
}

float peakAbs(const std::vector<float>& x) {
    float m = 0.0f;
    for (float v : x) m = std::max(m, std::fabs(v));
    return m;
}

// Synthetic speech at a given RMS (measured over the voiced parts only)
// over white noise at a given RMS. `active[f]` says whether frame f is
// inside a phrase.
struct Speech {
    std::vector<float> x;
    std::vector<bool> active;
};

Speech makeSpeech(double seconds, double speechDbfs, double noiseDbfs,
                  uint32_t seed = 1, double phraseSec = 1.5, double pauseSec = 0.5) {
    const size_t total = size_t(seconds * kFs);
    const size_t frames = total / kN;
    Speech s;
    s.x.assign(frames * kN, 0.0f);
    s.active.assign(frames, false);

    // Voiced tone with a syllabic envelope, unscaled.
    std::vector<float> voice(frames * kN, 0.0f);
    double vAcc = 0.0;
    size_t vCount = 0;
    const double period = phraseSec + pauseSec;
    for (size_t i = 0; i < voice.size(); ++i) {
        const double t = double(i) / kFs;
        const double inPhrase = std::fmod(t, period);
        if (inPhrase >= phraseSec) continue;
        double tone = 0.0;
        for (int k = 1; k * 140.0 < 3500.0; ++k)
            tone += std::sin(2.0 * kPi * 140.0 * k * t + k * 0.7) / k;
        const double env = 0.5 - 0.5 * std::cos(2.0 * kPi * 4.0 * inPhrase);
        voice[i] = float(tone * env);
        vAcc += voice[i] * double(voice[i]);
        ++vCount;
    }
    const double vRms = std::sqrt(vAcc / double(vCount));
    const double vScale = std::pow(10.0, speechDbfs / 20.0) / vRms;

    // Uniform white noise has RMS 1/sqrt(3).
    const double nScale = std::pow(10.0, noiseDbfs / 20.0) * std::sqrt(3.0);
    Lcg rng;
    rng.s ^= seed * 2654435761u;
    for (size_t i = 0; i < s.x.size(); ++i)
        s.x[i] = float(voice[i] * vScale + rng.next() * nScale);
    for (size_t f = 0; f < frames; ++f) {
        const double t0 = double(f * kN) / kFs;
        const double t1 = double((f + 1) * kN) / kFs;
        // Wholly inside a phrase.
        s.active[f] = std::fmod(t0, period) < phraseSec
                      && std::fmod(t1 - 1e-9, period) < phraseSec
                      && std::fmod(t0, period) <= std::fmod(t1 - 1e-9, period);
    }
    return s;
}

// Runs the AGC over the whole signal in 20 ms frames, in place.
// Returns the gain in dB after each frame.
std::vector<float> runAgc(SpeechAgc& agc, std::vector<float>& x) {
    std::vector<float> gains;
    for (size_t off = 0; off + kN <= x.size(); off += kN) {
        agc.processFrame(x.data() + off, kN);
        gains.push_back(agc.gainDb());
    }
    return gains;
}

// Power-average level of the active frames in [fromFrame, end).
double activeLevelDb(const std::vector<float>& x, const std::vector<bool>& active,
                     size_t fromFrame) {
    double acc = 0.0;
    size_t n = 0;
    for (size_t f = fromFrame; f < active.size(); ++f) {
        if (!active[f]) continue;
        for (size_t i = f * kN; i < (f + 1) * kN; ++i) acc += double(x[i]) * x[i];
        n += kN;
    }
    return n ? 10.0 * std::log10(acc / double(n)) : -120.0;
}

} // namespace

class TestVoiceGain : public QObject {
    Q_OBJECT

private slots:
    // ---------------- PeakLimiter ----------------

    void limiterIsTransparentBelowCeiling() {
        // Anything under the ceiling must come out bit-identical, only
        // delayed: the limiter may not colour ordinary speech.
        PeakLimiter lim;
        std::vector<float> in(kN * 20), out;
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = 0.5f * float(std::sin(2.0 * kPi * 440.0 * double(i) / kFs));
        out = in;
        for (size_t off = 0; off < out.size(); off += kN)
            lim.process(out.data() + off, kN);
        const int d = PeakLimiter::latencySamples();
        QCOMPARE(d, 95);
        for (size_t i = size_t(d); i < out.size(); ++i)
            QCOMPARE(out[i], in[i - size_t(d)]);
        QCOMPARE(lim.lastBlockMinGain(), 1.0f);
    }

    void limiterNeverExceedsCeiling_data() {
        QTest::addColumn<int>("kind");
        QTest::newRow("sine +20 dB") << 0;
        QTest::newRow("isolated impulses x10") << 1;
        QTest::newRow("loud white noise") << 2;
        QTest::newRow("speech boosted +24 dB") << 3;
        QTest::newRow("full-scale square") << 4;
    }
    void limiterNeverExceedsCeiling() {
        QFETCH(int, kind);
        std::vector<float> x(kN * 100, 0.0f);
        Lcg rng;
        if (kind == 3) x = makeSpeech(2.0, -18.0 + 24.0, -60.0).x;
        for (size_t i = 0; i < x.size(); ++i) {
            switch (kind) {
            case 0: x[i] = 10.0f * float(std::sin(2.0 * kPi * 220.0 * double(i) / kFs)); break;
            case 1: x[i] = (i % 997 == 0) ? 10.0f : 0.01f * rng.next(); break;
            case 2: x[i] = 4.0f * rng.next(); break;
            case 4: x[i] = ((i / 55) % 2) ? 1.0f : -1.0f; break;
            default: break;
            }
        }
        PeakLimiter lim;
        for (size_t off = 0; off + kN <= x.size(); off += kN) lim.process(x.data() + off, kN);
        QVERIFY2(peakAbs(x) <= lim.ceiling() * (1.0f + 1e-6f),
                 qPrintable(QStringLiteral("peak %1 > ceiling %2")
                                .arg(double(peakAbs(x))).arg(double(lim.ceiling()))));
    }

    void limiterGainMovesSmoothly() {
        // A -20 dBFS tone with a 0.2 s burst 26 dB louder. The applied
        // gain (output over delayed input) must never jump: the attack
        // is a 2 ms ramp, the release an 80 ms exponential.
        std::vector<float> in(kN * 50);
        for (size_t i = 0; i < in.size(); ++i) {
            const double a = (i >= kN * 20 && i < kN * 30) ? 2.0 : 0.1;
            in[i] = float(a * std::sin(2.0 * kPi * 300.0 * double(i) / kFs));
        }
        std::vector<float> out = in;
        PeakLimiter lim;
        for (size_t off = 0; off < out.size(); off += kN) lim.process(out.data() + off, kN);
        const size_t d = size_t(PeakLimiter::latencySamples());
        // Gain slope per sample. Samples near zero crossings are skipped
        // (the ratio is meaningless there), so the change since the last
        // usable sample is divided by the distance to it.
        double prev = -1.0, maxSlope = 0.0, minGain = 1.0;
        size_t prevAt = 0;
        for (size_t i = d; i < out.size(); ++i) {
            const float xi = in[i - d];
            if (std::fabs(xi) < 0.05f) continue;
            const double g = double(out[i]) / double(xi);
            minGain = std::min(minGain, g);
            if (prev >= 0.0)
                maxSlope = std::max(maxSlope, std::fabs(g - prev) / double(i - prevAt));
            prev = g;
            prevAt = i;
        }
        QVERIFY2(minGain < 0.5, "burst was never limited");
        // The attack ramp takes the gain from 1 to ~0.45 over 96 samples,
        // ~0.006 per sample. A hard clamp changes the effective gain by
        // ~0.5 between adjacent samples.
        QVERIFY2(maxSlope < 0.02, qPrintable(QString::number(maxSlope)));
    }

    // ---------------- SpeechAgc ----------------

    void agcLiftsQuietSpeechToTarget() {
        // A -40 dBFS talker — a typical laptop/USB mic at speaking
        // distance, and the reason calls sounded quiet — over a clean
        // -75 dBFS floor. Without the AGC this arrives at -40, 22 dB
        // under target.
        Speech s = makeSpeech(14.0, -40.0, -75.0);
        const double before = activeLevelDb(s.x, s.active, 0);
        QVERIFY(std::fabs(before - (-40.0)) < 1.5);
        SpeechAgc agc;
        runAgc(agc, s.x);
        const double after = activeLevelDb(s.x, s.active, s.active.size() - 200);
        QVERIFY2(std::fabs(after - (-18.0)) <= 3.0,
                 qPrintable(QStringLiteral("speech lands at %1 dBFS").arg(after)));
    }

    void agcConvergesWithinAFewSecondsOfSpeech() {
        Speech s = makeSpeech(8.0, -40.0, -75.0);
        SpeechAgc agc;
        const auto gains = runAgc(agc, s.x);
        // 5 s of signal is 3.75 s of speech (1.5 s phrases, 0.5 s pauses).
        QVERIFY2(gains[250] >= 17.0, qPrintable(QString::number(double(gains[250]))));
    }

    void agcTrimsLoudSpeech() {
        Speech s = makeSpeech(10.0, -6.0, -60.0);
        SpeechAgc agc;
        runAgc(agc, s.x);
        const double after = activeLevelDb(s.x, s.active, s.active.size() - 150);
        // Floored by minGain (-10 dB): -6 -> -16, within 3 dB of target.
        QVERIFY2(std::fabs(after - (-18.0)) <= 3.0, qPrintable(QString::number(after)));
    }

    void agcPlusLimiterNeverExceedsCeiling() {
        // The whole capture chain, with the input volume at 200% on top:
        // the worst case the settings allow.
        Speech s = makeSpeech(10.0, -12.0, -60.0);
        SpeechAgc agc;
        PeakLimiter lim;
        const float boost = bsfchat::audio::volumeToGain(200);
        for (size_t off = 0; off + kN <= s.x.size(); off += kN) {
            agc.processFrame(s.x.data() + off, kN);
            applyGainRamp(s.x.data() + off, kN, boost, boost);
            lim.process(s.x.data() + off, kN);
        }
        QVERIFY(peakAbs(s.x) <= lim.ceiling() * (1.0f + 1e-6f));
        std::vector<int16_t> pcm(s.x.size());
        floatToInt16(s.x.data(), pcm.data(), int(pcm.size()));
        int16_t mx = 0;
        for (int16_t v : pcm) mx = std::max<int16_t>(mx, int16_t(std::abs(int(v))));
        QVERIFY(mx <= 29205);  // -1 dBFS, and never the 32767 of a clamp
    }

    void agcDoesNotAmplifyNoiseOnly() {
        // 30 s of room noise and nobody talking. An AGC that chases
        // whatever is there would raise this to target; it must not move.
        std::vector<float> x(size_t(30 * kFs / kN) * kN);
        Lcg rng;
        for (float& v : x) v = rng.next() * float(std::pow(10.0, -60.0 / 20.0) * std::sqrt(3.0));
        SpeechAgc agc;
        const auto gains = runAgc(agc, x);
        const float maxGain = *std::max_element(gains.begin(), gains.end());
        QVERIFY2(maxGain <= 0.5f, qPrintable(QString::number(double(maxGain))));
        QCOMPARE(agc.speechFrames(), int64_t(0));
    }

    void agcHoldsGainThroughPauses() {
        // Speech, then 6 s of silence at the noise floor. Through the
        // pause the gain must not creep up (that is the hiss swelling up
        // between sentences) — hold, or fall, never rise.
        Speech talk = makeSpeech(8.0, -40.0, -70.0);
        std::vector<float> x = talk.x;
        Lcg rng;
        const size_t pauseSamples = size_t(6 * kFs / kN) * kN;
        const float nAmp = float(std::pow(10.0, -70.0 / 20.0) * std::sqrt(3.0));
        for (size_t i = 0; i < pauseSamples; ++i) x.push_back(rng.next() * nAmp);
        SpeechAgc agc;
        const auto gains = runAgc(agc, x);
        const size_t pauseStart = talk.x.size() / kN + 2;
        for (size_t f = pauseStart + 1; f < gains.size(); ++f)
            QVERIFY2(gains[f] <= gains[f - 1] + 1e-4f,
                     qPrintable(QStringLiteral("gain rose in pause at frame %1").arg(f)));
        // And what is transmitted in the pause stays under the -40 dBFS
        // noise ceiling.
        const double pauseOut = rmsDb(x.data() + (pauseStart + 10) * kN,
                                      x.size() - (pauseStart + 10) * kN);
        QVERIFY2(pauseOut <= -40.0, qPrintable(QString::number(pauseOut)));
    }

    void agcDoesNotPumpDuringSteadySpeech() {
        // Once converged on a steady talker, the gain must sit still:
        // swings of several dB across syllables are audible as pumping.
        Speech s = makeSpeech(20.0, -35.0, -75.0);
        SpeechAgc agc;
        const auto gains = runAgc(agc, s.x);
        const auto [lo, hi] = std::minmax_element(gains.begin() + 500, gains.end());
        QVERIFY2(*hi - *lo <= 3.0f,
                 qPrintable(QStringLiteral("gain swings %1 dB").arg(double(*hi - *lo))));
    }

    void agcRespectsNoiseCeiling() {
        // Quiet talker in a noisy room: -38 dBFS speech over a -52 floor.
        // Lifting the speech to target would need +20 dB and put the fan
        // at -32 dBFS. The gain is capped so the floor stays at or under
        // -40 dBFS.
        Speech s = makeSpeech(15.0, -38.0, -52.0);
        SpeechAgc agc;
        const auto gains = runAgc(agc, s.x);
        const float maxGain = *std::max_element(gains.begin(), gains.end());
        const float floorDb = agc.noiseFloorDbfs();
        QVERIFY2(floorDb + maxGain <= -40.0f + 0.5f,
                 qPrintable(QStringLiteral("floor %1 + gain %2").arg(double(floorDb))
                                .arg(double(maxGain))));
        QVERIFY2(maxGain > 3.0f, "the ceiling should still allow some lift");
    }

    void agcIgnoresDigitalSilenceAtStart() {
        // A device that delivers zeros while it warms up (or a TCC-denied
        // mic) followed by ordinary room noise. Zeros must not seed the
        // noise floor at -120, which would make the room noise look like
        // speech and get it boosted.
        std::vector<float> x(size_t(2 * kFs / kN) * kN, 0.0f);
        Lcg rng;
        const float nAmp = float(std::pow(10.0, -55.0 / 20.0) * std::sqrt(3.0));
        for (int i = 0; i < 6 * kFs; ++i) x.push_back(rng.next() * nAmp);
        x.resize(x.size() / kN * kN);
        SpeechAgc agc;
        const auto gains = runAgc(agc, x);
        QVERIFY(*std::max_element(gains.begin(), gains.end()) <= 0.5f);
    }

    void agcSilenceStaysSilent() {
        std::vector<float> x(size_t(3 * kFs / kN) * kN, 0.0f);
        SpeechAgc agc;
        runAgc(agc, x);
        QCOMPARE(peakAbs(x), 0.0f);
    }

    void agcGainChangesAreSmooth() {
        // Per-sample gain is output/input exactly (the AGC has no delay).
        // Across the whole convergence from 0 to +22 dB the gain must
        // never step: at most 12 dB/s spread over each 960-sample frame.
        Speech s = makeSpeech(8.0, -40.0, -75.0);
        const std::vector<float> in = s.x;
        SpeechAgc agc;
        runAgc(agc, s.x);
        double prevDb = 0.0, maxStepDb = 0.0;
        bool have = false;
        for (size_t i = 0; i < in.size(); ++i) {
            if (std::fabs(in[i]) < 1e-4f) continue;
            const double gDb = 20.0 * std::log10(std::fabs(double(s.x[i]) / double(in[i])));
            if (have) maxStepDb = std::max(maxStepDb, std::fabs(gDb - prevDb));
            prevDb = gDb;
            have = true;
        }
        QVERIFY2(maxStepDb < 0.01, qPrintable(QString::number(maxStepDb)));
    }

    void agcReportsRawSpeechLevel() {
        // The diagnostic the log will now carry: what the mic delivered
        // before any gain.
        Speech s = makeSpeech(10.0, -40.0, -75.0);
        SpeechAgc agc;
        runAgc(agc, s.x);
        QVERIFY(agc.speechFrames() > 200);
        const float p50 = agc.rawSpeechPercentileDbfs(0.5f);
        QVERIFY2(p50 >= -46.0f && p50 <= -34.0f, qPrintable(QString::number(double(p50))));
        QVERIFY(agc.rawSpeechPercentileDbfs(0.1f) <= p50);
        QVERIFY(agc.rawSpeechPercentileDbfs(0.9f) >= p50);
    }

    // ---------------- Mixer ----------------

    void mixerSumsWithoutAttenuation() {
        // Two quiet peers: the mix is their exact sum (delayed by the
        // limiter), no divide-by-N, no gain.
        AudioMixer mixer;
        std::vector<int16_t> a(kN), b(kN);
        std::vector<int16_t> outAll;
        std::vector<int32_t> sums;
        for (int f = 0; f < 4; ++f) {
            for (int i = 0; i < kN; ++i) {
                a[i] = int16_t(1000.0 * std::sin(0.01 * (f * kN + i)));
                b[i] = int16_t(700.0 * std::sin(0.023 * (f * kN + i)));
                sums.push_back(int32_t(a[i]) + b[i]);
            }
            mixer.begin();
            mixer.add(a.data(), kN);
            mixer.add(b.data(), kN);
            const auto& out = mixer.finish();
            outAll.insert(outAll.end(), out.begin(), out.end());
        }
        const size_t d = size_t(PeakLimiter::latencySamples());
        for (size_t i = d; i < outAll.size(); ++i) QCOMPARE(int32_t(outAll[i]), sums[i - d]);
    }

    void mixerLimitsInsteadOfClipping() {
        // Four loud peers at 20000 each. The old mixer hard-clamped to
        // +/-32767: flat tops, square-wave distortion. The limiter keeps
        // the mix under -1 dBFS with no flat-topped run.
        AudioMixer mixer;
        std::vector<int16_t> src(kN);
        std::vector<int16_t> outAll;
        for (int f = 0; f < 10; ++f) {
            for (int i = 0; i < kN; ++i)
                src[i] = int16_t(20000.0 * std::sin(2.0 * kPi * 200.0 * (f * kN + i) / kFs));
            mixer.begin();
            for (int p = 0; p < 4; ++p) mixer.add(src.data(), kN);
            const auto& out = mixer.finish();
            outAll.insert(outAll.end(), out.begin(), out.end());
        }
        int16_t mx = 0;
        int run = 0, maxRun = 0;
        for (size_t i = 1; i < outAll.size(); ++i) {
            mx = std::max<int16_t>(mx, int16_t(std::abs(int(outAll[i]))));
            run = (outAll[i] == outAll[i - 1] && std::abs(int(outAll[i])) > 20000) ? run + 1 : 0;
            maxRun = std::max(maxRun, run);
        }
        QVERIFY2(mx <= 29205, qPrintable(QString::number(mx)));
        QVERIFY2(maxRun < 4, qPrintable(QStringLiteral("flat-topped run of %1").arg(maxRun)));
    }

    void mixerAppliesPeerAndMasterGain() {
        // Per-user 0% silences that user alone; master 200% is exactly x4
        // on a signal quiet enough not to need limiting.
        AudioMixer mixer;
        std::vector<int16_t> a(kN, 1000), b(kN, 3000);
        std::vector<int16_t> outAll;
        const float boost = bsfchat::audio::volumeToGain(200);
        for (int f = 0; f < 3; ++f) {
            mixer.begin();
            mixer.add(a.data(), kN, 1.0f);
            mixer.add(b.data(), kN, 0.0f);
            const auto& out = mixer.finish(boost, boost);
            outAll.insert(outAll.end(), out.begin(), out.end());
        }
        for (size_t i = kN; i < outAll.size(); ++i) QCOMPARE(outAll[i], int16_t(4000));
    }

    void mixerRampsGainChanges() {
        // A volume change mid-call is a ramp across the frame, not a step.
        AudioMixer mixer;
        std::vector<int16_t> a(kN, 8000);
        mixer.begin(); mixer.add(a.data(), kN, 1.0f); mixer.finish();
        mixer.begin(); mixer.add(a.data(), kN, 1.0f); mixer.finish();
        mixer.begin(); mixer.add(a.data(), kN, 1.0f, 0.25f);
        const std::vector<int16_t> out = mixer.finish();
        int maxStep = 0;
        for (int i = 1; i < kN; ++i) maxStep = std::max(maxStep, std::abs(out[i] - out[i - 1]));
        QVERIFY2(maxStep <= 10, qPrintable(QString::number(maxStep)));
    }

    // ---------------- Volume taper and migration ----------------

    void volumeTaper() {
        using namespace bsfchat::audio;
        QCOMPARE(volumeToGain(0), 0.0f);
        QCOMPARE(volumeToGain(100), 1.0f);
        QCOMPARE(volumeToGain(50), 0.25f);
        QCOMPARE(volumeToGain(200), 4.0f);  // +12.04 dB
        QVERIFY(std::fabs(linearToDb(volumeToGain(200)) - 12.04f) < 0.01f);
        QCOMPARE(volumeToGain(-10), 0.0f);
        QCOMPARE(volumeToGain(500), 4.0f);
        for (int p = 1; p <= kVolumeMax; ++p) QVERIFY(volumeToGain(p) > volumeToGain(p - 1));
        QCOMPARE(clampVolume(250), 200);
        QCOMPARE(clampVolume(-1), 0);
    }

    void volumeMigration() {
        using namespace bsfchat::audio;
        // Pre-schema values were set while the slider did nothing: reset.
        QCOMPARE(migrateStoredVolume(30, 0), 100);
        QCOMPARE(migrateStoredVolume(0, 1), 100);
        QCOMPARE(migrateStoredVolume(100, 0), 100);
        QCOMPARE(migrateStoredVolume(std::nullopt, 0), 100);
        // From schema 2 on, honoured, clamped.
        QCOMPARE(migrateStoredVolume(150, kVolumeSchema), 150);
        QCOMPARE(migrateStoredVolume(30, kVolumeSchema), 30);
        QCOMPARE(migrateStoredVolume(999, kVolumeSchema), 200);
        QCOMPARE(migrateStoredVolume(std::nullopt, kVolumeSchema), 100);
    }

    // ---------------- Measurement: does the codec path lose level? ----------------

    void opusRoundTripPreservesLevel() {
        // Rules out the other suspects for "quiet": the encoder settings
        // AudioWorker uses (VOIP, 32 kbps, voice signal) and the jitter
        // buffer's decode. A -30 dBFS talker must come out at -30.
        int err = 0;
        OpusEncoder* enc = opus_encoder_create(kFs, 1, OPUS_APPLICATION_VOIP, &err);
        QVERIFY(err == OPUS_OK && enc);
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(32000));
        opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        JitterBuffer jb(kFs, 1, kN);
        QVERIFY(jb.isValid());

        Speech s = makeSpeech(10.0, -30.0, -70.0);
        std::vector<int16_t> pcm(s.x.size());
        floatToInt16(s.x.data(), pcm.data(), int(pcm.size()));
        std::vector<int16_t> out(kN);
        double inAcc = 0.0, outAcc = 0.0;
        size_t inN = 0, outN = 0;
        const size_t frames = pcm.size() / kN;
        for (size_t f = 0; f < frames; ++f) {
            unsigned char pkt[4 + 4000];
            const int n = opus_encode(enc, pcm.data() + f * kN, kN, pkt + 4, 4000);
            QVERIFY(n > 0);
            pkt[0] = uint8_t(f >> 8); pkt[1] = uint8_t(f);
            const uint16_t ts = uint16_t(f * 20);
            pkt[2] = uint8_t(ts >> 8); pkt[3] = uint8_t(ts);
            jb.pushPacket(reinterpret_cast<const char*>(pkt), 4 + n);
            const auto r = jb.pop(out.data());
            if (f < 50) continue;  // settle
            for (int i = 0; i < kN; ++i) {
                inAcc += double(pcm[f * kN + i]) * pcm[f * kN + i];
            }
            inN += kN;
            if (r == JitterBuffer::PopResult::Decoded) {
                for (int16_t v : out) outAcc += double(v) * v;
                outN += kN;
            }
        }
        opus_encoder_destroy(enc);
        QVERIFY(outN > inN / 2);
        const double inDb = 10.0 * std::log10(inAcc / double(inN) / (32768.0 * 32768.0));
        const double outDb = 10.0 * std::log10(outAcc / double(outN) / (32768.0 * 32768.0));
        qInfo("opus round trip: in %.2f dBFS, out %.2f dBFS", inDb, outDb);
        QVERIFY2(std::fabs(outDb - inDb) < 1.5,
                 qPrintable(QStringLiteral("in %1 out %2").arg(inDb).arg(outDb)));
    }
};

QTEST_GUILESS_MAIN(TestVoiceGain)
#include "test_voice_gain.moc"
