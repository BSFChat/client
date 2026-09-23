#include "voice/AudioEngine.h"
#include "voice/AudioPacketQueue.h"
#include "voice/AudioWorker.h"
#include "core/AudioDeviceStatus.h"
#include "core/AudioGainSettings.h"
#include "voice/IosAudioSession.h"

#include <QAudioDevice>
#include <QMediaDevices>
#include <QThread>

using bsfchat::voice::AudioPacketQueue;
using bsfchat::voice::VoiceAudioAction;

namespace {
// The one engine that currently owns the platform audio session.
// IosAudioSession's handler is a plain function pointer with no context
// argument, and there is never more than one voice session in this
// process, so a file-static is the honest representation rather than a
// shortcut. Written and read on the GUI thread only: start() and
// teardownThread() both run there, and the AVAudioSession observers are
// registered against [NSOperationQueue mainQueue].
AudioEngine* g_lifecycleOwner = nullptr;
} // namespace

AudioEngine::AudioEngine(QObject* parent)
    : QObject(parent)
    , m_queue(std::make_shared<AudioPacketQueue>())
{
    // The subscription nothing in this client had. Without it the
    // device in use was whatever startDevices() resolved at join and
    // stayed that way: AirPods connected mid-call were never used even
    // once macOS had made them the system default, and a device that
    // disappeared left a dead sink. See the "Device enumeration" note in
    // the header for why this instance lives on this thread.
    m_mediaDevices = new QMediaDevices(this);
    connect(m_mediaDevices, &QMediaDevices::audioInputsChanged,
            this, [this]() { pushDeviceSnapshot(true, true); });
    connect(m_mediaDevices, &QMediaDevices::audioOutputsChanged,
            this, [this]() { pushDeviceSnapshot(false, true); });

    // Volume and AGC. Read when a session starts (see start()) and
    // followed live, so a slider moved mid-call is heard as it moves.
    auto& gains = bsfchat::AudioGainSettings::instance();
    connect(&gains, &bsfchat::AudioGainSettings::changed,
            this, &AudioEngine::applyGainSettings);
    connect(&gains, &bsfchat::AudioGainSettings::peerGainChanged, this,
            [this](const QString& userId, float gain) {
                if (m_worker) m_queue->pushPeerGain(userId, gain);
            });

    // The UI's tap-to-resume. Routed through AudioDeviceStatus for the
    // same reason the device captions are: the producer and the consumer
    // have no owner in common.
    connect(&bsfchat::AudioDeviceStatus::instance(),
            &bsfchat::AudioDeviceStatus::resumeRequested,
            this, &AudioEngine::requestAudioResume);
}

void AudioEngine::applyGainSettings()
{
    if (!m_worker) return;
    const auto& gains = bsfchat::AudioGainSettings::instance();
    m_worker->setInputGain(gains.inputGain());
    m_worker->setOutputGain(gains.outputGain());
    m_worker->setAutoGain(gains.autoGain());
}

void AudioEngine::pushDeviceSnapshot(bool input, bool live)
{
    AudioWorker* worker = m_worker;
    // No pipeline to reconfigure. start() seeds the worker itself, from
    // the device set as it stands at that moment.
    if (!worker) return;

    const auto dir = input ? AudioWorker::Direction::Input
                           : AudioWorker::Direction::Output;
    const QList<QAudioDevice> devices = input ? QMediaDevices::audioInputs()
                                              : QMediaDevices::audioOutputs();
    const QAudioDevice def = input ? QMediaDevices::defaultAudioInput()
                                   : QMediaDevices::defaultAudioOutput();

    // A captured lambda rather than a slot invocation, deliberately:
    // QAudioDevice is not registered as a queued-connection metatype,
    // and capturing by value means it never has to be.
    if (live) {
        QMetaObject::invokeMethod(worker, [worker, dir, devices, def]() {
            worker->onSystemDevicesChanged(dir, devices, def);
        }, Qt::QueuedConnection);
    } else {
        QMetaObject::invokeMethod(worker, [worker, dir, devices, def]() {
            worker->setDeviceSnapshot(dir, devices, def);
        }, Qt::BlockingQueuedConnection);
    }
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
    // Same for volume: in force from the first frame, so a boosted or
    // attenuated user never hears one frame at unity on join.
    applyGainSettings();
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
    // Queued for the same reason as the rest: AudioDeviceStatus is read
    // by a QML binding and must only ever be touched from here.
    connect(m_worker, &AudioWorker::deviceInUseChanged, this,
            [](bool input, const QString& description) {
                auto& status = bsfchat::AudioDeviceStatus::instance();
                if (input) status.setInputInUse(description);
                else       status.setOutputInUse(description);
            }, Qt::QueuedConnection);

    // TimeCritical is the honest description of a 20ms deadline. Where
    // the platform refuses the hint (Linux without CAP_SYS_NICE) Qt
    // warns and runs at default priority, which is what we had before.
    m_thread->start(QThread::TimeCriticalPriority);

    // Seed the worker's view of the device set before it opens anything,
    // so the join resolves through exactly the same policy as every
    // later change rather than through a parallel path that only runs
    // once.
    pushDeviceSnapshot(true, false);
    pushDeviceSnapshot(false, false);

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

    // Per-user volumes for everyone who has one. Pushed from here, on the
    // GUI thread, before this function returns — and so ahead of any
    // packet from those users, since receivePeerAudio() runs on this
    // thread too. The first pump drains them before its first render.
    const auto& peerGains = bsfchat::AudioGainSettings::instance().peerGains();
    for (auto it = peerGains.cbegin(); it != peerGains.cend(); ++it)
        m_queue->pushPeerGain(it.key(), it.value());

    // Devices are open, so the lifecycle is Running. The Open action is
    // already done — startDevices() above IS the open — so the return
    // value is deliberately dropped here and only the state matters.
    m_lifecycle.onStart();
    g_lifecycleOwner = this;
    // Installed per session rather than once at startup, which is a
    // small departure from the advice in IosAudioSession.h. The reason
    // it is safe: an interruption that arrives with no session is a
    // no-op in the state machine anyway (VoiceAudioState::Stopped
    // answers None to everything), so the window the header warns about
    // has nothing to lose in it.
    bsfchat::ios_audio::setEventHandler(&AudioEngine::platformAudioEventTrampoline);
    publishAudioState();
    return true;
}

