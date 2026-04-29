#include "voice/AudioEngine.h"
#include "voice/AudioMixer.h"
#include "voice/AndroidAudioRouting.h"

#include <QAudioFormat>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QSettings>
#include <QtEndian>
#include <cmath>
#include <cstring>

namespace {
// Look up an audio device (input or output) by its human-readable
// description. Returns a null QAudioDevice if nothing matches — caller
// should fall back to QMediaDevices::defaultAudio{Input,Output}().
QAudioDevice findInputByDescription(const QString& desc) {
    if (desc.isEmpty()) return {};
    for (const auto& d : QMediaDevices::audioInputs()) {
        if (d.description() == desc) return d;
    }
    return {};
}
QAudioDevice findOutputByDescription(const QString& desc) {
    if (desc.isEmpty()) return {};
    for (const auto& d : QMediaDevices::audioOutputs()) {
        if (d.description() == desc) return d;
    }
    return {};
}
} // namespace

AudioEngine::AudioEngine(QObject* parent)
    : QObject(parent)
    , m_mixer(new AudioMixer)
{
    m_playbackTimer.setInterval(20); // 20ms tick
    connect(&m_playbackTimer, &QTimer::timeout, this, &AudioEngine::onPlaybackTick);
}

AudioEngine::~AudioEngine() {
    stop();
    delete m_mixer;
}

bool AudioEngine::start() {
    if (m_running) return true;

    // Flip Android into VoIP mode + route to speakerphone BEFORE we
    // open QAudioSource. Done in MODE_NORMAL, Android routes capture
    // to a "normal" profile that prefers earpiece for playback and
    // skips hardware AEC — switching the mode after the device is
    // open doesn't re-plumb the routing graph. No-op off Android.
    bsfchat::audio_routing::enterVoiceMode();

    // Initialize Opus encoder
    int err;
    m_encoder = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !m_encoder) {
        qWarning("Failed to create Opus encoder: %s", opus_strerror(err));
        return false;
    }
    opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(32000));
    opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    // Audio format: 48kHz, mono, 16-bit signed
    QAudioFormat format;
    format.setSampleRate(kSampleRate);
    format.setChannelCount(kChannels);
    format.setSampleFormat(QAudioFormat::Int16);

    // Honour the user's selection from Client Settings → Audio; fall back
    // to the OS default if the saved preference isn't present (device
    // unplugged, renamed, etc).
    QSettings prefs("BSFChat", "BSFChat");
    QString preferredIn  = prefs.value("audio/inputDevice").toString();
    QString preferredOut = prefs.value("audio/outputDevice").toString();

    auto inputDevice = findInputByDescription(preferredIn);
    if (inputDevice.isNull()) {
        if (!preferredIn.isEmpty()) {
            qWarning("[voice] Preferred input '%s' not found — using system default",
                     qPrintable(preferredIn));
        }
        inputDevice = QMediaDevices::defaultAudioInput();
    }
    if (inputDevice.isNull()) {
        qWarning("[voice] No audio input device available");
    } else {
        qInfo("[voice] Input device: %s", qPrintable(inputDevice.description()));
        m_audioSource = new QAudioSource(inputDevice, format, this);
        m_captureDevice = m_audioSource->start();
        if (m_captureDevice) {
            connect(m_captureDevice, &QIODevice::readyRead, this, &AudioEngine::onMicDataReady);
        } else {
            qWarning("[voice] QAudioSource::start() returned null — "
                     "macOS likely still denying microphone access");
        }
        // QAudioSource has a State enum we can peek at for a sanity check.
        qInfo("[voice] QAudioSource initial state=%d error=%d",
              int(m_audioSource->state()), int(m_audioSource->error()));
    }

    // Start playback — same device-selection logic as input.
    auto outputDevice = findOutputByDescription(preferredOut);
    if (outputDevice.isNull()) {
        if (!preferredOut.isEmpty()) {
            qWarning("[voice] Preferred output '%s' not found — using system default",
                     qPrintable(preferredOut));
        }
        outputDevice = QMediaDevices::defaultAudioOutput();
    }
    if (!outputDevice.isNull()) {
        qInfo("[voice] Output device: %s", qPrintable(outputDevice.description()));
        m_audioSink = new QAudioSink(outputDevice, format, this);
        m_playbackDevice = m_audioSink->start();
    }

    m_playbackTimer.start();
    m_running = true;
    m_sequence = 0;
    m_captureBuffer.clear();

    return true;
}

void AudioEngine::stop() {
    if (!m_running) return;
    m_running = false;

    m_playbackTimer.stop();

    if (m_audioSource) {
        m_audioSource->stop();
        delete m_audioSource;
        m_audioSource = nullptr;
        m_captureDevice = nullptr;
    }

    if (m_audioSink) {
        m_audioSink->stop();
        delete m_audioSink;
        m_audioSink = nullptr;
        m_playbackDevice = nullptr;
    }

    if (m_encoder) {
        opus_encoder_destroy(m_encoder);
        m_encoder = nullptr;
    }

    for (auto* dec : m_decoders) {
        opus_decoder_destroy(dec);
    }
    m_decoders.clear();

    // Hand VoIP mode back to the OS. Balanced against enterVoiceMode()
    // in start().
    bsfchat::audio_routing::exitVoiceMode();
}

