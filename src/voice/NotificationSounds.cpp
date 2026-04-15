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

    if (m_joinFile) m_joinSound.setSource(QUrl::fromLocalFile(m_joinFile->fileName()));
    if (m_leaveFile) m_leaveSound.setSource(QUrl::fromLocalFile(m_leaveFile->fileName()));
    if (m_muteFile) m_muteSound.setSource(QUrl::fromLocalFile(m_muteFile->fileName()));

    m_joinSound.setVolume(0.5f);
    m_leaveSound.setVolume(0.5f);
    m_muteSound.setVolume(0.3f);
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
