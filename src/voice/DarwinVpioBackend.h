#pragma once

// IAudioBackend on one kAudioUnitSubType_VoiceProcessingIO audio unit,
// shared by macOS and iOS.
//
// WHY BOTH DIRECTIONS GO THROUGH ONE OBJECT
// -----------------------------------------
// This is the crux of the design and the reason the seam in
// AudioBackend.h had to exist at all. VPIO's acoustic echo canceller
// takes its far-end reference from the unit's OWN output bus. Audio
// played through anything else — a QAudioSink, another unit, the system
// mixer — is not a reference the canceller has, so it is not cancelled.
// A "VPIO capture path" bolted onto Qt playback would therefore be all
// of the cost of this file and none of the benefit: the far end would
// still hear themselves.
//
// That is also why closeCapture() does not stop the unit while render is
// still open, and vice versa. The unit is the AEC; stopping half of it
// stops the cancellation.
//
// WHAT VPIO BUYS
// --------------
// Echo cancellation, noise suppression and automatic gain control,
// applied by the OS on the capture stream, on both platforms:
//
//   * iOS — verified broken today without it. Qt's iOS QAudioSource is
//     built on kAudioUnitSubType_RemoteIO ('rioc'), which is the raw
//     path, so speakerphone echoes. Confirmed on a real iPhone 16 Pro
//     Max on 2026-09-23: calls work, audio flows both ways, and the far
//     end hears themselves.
//   * macOS — the same gap, open for as long as this client has
//     existed, recorded in VoiceGain.h as the reason a software AEC was
//     considered and rejected. AUVoiceProcessing is the same audio unit
//     subtype on the desktop, so one backend closes both.
//
// And the hard part of any echo canceller — time-aligning the far-end
// reference with the microphone signal — does not arise. The unit owns
// both ends of it.
//
// SHAPE MISMATCH, AND THE TWO RINGS
// ---------------------------------
// AudioWorker is push-capture (readyRead) and pull-playback
// (bytesFree + write). VPIO is neither: it is one callback-driven unit
// on a CoreAudio real-time thread that pushes input at us and pulls
// output from us, and that thread must never be blocked, never allocate
// and never take a lock the audio thread holds.
//
// So there are two lock-free SPSC rings (AudioRingBuffer.h):
//
//    input callback  --push-->  capture ring  --pull-->  pump/drain
//    pump/write      --push-->  playback ring --pull-->  render callback
//
// The pump and the frame arithmetic in AudioWorker are untouched. What
// changes is only that capture is polled from the pump rather than
// pushed (capturePolled() returns true), because a CoreAudio callback
// cannot call into a QObject.
//
// DEVICE SELECTION
// ----------------
//   * macOS — kAudioOutputUnitProperty_CurrentDevice per element, from
//     the QAudioDevice the existing AudioDevicePolicy resolved. Qt's
//     CoreAudio backend sets QAudioDevice::id() to the device's
//     kAudioDevicePropertyDeviceUID (verified in Qt 6's
//     qdarwinmediadevices.mm, uniqueId()), so the id maps straight back
//     to an AudioDeviceID. Changing either device rebuilds the unit,
//     because CurrentDevice cannot be changed on an initialised unit.
//   * iOS — there is no device to choose. Routing is AVAudioSession's,
//     Qt enumerates exactly one microphone, and the QAudioDevice
//     argument is ignored (and said so in the log).
//
// LIFETIME AND THREADING
// ----------------------
// Every public method runs on the audio thread. The two callbacks run
// on CoreAudio's thread and touch nothing but the rings, the converters
// they own, and their own scratch buffers. Teardown stops the unit
// BEFORE anything a callback reads is destroyed, which is what makes
// that safe.

#include <QAudioDevice>
#include <QAudioFormat>
#include <QString>

#include "voice/AudioBackend.h"
#include "voice/AudioRingBuffer.h"
#include "voice/VpioFormat.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

