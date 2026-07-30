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

#include <opus.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "voice/AudioPacketQueue.h"

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

    // ---- audio-thread only, invoked via BlockingQueuedConnection ----

    // Opens the mic and speaker and starts the pump. Returns false only
    // when the Opus encoder could not be created; a missing or denied
    // device is reported by warning and leaves that half of the pipeline
    // inert, matching the pre-threading behaviour.
    bool startDevices();
    // Tears down every thread-affine resource: devices, pump timer,
    // encoder, and all per-peer jitter buffers. Idempotent.
    void stopDevices();

signals:
    void audioFrameReady(const QByteArray& opusFrame);
    void micLevelChanged(float level);
    void peerLevelChanged(const QString& peerId, float level);

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
    uint16_t m_sequence = 0;
    bool m_started = false;

    // EWMA of frame RMS so the UI dot doesn't flicker at the Opus tick rate.
    float m_smoothedLevel = 0.0f;

    // Counts the first few captured frames for the "is the mic actually
    // producing samples" diagnostic. Per-instance rather than a
    // function-local static: see onMicDataReady().
    int m_debugFrameCount = 0;
};