void AudioEngine::onMicDataReady() {
    if (!m_captureDevice || !m_encoder) return;

    QByteArray chunk = m_captureDevice->readAll();
    m_captureBuffer.append(chunk);

    // Log the first N frames so we can confirm captured data actually has
    // amplitude. macOS TCC-denied mic returns all zeros silently.
    static int s_debugFrameCount = 0;
    if (s_debugFrameCount < 5 && chunk.size() > 0) {
        const int16_t* p = reinterpret_cast<const int16_t*>(chunk.constData());
        int n = chunk.size() / 2;
        int16_t mx = 0;
        for (int i = 0; i < n; ++i) {
            int16_t v = p[i] < 0 ? -p[i] : p[i];
            if (v > mx) mx = v;
        }
        qInfo("[voice] mic frame #%d: %d bytes, peak |sample|=%d (0=silent, 32767=clip)",
              s_debugFrameCount, int(chunk.size()), int(mx));
        s_debugFrameCount++;
    }

    while (m_captureBuffer.size() >= kFrameBytes) {
        const int16_t* pcm = reinterpret_cast<const int16_t*>(m_captureBuffer.constData());

        // ----- Mic transmit level -----
        // Linear RMS in int16 units, normalized to 0..1 by int16 max, then
        // log-compressed so the UI isn't dominated by the top 10 dB.
        // We always publish the level so clients can display mic input even
        // when muted (useful to confirm the device is live); but we also
        // force it to zero when muted so "you're transmitting" indicators
        // don't falsely light up.
        float level = 0.0f;
        if (!m_muted) {
            double acc = 0.0;
            for (int i = 0; i < kFrameSamples; ++i) {
                double s = pcm[i];
                acc += s * s;
            }
            float rms = static_cast<float>(std::sqrt(acc / kFrameSamples) / 32768.0);
            // Map to a perceptual 0..1: everything below ~-60 dBFS → 0,
            // above ~-10 dBFS → 1, linear-in-dB in between.
            constexpr float kFloor = -60.0f;
            constexpr float kCeil = -10.0f;
            float db = rms > 1e-6f ? 20.0f * std::log10(rms) : kFloor;
            level = (db - kFloor) / (kCeil - kFloor);
            if (level < 0.0f) level = 0.0f;
            if (level > 1.0f) level = 1.0f;
        }
        // EWMA smoothing — fast attack, slow release so the indicator
        // tracks voice onsets but doesn't twitch off between syllables.
        constexpr float kAttack = 0.6f;
        constexpr float kRelease = 0.15f;
        float alpha = level > m_smoothedLevel ? kAttack : kRelease;
        m_smoothedLevel = m_smoothedLevel + alpha * (level - m_smoothedLevel);
        emit micLevelChanged(m_smoothedLevel);

        // ----- Encode + transmit -----
        if (!m_muted) {
            unsigned char opusBuf[kMaxOpusPacket];
            int encoded = opus_encode(m_encoder, pcm, kFrameSamples, opusBuf, kMaxOpusPacket);

            if (encoded > 0) {
                // Build frame: 2 bytes sequence + 2 bytes timestamp_delta + opus data
                QByteArray frame;
                frame.resize(4 + encoded);
                // Big-endian sequence number
                frame[0] = static_cast<char>((m_sequence >> 8) & 0xFF);
                frame[1] = static_cast<char>(m_sequence & 0xFF);
                // Timestamp delta (20ms per frame)
                uint16_t ts = static_cast<uint16_t>(m_sequence * 20);
                frame[2] = static_cast<char>((ts >> 8) & 0xFF);
                frame[3] = static_cast<char>(ts & 0xFF);
                std::memcpy(frame.data() + 4, opusBuf, encoded);

                m_sequence++;
                emit audioFrameReady(frame);
            }
        }

        m_captureBuffer.remove(0, kFrameBytes);
    }
}

void AudioEngine::onPlaybackTick() {
    if (!m_playbackDevice) return;

    auto mixed = m_mixer->mix();

    if (!m_deafened && m_playbackDevice) {
        m_playbackDevice->write(
            reinterpret_cast<const char*>(mixed.data()),
            mixed.size() * sizeof(int16_t)
        );
    }
}

void AudioEngine::receivePeerAudio(const QString& peerId, const QByteArray& opusFrame) {
    if (opusFrame.size() < 5) return; // 4-byte header + at least 1 byte opus

    // Strip header
    const unsigned char* opusData = reinterpret_cast<const unsigned char*>(opusFrame.constData() + 4);
    int opusLen = opusFrame.size() - 4;

    // Get or create decoder for this peer
    if (!m_decoders.contains(peerId)) {
        int err;
        auto* dec = opus_decoder_create(kSampleRate, kChannels, &err);
        if (err != OPUS_OK || !dec) return;
        m_decoders[peerId] = dec;
    }

    auto* decoder = m_decoders[peerId];
    std::vector<int16_t> pcm(kFrameSamples);
    int decoded = opus_decode(decoder, opusData, opusLen, pcm.data(), kFrameSamples, 0);

    if (decoded > 0) {
        pcm.resize(decoded);
        // Peak-magnitude → 0..1 level, EWMA-smoothed per peer so the
        // UI ring doesn't flicker. Same vocabulary as micLevelChanged.
        int16_t peak = 0;
        for (auto v : pcm) {
            int16_t a = v < 0 ? int16_t(-(v + 1)) : v;
            if (a > peak) peak = a;
        }
        float raw = float(peak) / 32768.0f;
        float prev = m_peerLevels.value(peerId, 0.0f);
        float smoothed = prev * 0.75f + raw * 0.25f;
        m_peerLevels[peerId] = smoothed;
        emit peerLevelChanged(peerId, smoothed);

        m_mixer->addFrame(peerId, pcm);
    }
}

void AudioEngine::removePeer(const QString& peerId) {
    m_mixer->removePeer(peerId);
    if (m_decoders.contains(peerId)) {
        opus_decoder_destroy(m_decoders[peerId]);
        m_decoders.remove(peerId);
    }
}