// CoreAudio's headers are plain C and include cleanly from C++, so this
// header can be included from AudioWorker.cpp without that file becoming
// Objective-C++. Only the .mm needs Objective-C, and only for the
// AVAudioSession query on iOS.
//
// This header is Apple-only by construction: CMake compiles the .mm only
// on Darwin, and every include of it is inside the same guard.
#include <TargetConditionals.h>

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#if TARGET_OS_OSX
// AudioDeviceID and the hardware object API are macOS-only: on iOS there
// are no devices to enumerate, routing is AVAudioSession's, and
// <CoreAudio/AudioHardware.h> is not part of the iOS SDK's AudioToolbox
// umbrella. Everything in this file that names a device is therefore
// behind the same guard.
#include <CoreAudio/CoreAudio.h>
#endif

namespace bsfchat::voice {

class DarwinVpioBackend : public IAudioBackend {
public:
    // Ring depth, in 20 ms pipeline frames, per direction.
    //
    // It has to cover the mismatch between the unit's callback cadence
    // and the 10 ms pump, and that cadence is not ours to choose: iOS
    // hands out anything from 5 ms to 40 ms depending on the route, and
    // changes it when a Bluetooth headset connects mid-call. 8 frames
    // (160 ms) swallows a 40 ms callback against a late pump with room
    // to spare, and costs 15 KB per direction.
    //
    // The ring is a SAFETY margin, not a latency budget: it normally
    // runs nearly empty in the capture direction and at the sink depth
    // (kSinkBufferFrames) in the playback direction, because
    // AudioWorker's pump writes only what bytesFree() asks for.
    static constexpr int kRingFrames = 8;

    // Frames the unit may ask for in one callback. 4096 at 48 kHz is
    // 85 ms, well past anything CoreAudio actually uses, and it is what
    // the scratch buffers are sized from.
    static constexpr int kMaxFramesPerSlice = 4096;

    DarwinVpioBackend();
    ~DarwinVpioBackend() override;

    // Whether the VoiceProcessingIO component exists at all on this
    // system. Cheap and side-effect free: it looks the component up, it
    // does not open a device, so it cannot raise a microphone
    // permission prompt.
    static bool available();

    AudioBackendKind kind() const override { return AudioBackendKind::DarwinVpio; }
    // The point of the whole file. AudioWorker stands its SpeechAgc
    // down when this is true — two AGCs in series chase each other.
    bool platformVoiceProcessing() const override { return true; }
    bool capturePolled() const override { return true; }

    bool openCapture(const QAudioDevice& device,
                     const QAudioFormat& format) override;
    void closeCapture() override;
    bool captureOpen() const override { return m_captureWanted && m_unitRunning; }
    qint64 readCapture(char* dst, qint64 maxBytes) override;

    bool openRender(const QAudioDevice& device, const QAudioFormat& format,
                    int bufferBytes) override;
    void closeRender() override;
    bool renderOpen() const override { return m_renderWanted && m_unitRunning; }
    qint64 bytesFree() override;
    qint64 writeRender(const char* src, qint64 bytes) override;

    bool restartForRouteChange() override;

    QString describe() const override;

    // True once an open has failed in a way that will not get better by
    // retrying. AudioWorker reads it to demote itself to the Qt backend
    // for the rest of the process.
    bool failed() const { return m_failed; }
    const QString& failureReason() const { return m_failureReason; }

    // One block of input for an AudioConverter, or a pull from a ring.
    // Public only because the two AudioConverterComplexInputDataProcs
    // are free functions in the .mm — CoreAudio takes C function
    // pointers, so they cannot be members.
    struct ConverterSource;

private:
    // Build, configure, initialise and start the unit for whatever
    // combination of directions is currently wanted. Idempotent when
    // nothing has changed.
    bool ensureUnit();
    // Stop, uninitialise and dispose. Safe at any point, including
    // half-built.
    void teardownUnit();
    // Everything between AudioComponentInstanceNew and
    // AudioOutputUnitStart. Split out so ensureUnit() can tear down
    // cleanly on any failure.
    bool configureUnit();
    bool applyStreamFormats();
    bool resolveDevices();

