#pragma once

// The real-time half of the voice pipeline. Everything in this class
// runs on the dedicated audio thread owned by AudioEngine — capture,
// Opus encode, jitter buffering, Opus decode, mixing and sink writes.
// Nothing here is called from the GUI thread except:
//
//   * setMuted()/setDeafened(), which are atomics (see below), and
//   * the AudioPacketQueue, which is explicitly thread-safe.
//
// Everything else — startDevices(), stopDevices(), the pump timer, the
// QAudioSource/QAudioSink pair, the per-peer JitterBuffers and the
// AudioMixer — is audio-thread-only state and must stay that way.
//
// Device affinity
// ---------------
// QAudioSource and QAudioSink must be created and driven on one thread;
// Qt does not diagnose a violation, it just misbehaves quietly. Both
// are constructed inside startDevices() and destroyed inside
// stopDevices(), and AudioEngine invokes each of those with a
// BlockingQueuedConnection so they always execute on the audio thread.
// The pump QTimer is heap-allocated for the same reason: a QTimer must
// be started and stopped on the thread that owns it, so it is created
// and deleted alongside the devices rather than living as a member that
// the GUI thread might construct.
//
// Live device changes
// -------------------
// The device in use used to be resolved once, in startDevices(), and
// then never again: nothing in the client subscribed to
// QMediaDevices::audio{Inputs,Outputs}Changed. AirPods that connected
// after join were never used even once macOS had made them the
// system-wide default, and a device that disappeared mid-call left a
// dead sink behind.
//
// It is now re-resolved on every change notification. Two rules keep
// that from being worse than the freeze it replaces:
//
//   * ONE DIRECTION AT A TIME. openSink()/closeSink() and their capture
//     counterparts touch nothing else — not the Opus encoder, not a
//     single JitterBuffer or its playout target, not the pump timer,
//     not the voice session. Swapping headsets must not cost the call.
//   * NEVER FOR NOTHING. AudioDevicePolicy answers Keep when the
//     resolved device id is the one already open, and RestartDebounce
//     collapses the notification burst a Bluetooth connect produces
//     into one restart per direction per window. Each restart is an
//     audible gap, so the bar for performing one is that the device
//     actually changed.
//
// The enumeration itself stays on the GUI thread (see AudioEngine):
// QMediaDevices wants a thread with an event loop, and this one's
// exists to service a 20ms deadline.
//
// Jitter buffer ownership
// -----------------------
// JitterBuffer is not internally synchronised, by design. The rule here
// is absolute: the JitterBuffer map, and every buffer in it, is touched
// ONLY on the audio thread. Received packets reach it through
// AudioPacketQueue, drained at the top of each pump. There is no mutex
// on pop(), and there must never need to be.

#include <QObject>
#include <QMap>
#include <QByteArray>
#include <QString>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QList>

#include <opus.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "voice/AudioPacketQueue.h"
#include "voice/AudioDevicePolicy.h"
#include "voice/VoiceGain.h"

class AudioMixer;
class QAudioSource;
class QAudioSink;
class QIODevice;
class QTimer;

namespace bsfchat::voice { class JitterBuffer; }

class AudioWorker : public QObject {
    Q_OBJECT
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels = 1;
    static constexpr int kFrameSamples = 960;  // 20ms at 48kHz
    static constexpr int kFrameBytes = kFrameSamples * sizeof(int16_t);
    static constexpr int kMaxOpusPacket = 4000;

    // Sink buffer depth, in 20ms frames.
    //
    // This used to be justified as "enough slack for a GUI-thread
    // hiccup", which was the honest description of a real hazard: the
    // pump ran on the GUI thread, so a QML relayout or a JPEG decode of
    // an incoming screen-share frame that took longer than
    // kSinkBufferFrames * 20ms drained the device and produced an
    // audible underrun. 100ms was chosen as a guess at the worst
    // tolerable GUI stall, and was never validated against one.
    //
    // That hazard is now gone: the pump runs on a dedicated
    // TimeCritical thread whose event loop dispatches only the pump
    // timer, the mic readyRead and a queue drain, none of which can take
    // more than a fraction of a frame period. The bound is no longer
    // "how long can the GUI stall" but "how long can the OS defer a
    // high-priority thread with nothing else to do", which is orders of
    // magnitude smaller.
    //
    // Kept at 5 rather than reduced, deliberately. 100ms of sink depth
    // is now generous headroom instead of a gamble, and the obvious
    // follow-up — dropping to 3 frames (60ms) to reclaim 40ms of
    // mouth-to-ear latency — trades that headroom for latency against
    // three different Qt Multimedia backends (CoreAudio, WASAPI,
    // PulseAudio/ALSA), each of which rounds setBufferSize() to its own
    // period granularity and underruns differently below it. That is a
    // change to make with a real device and a listener on each
    // platform, not blind. Latency is meanwhile absorbed where it
    // belongs: JitterBuffer's adaptive playout target (40-200ms).
    static constexpr int kSinkBufferFrames = 5;

