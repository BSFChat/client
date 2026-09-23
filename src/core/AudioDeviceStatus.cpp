#include "core/AudioDeviceStatus.h"

namespace bsfchat {

AudioDeviceStatus& AudioDeviceStatus::instance()
{
    // Function-local static: constructed on first use, which is either
    // Settings' constructor or the first voice join, both on the GUI
    // thread. Never destroyed before exit, which is what we want — the
    // settings dialog can outlive any number of voice sessions.
    static AudioDeviceStatus s_instance;
    return s_instance;
}

void AudioDeviceStatus::setInputInUse(const QString& description)
{
    if (m_input == description) return;
    m_input = description;
    emit changed();
}

void AudioDeviceStatus::setOutputInUse(const QString& description)
{
    if (m_output == description) return;
    m_output = description;
    emit changed();
}

void AudioDeviceStatus::setVoiceAudioState(bool suspended, bool needsResume,
                                           const QString& reason)
{
    if (m_suspended == suspended && m_needsResume == needsResume
        && m_reason == reason) {
        return;
    }
    m_suspended = suspended;
    m_needsResume = needsResume;
    m_reason = reason;
    emit voiceAudioStateChanged();
}

} // namespace bsfchat
