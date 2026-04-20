#include "voice/NotificationSounds.h"
#include "voice/SoundGenerator.h"

#include <QUrl>

NotificationSounds::NotificationSounds(QObject* parent)
    : QObject(parent)
{
    // Generate sounds and write to temp files (QSoundEffect needs file URLs)
    auto writeTemp = [](const QByteArray& wav) -> std::unique_ptr<QTemporaryFile> {
        auto file = std::make_unique<QTemporaryFile>();
        file->setFileTemplate(file->fileTemplate() + ".wav");
        if (file->open()) {
            file->write(wav);
            file->flush();
        }
        return file;
    };

    m_joinFile = writeTemp(SoundGenerator::generateJoinSound());
    m_leaveFile = writeTemp(SoundGenerator::generateLeaveSound());
    m_muteFile = writeTemp(SoundGenerator::generateMuteSound());
    // Reuse the join-chime waveform for chat-message notifications; a
    // dedicated QSoundEffect + backing file means a message that arrives
    // mid-voice-join won't truncate the voice chime.
    m_messageFile = writeTemp(SoundGenerator::generateJoinSound());

    if (m_joinFile) m_joinSound.setSource(QUrl::fromLocalFile(m_joinFile->fileName()));
    if (m_leaveFile) m_leaveSound.setSource(QUrl::fromLocalFile(m_leaveFile->fileName()));
    if (m_muteFile) m_muteSound.setSource(QUrl::fromLocalFile(m_muteFile->fileName()));
    if (m_messageFile) m_messageSound.setSource(QUrl::fromLocalFile(m_messageFile->fileName()));

    m_joinSound.setVolume(0.5f);
    m_leaveSound.setVolume(0.5f);
    m_muteSound.setVolume(0.3f);
    // Quieter than the voice-join chime — chat notifications are more
    // frequent and shouldn't startle.
    m_messageSound.setVolume(0.35f);
}

void NotificationSounds::playJoin() {
    m_joinSound.play();
}

void NotificationSounds::playLeave() {
    m_leaveSound.play();
}

void NotificationSounds::playMute() {
    m_muteSound.play();
}

void NotificationSounds::playMessage() {
    m_messageSound.play();
}