    // Pump twice per frame period so a late timer tick still refills
    // before the device runs dry.
    static constexpr int kPumpIntervalMs = 10;
    // Upper bound on frames rendered in a single pump, so an absurd
    // bytesFree() (device reset, backend quirk) can't spin the loop.
    static constexpr int kMaxFramesPerPump = 16;
    // Compact the capture buffer once this many consumed bytes have
    // piled up at the front.
    static constexpr int kCaptureCompactBytes = 64 * kFrameBytes;

    // Emit peerLevelChanged once per this many rendered output frames.
    // Four frames = 80ms = 12.5Hz, which is well past what a pulsing
    // speaking ring can express and roughly the rate a display can show
    // anyway. At 50Hz with eight peers this signal alone was 400
    // cross-thread QMetaCallEvents per second, every one of them
    // carrying a QString, to animate something the eye reads as
    // "on". The value emitted is the maximum over the window, so a
    // short onset is never skipped — only the redundant intermediate
    // samples are.
    static constexpr int kPeerLevelEmitFrames = 4;

    // Window for coalescing device-change notifications, per direction.
    // A Bluetooth connect delivers several in a few hundred
    // milliseconds — the device appears, becomes the default, settles
    // its profile — and AirPods deliver more again when the microphone
    // opens. Acting on each is several audible gaps in a row for one
    // real change. 500ms is long enough to swallow a normal burst and
    // short enough that a deliberate switch in Control Centre still
    // feels immediate.
    static constexpr int kDeviceDebounceMs = 500;

    // Whether the OS already runs voice processing (gain control, and
    // usually noise suppression and echo cancellation) on the capture
    // stream. True on Android, where enterVoiceMode() switches to
    // MODE_IN_COMMUNICATION. When it does, SpeechAgc is NOT run whatever
    // the setting says: two AGCs in series each see the other's gain
    // changes as level changes and chase them, which pumps. The capture
    // limiter still runs everywhere — a limiter only ever turns peaks
    // down, so it cannot fight anything.
    //
    // Not yet verified on a device: whether Qt's Android QAudioSource
    // actually opens the VOICE_COMMUNICATION input preset under
    // MODE_IN_COMMUNICATION (which is what engages the platform AGC), or
    // only the routing. If a real Android sender turns out to be as quiet
    // as desktop was, this is the switch to revisit.
#ifdef Q_OS_ANDROID
    static constexpr bool kPlatformVoiceProcessing = true;
#else
    static constexpr bool kPlatformVoiceProcessing = false;
#endif

    // One-shot capture-level log line after this much detected speech
    // (500 frames = 10 s), and again at session end. See logLevelSummary().
    static constexpr int kLevelSummarySpeechFrames = 500;

    explicit AudioWorker(std::shared_ptr<bsfchat::voice::AudioPacketQueue> queue,
                         QObject* parent = nullptr);
    ~AudioWorker() override;

    // Both of these are called directly from the GUI thread rather than
    // marshalled. They are single bools read once per frame, and
    // push-to-talk should not have to wait behind whatever is already in
    // the audio thread's event queue for the mic to unmute — a queued
    // hop would add up to a pump period of latency to every PTT press
    // and release. Relaxed ordering is sufficient: there is nothing to
    // synchronise-with, and a mute that lands one 20ms frame late is
    // inaudible.
    void setMuted(bool muted) { m_muted.store(muted, std::memory_order_relaxed); }
    void setDeafened(bool d) { m_deafened.store(d, std::memory_order_relaxed); }

    // Same route as mute, for the same reasons: single values read once
    // per frame, and a slider should be heard as it moves rather than
    // after whatever is queued on the audio thread. Linear gains (see
    // core/AudioVolume.h for the percentage mapping). Each is applied as
    // a ramp from the previous frame's value, so a change is a fade.
    void setInputGain(float g) { m_inputGain.store(g, std::memory_order_relaxed); }
    void setOutputGain(float g) { m_outputGain.store(g, std::memory_order_relaxed); }
    void setAutoGain(bool on) { m_autoGain.store(on, std::memory_order_relaxed); }

