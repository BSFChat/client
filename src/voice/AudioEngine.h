#pragma once

// GUI-thread facade over the real-time audio pipeline.
//
// AudioEngine used to *be* the pipeline: it opened the mic and speaker,
// ran a 10ms pump timer, Opus-encoded capture, decoded and mixed every
// peer, and wrote to the sink — all on the Qt GUI thread. That put a
// hard 20ms deadline on the same thread that lays out QML, parses sync
// batches and JPEG-decodes incoming screen-share frames. Any one of
// those overrunning a frame period drained the sink and produced an
// audible click; a long one produced a dropout.
//
// The pipeline now lives on a dedicated thread (AudioWorker), and this
// class is the boundary. Its public API is unchanged, deliberately, so
// VoiceEngine did not have to learn about threads.
//
// Threading model
// ---------------
//                GUI thread                 |      audio thread
//   -----------------------------------------+---------------------------
//   VoiceEngine::setMuted ------------------>| atomic<bool>  (no hop)
//   receivePeerAudio / removePeer ----------->| AudioPacketQueue (mutex)
//   start() / stop() ----------------------- >| BlockingQueuedConnection
//   micLevelChanged   <----------------------| queued signal
//   peerLevelChanged  <----------------------| queued signal (throttled)
//   audioFrameReady   <----------------------| queued signal
//
// Three different mechanisms, because they want three different things:
// mute wants zero added latency, packets want bounded memory and no
// contention with the pump, and lifecycle wants a synchronous answer.
//
// What is still coupled to the GUI thread
// ---------------------------------------
// Two hops remain, both on purpose:
//
//  1. INGRESS. PeerConnectionManager's libdatachannel onMessage handler
//     already re-posts every received frame to the GUI thread with a
//     queued invokeMethod before emitting audioFrameReceived, so a
//     packet still transits the GUI thread on its way to our queue. A
//     GUI stall therefore still delivers packets in a burst. This is
//     benign where a playback stall was not: JitterBuffer exists to
//     absorb exactly this, and it raises its own playout target in
//     response. Bypassing it means pushing to the queue directly from
//     the libdatachannel callback, which touches PeerConnectionManager's
//     per-peer counters from another thread and is a separate change.
//
//  2. EGRESS. audioFrameReady is queued back to the GUI thread, where
//     PeerConnectionManager::sendAudioFrame does the SCTP send. That
//     method is not thread-safe — it reads m_dc, bumps m_framesSent,
//     and goes through sendOnDataChannel's per-peer rate-limit state —
//     so calling it from the audio thread would be a data race. The
//     consequence is that a GUI stall delays our *outbound* burst,
//     which the receiver's jitter buffer absorbs. Transmit jitter is
//     recoverable; a local sink underrun is not, and that is the one
//     this refactor had to fix.
//
// Device enumeration
// ------------------
// This class owns the pipeline's single QMediaDevices instance, and it
// owns it HERE rather than on the audio thread. QMediaDevices needs a
// thread with a running event loop and delivers its change
// notifications on the thread that owns it; the audio thread's event
// loop exists to service a 20ms deadline and dispatches the pump timer,
// the mic readyRead and a queue drain — nothing else belongs in it.
//
// So enumeration happens here, on the GUI thread, and the result is
// posted to the worker as a plain snapshot. QAudioDevice and QList are
// implicitly shared with atomic refcounts, so handing a copy to another
// thread is both cheap and safe.
//
// On macOS these notifications cover DEFAULT-device changes as well as
// plug and unplug: Qt's CoreAudio backend installs CoreAudio property
// listeners on kAudioHardwarePropertyDefaultInputDevice and
// kAudioHardwarePropertyDefaultOutputDevice next to
// kAudioHardwarePropertyDevices (qtmultimedia,
// src/multimedia/darwin/qdarwinaudiodevices.mm), and publishes the
// default first in the list with isDefault set — so switching the
// output in Control Centre reorders the list and signals. Nothing here
// polls.

#include <QObject>
#include <QByteArray>
#include <QString>

#include <memory>

#include "voice/DarwinVoiceLifecycle.h"

class AudioWorker;
class QMediaDevices;
class QThread;

namespace bsfchat::voice { class AudioPacketQueue; }

class AudioEngine : public QObject {
    Q_OBJECT
public:
    explicit AudioEngine(QObject* parent = nullptr);
    ~AudioEngine() override;

    // Spins up the audio thread and opens the devices on it. Blocks
    // until the thread reports back, so the return value means the same
    // thing it always did. False = the pipeline is not running.
    bool start();
    // Tears the pipeline and the thread down, deterministically: devices
    // are closed on the audio thread, then the thread is joined. Safe to
    // call when never started, and safe to call twice.
    void stop();

