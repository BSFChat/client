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
    // Short chime for inbound chat messages from other users. Reuses the
    // join-sound asset so we don't ship a second WAV for a near-identical
    // purpose, but routed through its own QSoundEffect so an in-progress
    // voice-join chime doesn't get cut off by a message arrival.
    void playMessage();

private:
    void initSound(QSoundEffect& effect, const QByteArray& wavData);

    QSoundEffect m_joinSound;
    QSoundEffect m_leaveSound;
    QSoundEffect m_muteSound;
    QSoundEffect m_messageSound;

    // Temp files to hold WAV data (QSoundEffect needs a URL)
    std::unique_ptr<QTemporaryFile> m_joinFile;
    std::unique_ptr<QTemporaryFile> m_leaveFile;
    std::unique_ptr<QTemporaryFile> m_muteFile;
    std::unique_ptr<QTemporaryFile> m_messageFile;
};
