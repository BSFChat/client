#pragma once

// The seam between AudioWorker's frame arithmetic and whatever is
// actually talking to the sound hardware.
//
// AudioWorker used to hold a QAudioSource and a QAudioSink directly.
// That is still exactly what happens on Windows, Linux and Android —
// QtAudioBackend is those two objects and nothing else, and its
// behaviour is meant to be indistinguishable from the code it replaced.
// The seam exists for Darwin, where the second implementation
// (DarwinVpioBackend) is a single kAudioUnitSubType_VoiceProcessingIO
// unit doing BOTH directions, which is the whole point: VPIO takes its
// far-end reference for echo cancellation from its own output bus, so
// capture and playback cannot stay split across Qt and the audio unit.
// Cancellation only works if the audio being cancelled went out through
// the same unit.
//
// Shape of the interface
// ----------------------
// Deliberately Qt-Multimedia-shaped rather than neutral, because
// AudioWorker's pump is carefully reasoned about (see the drift
// discussion in AudioWorker.cpp) and rewriting it for this would be
// changing the part that works to accommodate the part that does not
// exist yet. So:
//
//   readCapture()  is readAll() with a caller-supplied buffer
//   bytesFree()    is QAudioSink::bytesFree(), and means the same thing:
//                  how much the device will accept right now
//   writeRender()  is QIODevice::write(), short writes and all
//
// The one addition is capturePolled(). Qt hands capture up through
// QIODevice::readyRead on the audio thread, which is a push. A CoreAudio
// input callback runs on a real-time thread we do not own and must never
// block, so it pushes into a ring and the audio thread pulls from it —
// which means the pump has to ask. capturePolled() is how a backend says
// which of the two it is, so the Qt path keeps its exact existing
// timing instead of being converted to polling for the benefit of a
// platform it does not run on.
//
// Threading
// ---------
// Every method here is called on the audio thread and only on the audio
// thread, including construction and destruction. A backend that has
// callbacks of its own is responsible for making them safe against that
// — see the ring buffer contract in AudioRingBuffer.h.

#include <QAudioDevice>
#include <QAudioFormat>
#include <QString>
#include <QtGlobal>

#include <functional>

#include "voice/AudioBackendSelect.h"

namespace bsfchat::voice {

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;

    virtual AudioBackendKind kind() const = 0;

    // True when the OS is already applying voice processing — echo
    // cancellation, noise suppression and gain control — to the capture
    // stream. AudioWorker uses this to stand its own SpeechAgc down:
    // two AGCs in series chase each other's gain changes and pump. This
    // is the runtime form of what AudioWorker::kPlatformVoiceProcessing
    // used to say at compile time, and it has to be runtime now because
    // the answer on macOS and iOS depends on which backend opened.
    virtual bool platformVoiceProcessing() const = 0;

    // True when the worker must call readCapture() from its pump rather
    // than waiting to be told. See the note above.
    virtual bool capturePolled() const = 0;

    // ---- capture ----

    // `device` is the resolved QAudioDevice from AudioDevicePolicy. A
    // backend that does not choose devices that way (iOS, where routing
    // is AVAudioSession's business and Qt enumerates exactly one
    // microphone) may ignore it, but must say so in its log line rather
    // than silently.
    virtual bool openCapture(const QAudioDevice& device,
                             const QAudioFormat& format) = 0;
    virtual void closeCapture() = 0;
    virtual bool captureOpen() const = 0;
    // Non-blocking. Returns bytes written into `dst`, 0 when there is
    // nothing, and never more than `maxBytes`.
    virtual qint64 readCapture(char* dst, qint64 maxBytes) = 0;

    // ---- render ----

    // `bufferBytes` is the device buffer depth AudioWorker wants
    // (kFrameBytes * kSinkBufferFrames). A hint: backends round it to
    // their own granularity, which is what QAudioSink::setBufferSize()
    // already did.
    virtual bool openRender(const QAudioDevice& device,
                            const QAudioFormat& format, int bufferBytes) = 0;
    virtual void closeRender() = 0;
    virtual bool renderOpen() const = 0;
    // How much the device will accept right now. 0 when it is full, or
    // when render is not open.
    virtual qint64 bytesFree() = 0;
    // Short writes are normal and are the caller's problem to carry
    // (AudioWorker::flushPendingPlayback). Negative means an error.
    virtual qint64 writeRender(const char* src, qint64 bytes) = 0;

    // The platform says the audio route changed under us (iOS; see
    // IosAudioSession.h, which is the ONLY route signal that platform
    // has — QMediaDevices does not fire there). A backend whose unit is
    // bound to the old route rebuilds itself here and returns true; the
    // Qt backend has nothing to do and returns false.
    //
    // This is a real restart and therefore an audible gap. That is the
    // right trade: the route genuinely changed, and a VPIO unit built
    // against a 48 kHz speaker route is the wrong unit for a 16 kHz
    // Bluetooth one.
    virtual bool restartForRouteChange() { return false; }

    // ---- diagnostics ----

    // One line for the log when a session starts, describing what
    // actually opened. Free-form.
    virtual QString describe() const = 0;

    // False once the backend has failed in a way it will not recover
    // from by itself. AudioWorker polls this from the pump and demotes
    // to the Qt path when it goes false, which is what turns a dead
    // audio unit into a working call rather than a silent one.
    //
    // It exists because the first version only demoted on an OPEN
    // failure, so a unit that died mid-session — which is exactly what
    // happened on a device on 2026-09-25 — left the pipeline up, both
    // directions marked in-use, and nothing flowing in either. Silence
    // with no error and no way back short of rejoining.
    virtual bool healthy() const { return true; }

    // Counters worth putting in the end-of-session log line: how much
    // was actually captured and rendered, and how often anything had to
    // be rebuilt. Free-form, empty when a backend has nothing to add.
    //
    // The specific question this has to answer is "did the microphone
    // produce samples", because the level summary alone cannot: with
    // platform voice processing the software AGC does not run, and the
    // raw-level percentiles it reports are unmeasured rather than
    // silent. Reading those as evidence of silence is a trap this went
    // through once already.
    virtual QString diagnostics() const { return QString(); }

    // Set by AudioWorker. Invoked ON THE AUDIO THREAD when capture data
    // has arrived; only push-shaped backends call it.
    void setCaptureReadyHandler(std::function<void()> handler)
    {
        m_captureReady = std::move(handler);
    }

protected:
    void notifyCaptureReady()
    {
        if (m_captureReady) m_captureReady();
    }

private:
    std::function<void()> m_captureReady;
};

} // namespace bsfchat::voice
