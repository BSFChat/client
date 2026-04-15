#pragma once

#include <QObject>
#include <QSoundEffect>
#include <QBuffer>
#include <QTemporaryFile>
#include <memory>

// Plays notification sounds for voice chat events.
// Sounds are generated at startup and cached.
class NotificationSounds : public QObject {
    Q_OBJECT
public:
    explicit NotificationSounds(QObject* parent = nullptr);

    void playJoin();
    void playLeave();
    void playMute();

private:
    void initSound(QSoundEffect& effect, const QByteArray& wavData);

    QSoundEffect m_joinSound;
    QSoundEffect m_leaveSound;
    QSoundEffect m_muteSound;

    // Temp files to hold WAV data (QSoundEffect needs a URL)
    std::unique_ptr<QTemporaryFile> m_joinFile;
    std::unique_ptr<QTemporaryFile> m_leaveFile;
    std::unique_ptr<QTemporaryFile> m_muteFile;
};