    // ---- audio-thread only, invoked via BlockingQueuedConnection ----

    // Opens the mic and speaker and starts the pump. Returns false only
    // when the Opus encoder could not be created; a missing or denied
    // device is reported by warning and leaves that half of the pipeline
    // inert, matching the pre-threading behaviour.
    bool startDevices();
    // Tears down every thread-affine resource: devices, pump timer,
    // encoder, and all per-peer jitter buffers. Idempotent.
    void stopDevices();

    enum class Direction { Input, Output };

    // Hands the worker the GUI thread's view of the system's audio
    // devices. AudioEngine owns the QMediaDevices and does the
    // enumeration there, so this thread never calls into Qt Multimedia's
    // device layer behind the GUI thread's back.
    //
    // Both run ON the audio thread: AudioEngine invokes the first with a
    // BlockingQueuedConnection to seed the worker before startDevices(),
    // and the second with an ordinary QueuedConnection from the
    // QMediaDevices change signals. Neither is a slot — they are called
    // from inside a lambda posted by invokeMethod, which is what avoids
    // having to register QAudioDevice as a queued-connection metatype.
    void setDeviceSnapshot(Direction dir, QList<QAudioDevice> devices,
                           QAudioDevice defaultDevice);
    // Same, and then re-runs the device policy: restarts that direction
    // if, and only if, the resolved device id actually changed.
    // Debounced per direction.
    void onSystemDevicesChanged(Direction dir, QList<QAudioDevice> devices,
                                QAudioDevice defaultDevice);

signals:
    void audioFrameReady(const QByteArray& opusFrame);
    void micLevelChanged(float level);
    void peerLevelChanged(const QString& peerId, float level);
    // The description of the device this direction is now open on, empty
    // when nothing is. `input` rather than Direction because this one
    // really does cross threads as a signal, and a bool needs no
    // metatype registration. AudioEngine publishes it for the settings
    // dialog's "In use:" caption.
    void deviceInUseChanged(bool input, const QString& description);

private:
    void onMicDataReady();
    // Safety pump. How *much* it writes is decided by the sink's
    // bytesFree(), not by the timer.
    void pumpPlayback();
    // Moves everything the network side has queued into the jitter
    // buffers. Audio thread only.
    void ingestQueuedPackets();
    // Drains every peer's jitter buffer by exactly one frame and sums
    // the result into `out` (kFrameSamples samples).
    void renderMixedFrame(int16_t* out);
    // Retries the tail of a previous short write. Returns false when
    // the device is still full.
    bool flushPendingPlayback();
    bsfchat::voice::JitterBuffer* jitterFor(const QString& peerId);
    void dropPeer(const QString& peerId);
    // Capture and playback level summary to the log — measured levels,
    // never audio, and local only. Called off the per-frame path except
    // for the single one-shot after kLevelSummarySpeechFrames.
    void logLevelSummary(const char* when);
    // Runs the capture chain over m_captureFloat in place.
    void processCaptureFrame();

    // Resolve `dir` against the current snapshot and act on the answer.
    // The single place a device is chosen, at join and on every change.
    void evaluateDevice(Direction dir);
    // Snapshot -> policy input.
    QList<bsfchat::voice::DeviceInfo> deviceInfos(Direction dir) const;
    const QAudioDevice* snapshotDevice(Direction dir, const QString& id) const;
    // Open or close exactly ONE direction. These touch no shared
    // pipeline state: no encoder, no jitter buffers, no pump timer.
    void openSource(const QAudioDevice& device);
    void closeSource();
    void openSink(const QAudioDevice& device);
    void closeSink();
    void setInUse(Direction dir, const QString& id, const QString& description);

    std::shared_ptr<bsfchat::voice::AudioPacketQueue> m_queue;
    // Scratch for AudioPacketQueue::drain(), kept as a member so the
    // per-pump drain does not allocate.
    std::deque<bsfchat::voice::AudioPacketQueue::Item> m_inbox;

    QAudioSource* m_audioSource = nullptr;
    QIODevice* m_captureDevice = nullptr;
    QByteArray m_captureBuffer;
    // Read cursor into m_captureBuffer. Consuming a frame bumps this
    // index instead of memmove'ing the whole buffer down 1920 bytes
    // every 20ms; the buffer is compacted (or cleared outright, which
    // is the common case) once per drain.
    int m_captureHead = 0;
    std::vector<int16_t> m_captureFrame;  // one aligned 20ms mic frame
    std::vector<float> m_captureFloat;    // the same frame, being processed
    std::vector<int16_t> m_captureOut;    // processed frame, what is encoded