    // Callable at any time from the GUI thread, started or not.
    void setMuted(bool muted);
    void setDeafened(bool deafened);

    // Hands one received wire frame to the audio thread. Cheap: a queue
    // push under a mutex held for the push alone.
    void receivePeerAudio(const QString& peerId, const QByteArray& opusFrame);
    // Queued behind that peer's outstanding packets, so removal cannot
    // overtake them and resurrect the jitter buffer we just destroyed.
    void removePeer(const QString& peerId);

    // ---- platform audio lifecycle (Apple) ----
    //
    // This class owns the AVAudioSession event handler, as
    // docs/ios-voice.md §3 says it should, for one reason: the handler
    // runs on the Qt main thread and everything it has to do touches
    // AudioWorker, which is audio-thread-affine. This is the only object
    // that already has the machinery to cross that boundary correctly,
    // and duplicating it anywhere else would be duplicating the part
    // that is easy to get subtly wrong.
    //
    // The handler is a plain function pointer (IosAudioSession.h), so it
    // reaches us through a file-static instance pointer. Installed by
    // start(), cleared by teardownThread(); only ever one voice session
    // at a time.
    void onPlatformAudioEvent(bsfchat::ios_audio::SessionEvent event);
    // The user tapped the resume affordance. No-op unless the lifecycle
    // is parked waiting for exactly that.
    void requestAudioResume();

signals:
    void audioFrameReady(const QByteArray& opusFrame);
    // 0..1 smoothed RMS of the most recent 20ms mic frame. Emits zero
    // when muted. Use as a UI transmit-level indicator. Emitted at the
    // frame rate (50Hz).
    void micLevelChanged(float level);
    // Smoothed audio level (0..1) from the most recent decoded frames
    // of `peerId`. Used by the UI to pulse a speaking ring around each
    // peer's participant tile. Rate-limited to ~12.5Hz — see
    // AudioWorker::kPeerLevelEmitFrames.
    void peerLevelChanged(const QString& peerId, float level);

private:
    // Joins the audio thread and disposes of the worker. Used by both
    // stop() and the failure path in start().
    void teardownThread();

    // Performs one VoiceAudioAction against the worker, marshalling to
    // the audio thread the way the rest of this class does.
    void applyLifecycleAction(bsfchat::voice::VoiceAudioAction action);
    // Pushes the lifecycle's current view to AudioDeviceStatus, which is
    // what the UI reads.
    void publishAudioState();
    // Trampoline for IosAudioSession's plain-function-pointer handler.
    static void platformAudioEventTrampoline(
        bsfchat::ios_audio::SessionEvent event);

    // Pushes AudioGainSettings' input/output gain and AGC switch into the
    // worker. No-op when there is no worker; start() calls it on the new
    // one before the thread runs.
    void applyGainSettings();

    // Enumerate one direction on this thread and hand the result to the
    // worker. `live` distinguishes the seeding call made before
    // startDevices() — blocking, and never restarts anything — from a
    // QMediaDevices change notification, which is queued and may
    // restart that direction.
    void pushDeviceSnapshot(bool input, bool live);

    // How long to wait for the audio thread to finish before giving up
    // and terminating it. Nothing on that thread can block: QAudioSink
    // writes and QAudioSource reads are both non-blocking (they return a
    // short count instead), and the packet queue's lock is never held
    // across work. So exceeding this means something is genuinely wedged
    // in a driver, and hanging the GUI on a leave-call is worse than
    // leaking the thread.
    static constexpr int kThreadJoinTimeoutMs = 2000;

    QThread* m_thread = nullptr;
    AudioWorker* m_worker = nullptr;
    // Lives for the life of the engine, not of a session: the device
    // set is worth watching from the moment there is a pipeline to feed.
    QMediaDevices* m_mediaDevices = nullptr;
    // Authoritative copy of the mic/output gates, held here rather than
    // only in the worker. The worker is created by start() and destroyed
    // by stop(), so without this a setMuted() that arrived before start()
    // — or between a stop() and a restart — would be silently dropped
    // and the mic would come up live. Pushed into the worker as part of
    // bringing it up.
    bool m_muted = false;
    bool m_deafened = false;
    // Shared with the worker so the queue's lifetime does not depend on
    // the teardown order of two objects on two different threads.
    std::shared_ptr<bsfchat::voice::AudioPacketQueue> m_queue;

    // GUI thread only, like everything else in this class. Off Apple
    // nothing ever feeds it an event and it stays in Running for the
    // life of the session, which costs one enum's worth of memory and
    // keeps the code free of a second #ifdef.
    bsfchat::voice::DarwinVoiceLifecycle m_lifecycle;
};
