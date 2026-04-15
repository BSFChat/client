#include "voice/AudioEngine.h"
#include "voice/AudioMixer.h"

#include <QAudioFormat>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QtEndian>
#include <cstring>

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

    // Start capture
    auto inputDevice = QMediaDevices::defaultAudioInput();
    if (inputDevice.isNull()) {
        qWarning("No audio input device available");
    } else {
        m_audioSource = new QAudioSource(inputDevice, format, this);
        m_captureDevice = m_audioSource->start();
        if (m_captureDevice) {
            connect(m_captureDevice, &QIODevice::readyRead, this, &AudioEngine::onMicDataReady);
        }
    }

    // Start playback
    auto outputDevice = QMediaDevices::defaultAudioOutput();
    if (!outputDevice.isNull()) {
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
}

void AudioEngine::onMicDataReady() {
    if (!m_captureDevice || !m_encoder) return;

    m_captureBuffer.append(m_captureDevice->readAll());

    while (m_captureBuffer.size() >= kFrameBytes) {
        if (!m_muted) {
            // Encode the frame
            const int16_t* pcm = reinterpret_cast<const int16_t*>(m_captureBuffer.constData());
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