    // Capture chain: SpeechAgc -> input volume -> PeakLimiter. AUDIO
    // THREAD ONLY, and reset whenever a new input device opens — its
    // noise floor and level history belong to the old microphone.
    bsfchat::voice::SpeechAgc m_agc;
    bsfchat::voice::PeakLimiter m_captureLimiter;
    bool m_agcRan = false;            // whether the last frame used the AGC
    float m_appliedInputGain = 1.0f;  // input gain the last frame ended on
    float m_appliedOutputGain = 1.0f;

    // Per-user volume, fed by AudioPacketQueue::Kind::PeerGain. Kept
    // separately from m_jitter, and not dropped with the peer, so someone
    // who leaves and rejoins keeps the volume you gave them.
    struct PeerGain { float target = 1.0f; float applied = 1.0f; };
    QMap<QString, PeerGain> m_peerGains;

    // Level-summary counters. Increments only on the frame path.
    int64_t m_captureFrames = 0;
    int64_t m_captureLimitedFrames = 0;
    int64_t m_playbackFrames = 0;
    int64_t m_playbackLimitedFrames = 0;
    bool m_levelSummaryLogged = false;

    QAudioSink* m_audioSink = nullptr;
    QIODevice* m_playbackDevice = nullptr;
    QTimer* m_playbackTimer = nullptr;
    // Bytes handed to the mixer but not yet accepted by the device.
    QByteArray m_playbackPending;
    int m_playbackPendingHead = 0;
    std::vector<int16_t> m_playbackFrame;  // one mixed 20ms output frame
    std::vector<int16_t> m_peerFrame;      // one peer's 20ms frame

    OpusEncoder* m_encoder = nullptr;
    // One jitter buffer per peer; each owns its own Opus decoder,
    // because decoding has to happen at playout time for packet-loss
    // concealment to be interleaved correctly. AUDIO THREAD ONLY.
    QMap<QString, bsfchat::voice::JitterBuffer*> m_jitter;
    QMap<QString, float> m_peerLevels;         // smoothed 0..1 per peer
    QMap<QString, float> m_peerLevelPending;   // max since last emit
    int m_peerLevelFrames = 0;

    AudioMixer* m_mixer = nullptr;

    std::atomic<bool> m_muted{false};
    std::atomic<bool> m_deafened{false};
    std::atomic<float> m_inputGain{1.0f};
    std::atomic<float> m_outputGain{1.0f};
    std::atomic<bool> m_autoGain{true};
    uint16_t m_sequence = 0;
    bool m_started = false;

    // Identical for both directions and constant for the life of a
    // session; a member so a mid-call restart can reopen a device
    // without rebuilding it.
    QAudioFormat m_format;

    // The GUI thread's most recent view of the system's devices, per
    // direction, plus the device it named as the default.
    QList<QAudioDevice> m_inputSnapshot;
    QList<QAudioDevice> m_outputSnapshot;
    QAudioDevice m_defaultInput;
    QAudioDevice m_defaultOutput;

    // The persisted preference, read once in startDevices(). An empty
    // description means "follow the system default". The id is only ever
    // a tie-breaker between devices that share a description — ids are
    // not stable across reboots on every platform, so the description
    // stays authoritative.
    QString m_preferredInput;
    QString m_preferredOutput;
    QString m_inputHintId;
    QString m_outputHintId;

    // What is open right now. The id is what the policy compares; the
    // description is what the UI shows.
    QString m_inputInUseId;
    QString m_outputInUseId;
    QString m_inputInUseDesc;
    QString m_outputInUseDesc;

    bsfchat::voice::RestartDebounce m_inputDebounce{kDeviceDebounceMs};
    bsfchat::voice::RestartDebounce m_outputDebounce{kDeviceDebounceMs};
    // Created lazily on the audio thread, like the pump timer and for
    // the same reason: a QTimer must be started and stopped on the
    // thread that owns it. Single-shot, one per direction.
    QTimer* m_inputRestartTimer = nullptr;
    QTimer* m_outputRestartTimer = nullptr;

    // EWMA of frame RMS so the UI dot doesn't flicker at the Opus tick rate.
    float m_smoothedLevel = 0.0f;

    // Counts the first few captured frames for the "is the mic actually
    // producing samples" diagnostic. Per-instance rather than a
    // function-local static: see onMicDataReady().
    int m_debugFrameCount = 0;
};
