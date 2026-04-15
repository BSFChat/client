#pragma once

#include <QObject>
#include <QAudioSource>
#include <QAudioSink>
#include <QTimer>
#include <QMap>
#include <QByteArray>

#include <opus.h>
#include <vector>
#include <cstdint>

class AudioMixer;

class AudioEngine : public QObject {
    Q_OBJECT
public:
    explicit AudioEngine(QObject* parent = nullptr);
    ~AudioEngine();

    bool start();
    void stop();

    void setMuted(bool muted) { m_muted = muted; }
    void setDeafened(bool deafened) { m_deafened = deafened; }

    void receivePeerAudio(const QString& peerId, const QByteArray& opusFrame);
    void removePeer(const QString& peerId);

signals:
    void audioFrameReady(const QByteArray& opusFrame);

private:
    void onMicDataReady();
    void onPlaybackTick();

    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels = 1;
    static constexpr int kFrameSamples = 960;  // 20ms at 48kHz
    static constexpr int kFrameBytes = kFrameSamples * sizeof(int16_t);
    static constexpr int kMaxOpusPacket = 4000;

    QAudioSource* m_audioSource = nullptr;
    QIODevice* m_captureDevice = nullptr;
    QByteArray m_captureBuffer;

    QAudioSink* m_audioSink = nullptr;
    QIODevice* m_playbackDevice = nullptr;
    QTimer m_playbackTimer;

    OpusEncoder* m_encoder = nullptr;
    QMap<QString, OpusDecoder*> m_decoders;

    AudioMixer* m_mixer = nullptr;

    bool m_muted = false;
    bool m_deafened = false;
    uint16_t m_sequence = 0;
    bool m_running = false;
};
