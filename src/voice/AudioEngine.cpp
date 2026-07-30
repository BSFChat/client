#include "voice/AudioEngine.h"
#include "voice/AudioPacketQueue.h"
#include "voice/AudioWorker.h"

#include <QThread>

using bsfchat::voice::AudioPacketQueue;

AudioEngine::AudioEngine(QObject* parent)
    : QObject(parent)
    , m_queue(std::make_shared<AudioPacketQueue>())
{
}

AudioEngine::~AudioEngine() {
    stop();
}

bool AudioEngine::start() {
    if (m_thread) return true;

    m_thread = new QThread();
    m_thread->setObjectName(QStringLiteral("bsfchat-audio"));

    // No parent: a QObject cannot be moved to another thread while it
    // has one, and the worker must be affine to the audio thread.
    m_worker = new AudioWorker(m_queue);
    // Seed the gates before the thread exists, so a mute that was set
    // while we were stopped is in force from the very first mic frame
    // rather than one applyMicGate() later.
    m_worker->setMuted(m_muted);
    m_worker->setDeafened(m_deafened);
    m_worker->moveToThread(m_thread);
    // Canonical worker-object idiom: the worker deletes itself inside
    // its own thread once the event loop exits, so we never destroy an
    // audio-thread QObject from the GUI thread.
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    // Every signal below crosses from the audio thread to this object,
    // which is GUI-affine, so Qt::AutoConnection would already resolve
    // to a queued connection at emit time. Stated explicitly because
    // "must not be direct" is a correctness requirement here, not a
    // preference: peerLevelChanged and micLevelChanged land in QML
    // property bindings, and audioFrameReady lands in
    // PeerConnectionManager::sendAudioFrame, none of which may run on
    // the audio thread.
    connect(m_worker, &AudioWorker::audioFrameReady,
            this, &AudioEngine::audioFrameReady, Qt::QueuedConnection);
    connect(m_worker, &AudioWorker::micLevelChanged,
            this, &AudioEngine::micLevelChanged, Qt::QueuedConnection);
    connect(m_worker, &AudioWorker::peerLevelChanged,
            this, &AudioEngine::peerLevelChanged, Qt::QueuedConnection);

    // TimeCritical is the honest description of a 20ms deadline. Where
    // the platform refuses the hint (Linux without CAP_SYS_NICE) Qt
    // warns and runs at default priority, which is what we had before.
    m_thread->start(QThread::TimeCriticalPriority);

    // Open the devices on the audio thread and wait for the verdict.
    // Blocking is safe in both directions: this thread is not the audio
    // thread, and the audio thread cannot be waiting on us. The event is
    // delivered even though the loop may not be up yet — invokeMethod
    // posts and then waits on the semaphore.
    bool ok = false;
    QMetaObject::invokeMethod(m_worker, [w = m_worker, &ok]() {
        ok = w->startDevices();
    }, Qt::BlockingQueuedConnection);

    if (!ok) {
        // Encoder creation failed; don't leave a thread running for a
        // pipeline that will never produce anything. teardownThread()
        // leaves us in the never-started state, so a later stop() from
        // VoiceEngine is a no-op and a later start() can retry cleanly.
        teardownThread();
        return false;
    }
    return true;
}

void AudioEngine::stop() {
    if (!m_thread) return;
    teardownThread();
}

void AudioEngine::teardownThread() {
    if (!m_thread) return;

    if (m_worker) {
        // Close the devices, kill the pump timer and destroy the Opus
        // encoder and every jitter buffer — all on the thread that
        // created them. Doing this before quit() rather than in
        // ~AudioWorker is what guarantees no QAudioSink is ever
        // destroyed from the wrong thread.
        QMetaObject::invokeMethod(m_worker, [w = m_worker]() {
            w->stopDevices();
        }, Qt::BlockingQueuedConnection);
    }

    m_thread->quit();
    if (!m_thread->wait(kThreadJoinTimeoutMs)) {
        qWarning("[voice] audio thread did not exit within %dms — terminating",
                 kThreadJoinTimeoutMs);
        m_thread->terminate();
        m_thread->wait();
    }
    // The worker deleted itself via the finished() -> deleteLater
    // connection, which QThread flushes before wait() returns.
    m_worker = nullptr;
    delete m_thread;
    m_thread = nullptr;

    // Anything the network pushed while we were shutting down.
    if (m_queue) m_queue->clear();
}

void AudioEngine::setMuted(bool muted) {
    m_muted = muted;
    if (m_worker) m_worker->setMuted(muted);
}

void AudioEngine::setDeafened(bool deafened) {
    m_deafened = deafened;
    if (m_worker) m_worker->setDeafened(deafened);
}

void AudioEngine::receivePeerAudio(const QString& peerId,
                                   const QByteArray& opusFrame) {
    // Drop on the floor when there is no pipeline to consume it, rather
    // than filling the queue to its cap and back-pressuring nothing.
    if (!m_worker) return;
    m_queue->push(AudioPacketQueue::Kind::Audio, peerId, opusFrame);
}

void AudioEngine::removePeer(const QString& peerId) {
    if (!m_worker) return;
    m_queue->push(AudioPacketQueue::Kind::RemovePeer, peerId);
}
