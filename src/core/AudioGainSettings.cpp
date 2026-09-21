#include "core/AudioGainSettings.h"

namespace bsfchat {

AudioGainSettings& AudioGainSettings::instance()
{
    // Constructed on first use (Settings' constructor, on the GUI
    // thread) and never destroyed before exit — see AudioDeviceStatus.
    static AudioGainSettings s_instance;
    return s_instance;
}

void AudioGainSettings::setInputGain(float gain)
{
    if (m_inputGain == gain) return;
    m_inputGain = gain;
    emit changed();
}

void AudioGainSettings::setOutputGain(float gain)
{
    if (m_outputGain == gain) return;
    m_outputGain = gain;
    emit changed();
}

void AudioGainSettings::setAutoGain(bool on)
{
    if (m_autoGain == on) return;
    m_autoGain = on;
    emit changed();
}

void AudioGainSettings::setPeerGain(const QString& userId, float gain)
{
    if (userId.isEmpty()) return;
    if (peerGain(userId) == gain) return;
    if (gain == 1.0f) {
        m_peerGains.remove(userId);
    } else {
        m_peerGains.insert(userId, gain);
    }
    emit peerGainChanged(userId, gain);
}

} // namespace bsfchat