    void fail(const QString& reason);

    // ---- CoreAudio callbacks. Real-time thread; see the header note. ----
    // Exact AURenderCallback signatures: these are handed to CoreAudio
    // as function pointers, so the types have to be the real ones.
    static OSStatus inputTrampoline(void* refCon,
                                    AudioUnitRenderActionFlags* flags,
                                    const AudioTimeStamp* timeStamp,
                                    UInt32 bus, UInt32 frames,
                                    AudioBufferList* ioData);
    static OSStatus renderTrampoline(void* refCon,
                                     AudioUnitRenderActionFlags* flags,
                                     const AudioTimeStamp* timeStamp,
                                     UInt32 bus, UInt32 frames,
                                     AudioBufferList* ioData);
    OSStatus onInput(AudioUnitRenderActionFlags* flags,
                     const AudioTimeStamp* timeStamp, UInt32 bus, UInt32 frames);
    OSStatus onRender(UInt32 frames, AudioBufferList* ioData);

    AudioComponentInstance m_unit = nullptr;
    bool m_unitRunning = false;
    bool m_failed = false;
    QString m_failureReason;

    bool m_captureWanted = false;
    bool m_renderWanted = false;

    // The devices AudioDevicePolicy resolved, as Qt reported them.
    // Empty on iOS, where they are meaningless.
    QString m_captureDeviceId;
    QString m_renderDeviceId;
    QString m_captureDeviceDesc;
    QString m_renderDeviceDesc;
#if TARGET_OS_OSX
    // Resolved AudioDeviceIDs. kAudioObjectUnknown == "use the system
    // default", which is also what an unresolvable UID falls back to.
    // macOS only — see the include guard above.
    AudioDeviceID m_captureDeviceNative = kAudioObjectUnknown;
    AudioDeviceID m_renderDeviceNative = kAudioObjectUnknown;
    // What the unit was last built against, so ensureUnit() knows
    // whether a rebuild is actually needed.
    AudioDeviceID m_builtCaptureDevice = kAudioObjectUnknown;
    AudioDeviceID m_builtRenderDevice = kAudioObjectUnknown;
#endif

    PipelineAudioFormat m_pipeline;
    // The sink depth AudioWorker asked for, enforced by bytesFree()
    // rather than by the ring's size. See openRender().
    int m_renderTargetBytes = 0;
    FormatPlan m_capturePlan;
    FormatPlan m_renderPlan;

    AudioRingBuffer m_captureRing;
    AudioRingBuffer m_playbackRing;

    // Converters, non-null only when the corresponding plan says
    // Convert. Owned; disposed in teardownUnit() after the unit has
    // stopped.
    AudioConverterRef m_captureConverter = nullptr;
    AudioConverterRef m_renderConverter = nullptr;

    // Callback-thread scratch. Allocated at open, never on the
    // real-time path. Held as bytes because their interpretation
    // (unit-format frames vs pipeline-format frames) differs by
    // direction.
    std::vector<char> m_unitCaptureScratch;   // unit format, from AudioUnitRender
    std::vector<char> m_pipelineCaptureScratch;  // pipeline format, post-convert
    std::vector<char> m_renderPullScratch;    // pipeline format, from the ring

    // Enough state for the converter input procs to hand one block over
    // without allocating. Callback thread only.
    std::unique_ptr<ConverterSource> m_captureSource;
    std::unique_ptr<ConverterSource> m_renderSource;

    // Diagnostics, read from the audio thread, written from the
    // callback thread. Relaxed atomics would be more correct than
    // nothing and less honest than saying so: these are counters for a
    // log line, and a torn read of one changes a number in a message.
    std::atomic<uint32_t> m_renderUnderrunFrames{0};

    // One-shot: the "iOS ignores the device argument" note is true of
    // every open, and saying it on every headset change is noise.
    [[maybe_unused]] bool m_captureLoggedIosDeviceNote = false;
};

} // namespace bsfchat::voice
