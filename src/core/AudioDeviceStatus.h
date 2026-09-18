#pragma once

// Which audio devices the voice pipeline is ACTUALLY using right now.
//
// Settings publishes the user's *preference*; this publishes the
// *outcome*, and the two legitimately differ: the preference may be
// "follow the system default", or the chosen device may have gone away
// and been fallen back on. The settings dialog shows this under each
// combo box, so "why is this not coming out of my AirPods" has an
// answer on screen instead of only in the log.
//
// A process-wide singleton rather than a member of something, because
// the producer and the consumer have no owner in common: AudioEngine is
// created lazily inside VoiceEngine::start() and destroyed on leave,
// while Settings is a QML singleton that exists for the life of the
// process. Threading a pointer between them through VoiceEngine,
// ServerManager and App to carry two strings would be a much larger
// change than the strings are worth.
//
// GUI THREAD ONLY. The audio thread is where the devices are actually
// opened; AudioEngine marshals its worker's reports across with a queued
// connection and calls in from the GUI thread, exactly as it already
// does for mic and peer levels.

#include <QObject>
#include <QString>

namespace bsfchat {

class AudioDeviceStatus : public QObject {
    Q_OBJECT
public:
    static AudioDeviceStatus& instance();

    QString inputInUse() const { return m_input; }
    QString outputInUse() const { return m_output; }

    // Empty clears the caption — no voice session, or that half of the
    // pipeline never opened.
    void setInputInUse(const QString& description);
    void setOutputInUse(const QString& description);

signals:
    // One signal for both, because the only consumer re-reads both.
    void changed();

private:
    AudioDeviceStatus() = default;

    QString m_input;
    QString m_output;
};

} // namespace bsfchat