void AudioEngine::stop() {
    if (!m_thread) return;
    teardownThread();
}

void AudioEngine::teardownThread() {
    if (!m_thread) return;

    // Before the worker goes away, so a late notification cannot arrive
    // and try to reopen devices on a pipeline that is being destroyed.
    if (g_lifecycleOwner == this) {
        bsfchat::ios_audio::setEventHandler(nullptr);
        g_lifecycleOwner = nullptr;
    }
    m_lifecycle.onStop();

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

    // The settings dialog's "In use:" captions now describe a pipeline
    // that no longer exists. Cleared from here rather than from
    // stopDevices(), whose queued report would arrive after the worker
    // is gone and be discarded.
    bsfchat::AudioDeviceStatus::instance().setInputInUse(QString());
    bsfchat::AudioDeviceStatus::instance().setOutputInUse(QString());
    publishAudioState();
}

// ---------------------------------------------------------------------
// Platform audio lifecycle
// ---------------------------------------------------------------------

void AudioEngine::platformAudioEventTrampoline(
    bsfchat::ios_audio::SessionEvent event)
{
    if (g_lifecycleOwner) g_lifecycleOwner->onPlatformAudioEvent(event);
}

void AudioEngine::onPlatformAudioEvent(bsfchat::ios_audio::SessionEvent event)
{
    applyLifecycleAction(m_lifecycle.onEvent(event));
    publishAudioState();
}

void AudioEngine::requestAudioResume()
{
    applyLifecycleAction(m_lifecycle.onUserResumeRequested());
    publishAudioState();
}

void AudioEngine::applyLifecycleAction(VoiceAudioAction action)
{
    AudioWorker* worker = m_worker;
    if (!worker) return;

    switch (action) {
    case VoiceAudioAction::None:
    case VoiceAudioAction::Open:
    case VoiceAudioAction::Close:
        // Open and Close are performed by start()/stop() themselves;
        // seeing one here would mean the state machine and the caller
        // had got out of step.
        return;

    case VoiceAudioAction::Suspend:
        // Blocking: the UI is about to claim the microphone is closed,
        // and it must be closed before that claim is on screen.
        QMetaObject::invokeMethod(worker, [worker]() {
            worker->suspendDevices();
        }, Qt::BlockingQueuedConnection);
        return;

    case VoiceAudioAction::Resume: {
        bool ok = false;
        QMetaObject::invokeMethod(worker, [worker, &ok]() {
            ok = worker->resumeDevices();
        }, Qt::BlockingQueuedConnection);
        if (!ok) m_lifecycle.onResumeFailed();
        return;
    }

    case VoiceAudioAction::Rebuild: {
        // A media-services reset invalidates every audio object in the
        // process, so partial recovery is not an option: the encoder,
        // the jitter buffers and the session all go, and are built
        // again. The peer connections survive — nothing about them is
        // CoreAudio — so this is still not a disconnect.
        qWarning("[voice] rebuilding the audio pipeline after a media "
                 "services reset");
        bool ok = false;
        QMetaObject::invokeMethod(worker, [worker, &ok]() {
            worker->stopDevices();
            ok = worker->startDevices();
        }, Qt::BlockingQueuedConnection);
        if (!ok) {
            m_lifecycle.onResumeFailed();
            return;
        }
        // The worker forgot the gain settings and per-peer volumes along
        // with everything else it tore down.
        applyGainSettings();
        const auto& peerGains = bsfchat::AudioGainSettings::instance().peerGains();
        for (auto it = peerGains.cbegin(); it != peerGains.cend(); ++it)
            m_queue->pushPeerGain(it.key(), it.value());
        return;
    }

    case VoiceAudioAction::ReevaluateRoute:
        // Queued, not blocking: nothing is waiting on the answer, and a
        // route change can arrive in a burst.
        QMetaObject::invokeMethod(worker, [worker]() {
            worker->reevaluateDevices();
        }, Qt::QueuedConnection);
        return;
    }
}

void AudioEngine::publishAudioState()
{
    // Deliberately NOT forcing the user's mute toggle on, which
    // docs/ios-voice.md §3 suggests. Overwriting a user's own setting
    // means having to guess later whether to put it back, and guessing
    // wrong leaves someone muted who does not know why. The requirement
    // behind the suggestion — that nobody believes they are being heard
    // when they are not — is met by closing the devices (so nothing is
    // transmitted), by the mic level falling to zero, and by this
    // banner saying so in words.
    bsfchat::AudioDeviceStatus::instance().setVoiceAudioState(
        !m_lifecycle.audioLive()
            && m_lifecycle.state() != bsfchat::voice::VoiceAudioState::Stopped,
        m_lifecycle.needsUserResume(),
        QString::fromLatin1(m_lifecycle.reason()));
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
