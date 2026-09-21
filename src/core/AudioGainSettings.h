#pragma once

// The live gain settings the voice pipeline applies: input volume,
// output volume, whether the capture AGC runs, and per-user volume.
//
// Same shape, and same reason for being a process-wide singleton, as
// AudioDeviceStatus next door: Settings (the producer) is a QML
// singleton for the life of the process, while AudioEngine (the
// consumer) is created inside VoiceEngine::start() and destroyed on
// leave, and the two share no owner. Settings loads the stored values
// into this at construction and pushes every change; AudioEngine reads
// it when a session starts and listens for changes during one, so a
// slider moved mid-call is heard immediately.
//
// Values are LINEAR gains, already mapped from the stored percentages
// by core/AudioVolume.h — the audio side never sees a percentage.
//
// GUI THREAD ONLY. AudioEngine carries the values across to the audio
// thread: the global gains as atomics on the worker (the same route as
// mute), per-user gains through the AudioPacketQueue (the same route as
// peer removal, so they are ordered against that peer's packets).

#include <QHash>
#include <QObject>
#include <QString>

namespace bsfchat {

class AudioGainSettings : public QObject {
    Q_OBJECT
public:
    static AudioGainSettings& instance();

    float inputGain() const { return m_inputGain; }
    float outputGain() const { return m_outputGain; }
    bool autoGain() const { return m_autoGain; }
    // 1.0 for anyone without an explicit setting.
    float peerGain(const QString& userId) const { return m_peerGains.value(userId, 1.0f); }
    // Only users whose gain is not unity.
    const QHash<QString, float>& peerGains() const { return m_peerGains; }

    void setInputGain(float gain);
    void setOutputGain(float gain);
    void setAutoGain(bool on);
    // Unity removes the entry.
    void setPeerGain(const QString& userId, float gain);

signals:
    // Input gain, output gain or the AGC switch.
    void changed();
    void peerGainChanged(const QString& userId, float gain);

private:
    AudioGainSettings() = default;

    float m_inputGain = 1.0f;
    float m_outputGain = 1.0f;
    bool m_autoGain = true;
    QHash<QString, float> m_peerGains;
};

} // namespace bsfchat
