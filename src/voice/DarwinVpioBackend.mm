// VoiceProcessingIO capture + render for macOS and iOS. See
// DarwinVpioBackend.h for the design and for why both directions have to
// go through one unit.
//
// *** NOT YET RUN ON ANY DEVICE. ***
// Every claim in the comments below about what CoreAudio does is either
// documented Apple behaviour or read off the headers; none of it has
// been observed from this process. The parts that can be checked without
// hardware — format negotiation, the ring, the lifecycle state machine,
// the backend choice — are unit tests. The audio unit itself is not
// testable without a microphone and a speaker in the same room, and this
// file does not pretend otherwise.
//
// Written without ARC, like the rest of the .mm files here (the project
// passes no -fobjc-arc, and MacCameraCapturer.mm calls -release
// directly). Nothing in this file owns an Objective-C object across a
// statement, so the question barely arises: the CoreAudio API is plain
// C, and the only ObjC is a read of AVAudioSession's current rate for
// the log.

#include "voice/DarwinVpioBackend.h"

#include <QtGlobal>

#include <TargetConditionals.h>

#if TARGET_OS_IPHONE
#import <AVFoundation/AVFoundation.h>
#endif
#if TARGET_OS_OSX
#include <CoreAudio/CoreAudio.h>
#endif

#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bsfchat::voice {
namespace {

// Element 0 is the output (speaker) bus, element 1 the input
// (microphone) bus. The naming is CoreAudio's and is confusing on
// purpose: on the OUTPUT element you set the format on the INPUT scope
// (what you feed it), and on the INPUT element you set the format on the
// OUTPUT scope (what it feeds you).
constexpr AudioUnitElement kOutputBus = 0;
constexpr AudioUnitElement kInputBus = 1;

AudioStreamBasicDescription makeInt16Asbd(double rate, int channels)
{
    AudioStreamBasicDescription a = {};
    a.mSampleRate = rate;
    a.mFormatID = kAudioFormatLinearPCM;
    a.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    a.mBitsPerChannel = 16;
    a.mChannelsPerFrame = static_cast<UInt32>(channels);
    a.mFramesPerPacket = 1;
    a.mBytesPerFrame = static_cast<UInt32>(2 * channels);
    a.mBytesPerPacket = a.mBytesPerFrame;
    return a;
}

// Classify an ASBD into the three buckets VpioFormat.h reasons about.
// Anything that is not packed interleaved int16 or float32 linear PCM is
// Other, which sends the caller back to the Qt path rather than into a
// converter built on a guess.
SampleType classify(const AudioStreamBasicDescription& a)
{
    if (a.mFormatID != kAudioFormatLinearPCM) return SampleType::Other;
    if (a.mFormatFlags & kAudioFormatFlagIsNonInterleaved) return SampleType::Other;
    if (!(a.mFormatFlags & kAudioFormatFlagIsPacked)) return SampleType::Other;
    if (a.mFormatFlags & kAudioFormatFlagIsFloat)
        return a.mBitsPerChannel == 32 ? SampleType::Float32 : SampleType::Other;
    if (a.mFormatFlags & kAudioFormatFlagIsSignedInteger)
        return a.mBitsPerChannel == 16 ? SampleType::Int16 : SampleType::Other;
    return SampleType::Other;
}

AudioStreamBasicDescription asbdFor(const StreamAudioFormat& f)
{
    if (f.sampleType == SampleType::Float32) {
        AudioStreamBasicDescription a = {};
        a.mSampleRate = f.sampleRate;
        a.mFormatID = kAudioFormatLinearPCM;
        a.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        a.mBitsPerChannel = 32;
        a.mChannelsPerFrame = static_cast<UInt32>(f.channels);
        a.mFramesPerPacket = 1;
        a.mBytesPerFrame = static_cast<UInt32>(4 * f.channels);
        a.mBytesPerPacket = a.mBytesPerFrame;
        return a;
    }
    return makeInt16Asbd(f.sampleRate, f.channels);
}

int bytesPerFrame(const StreamAudioFormat& f)
{
    return (f.sampleType == SampleType::Float32 ? 4 : 2) * f.channels;
}

// A four-character OSStatus reads as a FourCC far more often than as a
// number, and "-10879" costs ten minutes that "'!pro'" does not.
QString statusString(OSStatus st)
{
    const quint32 v = static_cast<quint32>(st);
    char c[4] = {char((v >> 24) & 0xFF), char((v >> 16) & 0xFF),
                 char((v >> 8) & 0xFF), char(v & 0xFF)};
    bool printable = true;
    for (char ch : c) {
        if (ch < 0x20 || ch > 0x7E) printable = false;
    }
    if (printable)
        return QStringLiteral("'%1' (%2)")
            .arg(QString::fromLatin1(c, 4))
            .arg(int(st));
    return QString::number(int(st));
}

#if TARGET_OS_OSX
// QAudioDevice::id() on macOS is the device's kAudioDevicePropertyDeviceUID
// as UTF-8 — verified in Qt 6's qdarwinmediadevices.mm, uniqueId(). So a
// Qt device id maps back to an AudioDeviceID by walking the hardware's
// device list and comparing UIDs.
AudioDeviceID deviceForUid(const QString& uid)
{
    if (uid.isEmpty()) return kAudioObjectUnknown;

    AudioObjectPropertyAddress addr = {kAudioHardwarePropertyDevices,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0,
                                       nullptr, &size)
        != noErr) {
        return kAudioObjectUnknown;
    }
    const int count = int(size / sizeof(AudioDeviceID));
    if (count <= 0) return kAudioObjectUnknown;

    std::vector<AudioDeviceID> devices(static_cast<size_t>(count));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
                                   &size, devices.data())
        != noErr) {
        return kAudioObjectUnknown;
    }

    AudioObjectPropertyAddress uidAddr = {kAudioDevicePropertyDeviceUID,
                                          kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain};
    for (AudioDeviceID dev : devices) {
        CFStringRef cf = nullptr;
        UInt32 sz = sizeof(cf);
        if (AudioObjectGetPropertyData(dev, &uidAddr, 0, nullptr, &sz, &cf)
                != noErr
            || !cf) {
            continue;
        }
        const QString found = QString::fromCFString(cf);
        CFRelease(cf);
        if (found == uid) return dev;
    }
    return kAudioObjectUnknown;
}
#endif  // TARGET_OS_OSX

// The rate AVAudioSession is running at right now, or 0 where there is
// no session to ask (macOS).
double currentSessionRate()
{
#if TARGET_OS_IPHONE
    @autoreleasepool {
        return [AVAudioSession sharedInstance].sampleRate;
    }
#else
    return 0.0;
#endif
}

} // namespace

// One block of input for an AudioConverter, or a pull from a ring.
// Callback thread only; never allocates.
struct DarwinVpioBackend::ConverterSource {
    // Set by the capture path before each FillComplexBuffer call.
    const char* block = nullptr;
    UInt32 blockFrames = 0;
    bool consumed = false;

    // Set by the render path; the proc pulls from here instead.
    AudioRingBuffer* ring = nullptr;
    std::vector<char>* scratch = nullptr;

    UInt32 channels = 1;
    UInt32 frameBytes = 2;
};

namespace {

// Capture: hand the converter the one block AudioUnitRender just
// produced, then report end-of-input. Returning noErr with zero packets
// is how an AudioConverterComplexInputDataProc says "that is all I
// have"; FillComplexBuffer then returns with whatever it managed to
// produce, which is exactly the behaviour wanted here.
OSStatus captureConverterInput(AudioConverterRef, UInt32* ioPackets,
                               AudioBufferList* ioData,
                               AudioStreamPacketDescription**, void* userData)
{
    auto* src = static_cast<DarwinVpioBackend::ConverterSource*>(userData);
    ioData->mNumberBuffers = 1;
    if (src->consumed || src->blockFrames == 0) {
        *ioPackets = 0;
        ioData->mBuffers[0].mData = nullptr;
        ioData->mBuffers[0].mDataByteSize = 0;
        ioData->mBuffers[0].mNumberChannels = src->channels;
        return noErr;
    }
    *ioPackets = src->blockFrames;
    ioData->mBuffers[0].mNumberChannels = src->channels;
    ioData->mBuffers[0].mData = const_cast<char*>(src->block);
    ioData->mBuffers[0].mDataByteSize = src->blockFrames * src->frameBytes;
    src->consumed = true;
    return noErr;
}

// Render: pull as much as the converter asks for out of the playback
// ring, capped by the scratch buffer. A short pull (or none) is an
// underrun, and the caller pads with silence — which is the right
// failure: a stretched or repeated frame is worse than a gap.
OSStatus renderConverterInput(AudioConverterRef, UInt32* ioPackets,
                              AudioBufferList* ioData,
                              AudioStreamPacketDescription**, void* userData)
{
    auto* src = static_cast<DarwinVpioBackend::ConverterSource*>(userData);
    ioData->mNumberBuffers = 1;
    ioData->mBuffers[0].mNumberChannels = src->channels;

    int wantBytes = int(*ioPackets) * int(src->frameBytes);
    const int cap = int(src->scratch->size());
    if (wantBytes > cap) wantBytes = cap;
    wantBytes -= wantBytes % int(src->frameBytes);

    const int got = wantBytes > 0
                        ? src->ring->read(src->scratch->data(), wantBytes)
                        : 0;
    if (got <= 0) {
        *ioPackets = 0;
        ioData->mBuffers[0].mData = nullptr;
        ioData->mBuffers[0].mDataByteSize = 0;
        return noErr;
    }
    *ioPackets = UInt32(got / int(src->frameBytes));
    ioData->mBuffers[0].mData = src->scratch->data();
    ioData->mBuffers[0].mDataByteSize = UInt32(got);
    return noErr;
}

} // namespace

// ---------------------------------------------------------------------
// Construction and availability
// ---------------------------------------------------------------------

DarwinVpioBackend::DarwinVpioBackend()
    : m_captureSource(std::make_unique<ConverterSource>())
    , m_renderSource(std::make_unique<ConverterSource>())
{
}

DarwinVpioBackend::~DarwinVpioBackend()
{
    teardownUnit();
}

bool DarwinVpioBackend::available()
{
    AudioComponentDescription desc = {};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_VoiceProcessingIO;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    // A lookup, not an instantiation: nothing is opened, so this cannot
    // raise a microphone permission prompt. Safe to call at any time.
    return AudioComponentFindNext(nullptr, &desc) != nullptr;
}

void DarwinVpioBackend::fail(const QString& reason)
{
    if (m_failed) return;
    m_failed = true;
    m_failureReason = reason;
    qWarning("[voice] VoiceProcessingIO unavailable: %s", qPrintable(reason));
}

// ---------------------------------------------------------------------
// Unit lifecycle
// ---------------------------------------------------------------------

bool DarwinVpioBackend::resolveDevices()
{
#if TARGET_OS_OSX
    m_captureDeviceNative = deviceForUid(m_captureDeviceId);
    m_renderDeviceNative = deviceForUid(m_renderDeviceId);
    // kAudioObjectUnknown means "we could not map the UID". That is not
    // fatal — the unit falls back to the system default device, which is
    // what the user would get from the Qt path too if their preference
    // had vanished — but it IS the case where the user's explicit device
    // choice silently stops being honoured, so it is a warning and not
    // an info.
    if (m_captureWanted && !m_captureDeviceId.isEmpty()
        && m_captureDeviceNative == kAudioObjectUnknown) {
        qWarning("[voice] VPIO: input device '%s' has no CoreAudio UID match; "
                 "using the system default input",
                 qPrintable(m_captureDeviceDesc));
    }
    if (m_renderWanted && !m_renderDeviceId.isEmpty()
        && m_renderDeviceNative == kAudioObjectUnknown) {
        qWarning("[voice] VPIO: output device '%s' has no CoreAudio UID match; "
                 "using the system default output",
                 qPrintable(m_renderDeviceDesc));
    }
#endif
    return true;
}

bool DarwinVpioBackend::ensureUnit()
{
    if (m_failed) return false;

    if (!m_captureWanted && !m_renderWanted) {
        teardownUnit();
        return true;
    }

    resolveDevices();

    // A direction opening or closing does NOT rebuild: both buses stay
    // enabled for the life of the unit, because the echo canceller needs
    // the render bus even on a frame where nothing is being captured,
    // and needs the capture bus to have the reference applied to it.
    // Only a device change forces a rebuild, and only on macOS — on iOS
    // there are no devices to change, and a route change is the
    // session's business rather than the unit's (it arrives through
    // restartForRouteChange() instead).
    const bool haveUnit = m_unit != nullptr && m_unitRunning;
#if TARGET_OS_OSX
    const bool deviceChanged = m_builtCaptureDevice != m_captureDeviceNative
                               || m_builtRenderDevice != m_renderDeviceNative;
#else
    const bool deviceChanged = false;
#endif
    if (haveUnit && !deviceChanged) return true;

    teardownUnit();
    if (!configureUnit()) {
        teardownUnit();
        return false;
    }
#if TARGET_OS_OSX
    m_builtCaptureDevice = m_captureDeviceNative;
    m_builtRenderDevice = m_renderDeviceNative;
#endif
    return true;
}

bool DarwinVpioBackend::configureUnit()
{
    AudioComponentDescription desc = {};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_VoiceProcessingIO;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) {
        fail(QStringLiteral("no VoiceProcessingIO audio component on this system"));
        return false;
    }
    OSStatus st = AudioComponentInstanceNew(comp, &m_unit);
    if (st != noErr || !m_unit) {
        fail(QStringLiteral("AudioComponentInstanceNew failed: %1")
                 .arg(statusString(st)));
        return false;
    }

    // Both directions, always. See the comment in ensureUnit().
    const UInt32 enable = 1;
    st = AudioUnitSetProperty(m_unit, kAudioOutputUnitProperty_EnableIO,
                              kAudioUnitScope_Input, kInputBus, &enable,
                              sizeof(enable));
    if (st != noErr) {
        fail(QStringLiteral("enabling the input bus failed: %1").arg(statusString(st)));
        return false;
    }
    st = AudioUnitSetProperty(m_unit, kAudioOutputUnitProperty_EnableIO,
                              kAudioUnitScope_Output, kOutputBus, &enable,
                              sizeof(enable));
    if (st != noErr) {
        fail(QStringLiteral("enabling the output bus failed: %1").arg(statusString(st)));
        return false;
    }

#if TARGET_OS_OSX
    // Must be set before the unit is initialised; CurrentDevice cannot
    // be changed on an initialised unit, which is why a device switch
    // rebuilds.
    //
    // Per-element device selection (element 1 = input, element 0 =
    // output) is what Chromium's VPIO path does and is the only way to
    // honour the user's two independent device choices. REASONED, NOT
    // VERIFIED: Apple documents kAudioOutputUnitProperty_CurrentDevice
    // as a global-scope property without saying whether VPIO reads the
    // element. If a Mac turns out to route both directions to whichever
    // one was set last, the symptom is "the output device setting is
    // ignored when echo cancellation is on", and the honest fix is to
    // fall back to the Qt path when the two devices differ.
    if (m_captureDeviceNative != kAudioObjectUnknown) {
        AudioDeviceID dev = m_captureDeviceNative;
        st = AudioUnitSetProperty(m_unit, kAudioOutputUnitProperty_CurrentDevice,
                                  kAudioUnitScope_Global, kInputBus, &dev,
                                  sizeof(dev));
        if (st != noErr) {
            qWarning("[voice] VPIO: could not select input device '%s': %s — "
                     "the system default input will be used",
                     qPrintable(m_captureDeviceDesc), qPrintable(statusString(st)));
        }
    }
    if (m_renderDeviceNative != kAudioObjectUnknown) {
        AudioDeviceID dev = m_renderDeviceNative;
        st = AudioUnitSetProperty(m_unit, kAudioOutputUnitProperty_CurrentDevice,
                                  kAudioUnitScope_Global, kOutputBus, &dev,
                                  sizeof(dev));
        if (st != noErr) {
            qWarning("[voice] VPIO: could not select output device '%s': %s — "
                     "the system default output will be used",
                     qPrintable(m_renderDeviceDesc), qPrintable(statusString(st)));
        }
    }
#endif

    // Upper bound on what a callback may ask for. The scratch buffers
    // below are sized from it, so if CoreAudio ignored it the callbacks
    // would be writing past their buffers — hence the defensive frame
    // check in onInput()/onRender() as well.
    UInt32 maxFrames = kMaxFramesPerSlice;
    AudioUnitSetProperty(m_unit, kAudioUnitProperty_MaximumFramesPerSlice,
                         kAudioUnitScope_Global, 0, &maxFrames, sizeof(maxFrames));

    if (!applyStreamFormats()) return false;

    // Our buffers, not the unit's, for the input bus.
    const UInt32 dontAllocate = 0;
    AudioUnitSetProperty(m_unit, kAudioUnitProperty_ShouldAllocateBuffer,
                         kAudioUnitScope_Output, kInputBus, &dontAllocate,
                         sizeof(dontAllocate));

    AURenderCallbackStruct inputCb = {};
    inputCb.inputProc = &DarwinVpioBackend::inputTrampoline;
    inputCb.inputProcRefCon = this;
    st = AudioUnitSetProperty(m_unit, kAudioOutputUnitProperty_SetInputCallback,
                              kAudioUnitScope_Global, 0, &inputCb, sizeof(inputCb));
    if (st != noErr) {
        fail(QStringLiteral("installing the input callback failed: %1")
                 .arg(statusString(st)));
        return false;
    }

    AURenderCallbackStruct renderCb = {};
    renderCb.inputProc = &DarwinVpioBackend::renderTrampoline;
    renderCb.inputProcRefCon = this;
    st = AudioUnitSetProperty(m_unit, kAudioUnitProperty_SetRenderCallback,
                              kAudioUnitScope_Input, kOutputBus, &renderCb,
                              sizeof(renderCb));
    if (st != noErr) {
        fail(QStringLiteral("installing the render callback failed: %1")
                 .arg(statusString(st)));
        return false;
    }

    // The whole point. Bypass off; AGC on, because with VPIO running,
    // AudioWorker stands its own SpeechAgc down (platformVoiceProcessing()
    // is true) and something has to level the microphone.
    const UInt32 bypass = 0;
    AudioUnitSetProperty(m_unit, kAUVoiceIOProperty_BypassVoiceProcessing,
                         kAudioUnitScope_Global, 0, &bypass, sizeof(bypass));
    const UInt32 agc = 1;
    AudioUnitSetProperty(m_unit, kAUVoiceIOProperty_VoiceProcessingEnableAGC,
                         kAudioUnitScope_Global, 0, &agc, sizeof(agc));

    st = AudioUnitInitialize(m_unit);
    if (st != noErr) {
        fail(QStringLiteral("AudioUnitInitialize failed: %1").arg(statusString(st)));
        return false;
    }

    // Sized from the plans, which are only known after applyStreamFormats().
    const int pipeFrameBytes = m_pipeline.channels * 2;
    m_unitCaptureScratch.assign(
        static_cast<size_t>(kMaxFramesPerSlice) * size_t(bytesPerFrame(m_capturePlan.unit)),
        0);
    m_pipelineCaptureScratch.assign(
        static_cast<size_t>(kMaxFramesPerSlice + m_pipeline.frameSamples)
            * size_t(pipeFrameBytes),
        0);
    m_renderPullScratch.assign(
        static_cast<size_t>(kMaxFramesPerSlice + m_pipeline.frameSamples)
            * size_t(pipeFrameBytes),
        0);

    // The rings are sized HERE, between AudioUnitInitialize and
    // AudioOutputUnitStart, and nowhere else.
    //
    // They used to be sized in openCapture()/openRender(), which is a
    // use-after-free waiting to happen: AudioWorker opens capture first,
    // that starts the unit, and then openRender() called resize() —
    // reallocating the playback ring's storage underneath a render
    // callback that was already running on the CoreAudio thread. Every
    // join went through that window.
    //
    // This is the only place in the object's life where the unit is
    // known to be stopped and about to start, which is exactly the
    // precondition AudioRingBuffer::resize() documents.
    m_captureRing.resize(ringCapacityBytes(m_pipeline, kRingFrames));
    m_playbackRing.resize(ringCapacityBytes(m_pipeline, kRingFrames));

    m_captureSource->channels = UInt32(m_capturePlan.unit.channels);
    m_captureSource->frameBytes = UInt32(bytesPerFrame(m_capturePlan.unit));
    m_renderSource->channels = UInt32(m_pipeline.channels);
    m_renderSource->frameBytes = UInt32(pipeFrameBytes);
    m_renderSource->ring = &m_playbackRing;
    m_renderSource->scratch = &m_renderPullScratch;

    st = AudioOutputUnitStart(m_unit);
    if (st != noErr) {
        fail(QStringLiteral("AudioOutputUnitStart failed: %1").arg(statusString(st)));
        return false;
    }
    m_unitRunning = true;
    m_builtSessionRate = currentSessionRate();

    qInfo("[voice] VPIO started: %s", qPrintable(describe()));
    return true;
}

bool DarwinVpioBackend::applyStreamFormats()
{
    const StreamAudioFormat want = desiredClientFormat(m_pipeline);
    const AudioStreamBasicDescription wantAsbd =
        makeInt16Asbd(want.sampleRate, want.channels);

    // Capture: the format the unit hands US, on the OUTPUT scope of the
    // INPUT element.
    OSStatus st =
        AudioUnitSetProperty(m_unit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Output, kInputBus, &wantAsbd,
                             sizeof(wantAsbd));
    if (st != noErr) {
        qInfo("[voice] VPIO: input bus refused the pipeline format (%s); "
              "taking the unit's own and converting",
              qPrintable(statusString(st)));
    }
    // Read back unconditionally. On the refusal path this is the only
    // way to learn what we actually got; on the success path it is a
    // cheap check that the unit did not quietly substitute something.
    AudioStreamBasicDescription got = {};
    UInt32 size = sizeof(got);
    st = AudioUnitGetProperty(m_unit, kAudioUnitProperty_StreamFormat,
                              kAudioUnitScope_Output, kInputBus, &got, &size);
    if (st != noErr) {
        fail(QStringLiteral("could not read the input bus format: %1")
                 .arg(statusString(st)));
        return false;
    }
    m_capturePlan = planConversion(
        StreamAudioFormat{got.mSampleRate, int(got.mChannelsPerFrame),
                          classify(got)},
        m_pipeline);

    // Render: the format WE hand the unit, on the INPUT scope of the
    // OUTPUT element.
    st = AudioUnitSetProperty(m_unit, kAudioUnitProperty_StreamFormat,
                              kAudioUnitScope_Input, kOutputBus, &wantAsbd,
                              sizeof(wantAsbd));
    if (st != noErr) {
        qInfo("[voice] VPIO: output bus refused the pipeline format (%s); "
              "taking the unit's own and converting",
              qPrintable(statusString(st)));
    }
    AudioStreamBasicDescription gotOut = {};
    size = sizeof(gotOut);
    st = AudioUnitGetProperty(m_unit, kAudioUnitProperty_StreamFormat,
                              kAudioUnitScope_Input, kOutputBus, &gotOut, &size);
    if (st != noErr) {
        fail(QStringLiteral("could not read the output bus format: %1")
                 .arg(statusString(st)));
        return false;
    }
    m_renderPlan = planConversion(
        StreamAudioFormat{gotOut.mSampleRate, int(gotOut.mChannelsPerFrame),
                          classify(gotOut)},
        m_pipeline);

    if (!m_capturePlan.usable()) {
        fail(QStringLiteral("input bus format unusable: %1 (%2 Hz, %3 ch)")
                 .arg(QString::fromLatin1(m_capturePlan.reason))
                 .arg(m_capturePlan.unit.sampleRate)
                 .arg(m_capturePlan.unit.channels));
        return false;
    }
    if (!m_renderPlan.usable()) {
        fail(QStringLiteral("output bus format unusable: %1 (%2 Hz, %3 ch)")
                 .arg(QString::fromLatin1(m_renderPlan.reason))
                 .arg(m_renderPlan.unit.sampleRate)
                 .arg(m_renderPlan.unit.channels));
        return false;
    }

    const AudioStreamBasicDescription pipelineAsbd =
        makeInt16Asbd(m_pipeline.sampleRate, m_pipeline.channels);

    if (m_capturePlan.action == FormatAction::Convert) {
        const AudioStreamBasicDescription src = asbdFor(m_capturePlan.unit);
        st = AudioConverterNew(&src, &pipelineAsbd, &m_captureConverter);
        if (st != noErr) {
            fail(QStringLiteral("AudioConverterNew (capture) failed: %1")
                     .arg(statusString(st)));
            return false;
        }
        qInfo("[voice] VPIO capture conversion: %.0f Hz %d ch -> %.0f Hz %d ch (%s)",
              m_capturePlan.unit.sampleRate, m_capturePlan.unit.channels,
              m_pipeline.sampleRate, m_pipeline.channels, m_capturePlan.reason);
    }
    if (m_renderPlan.action == FormatAction::Convert) {
        const AudioStreamBasicDescription dst = asbdFor(m_renderPlan.unit);
        st = AudioConverterNew(&pipelineAsbd, &dst, &m_renderConverter);
        if (st != noErr) {
            fail(QStringLiteral("AudioConverterNew (render) failed: %1")
                     .arg(statusString(st)));
            return false;
        }
        qInfo("[voice] VPIO render conversion: %.0f Hz %d ch -> %.0f Hz %d ch (%s)",
              m_pipeline.sampleRate, m_pipeline.channels,
              m_renderPlan.unit.sampleRate, m_renderPlan.unit.channels,
              m_renderPlan.reason);
    }
    return true;
}

void DarwinVpioBackend::teardownUnit()
{
    if (m_unit) {
        // Stop FIRST. Everything below is state a callback reads, and
        // the only thing that guarantees no callback is in flight is
        // that the unit has stopped.
        if (m_unitRunning) AudioOutputUnitStop(m_unit);
        AudioUnitUninitialize(m_unit);
        AudioComponentInstanceDispose(m_unit);
        m_unit = nullptr;
    }
    m_unitRunning = false;

    if (m_captureConverter) {
        AudioConverterDispose(m_captureConverter);
        m_captureConverter = nullptr;
    }
    if (m_renderConverter) {
        AudioConverterDispose(m_renderConverter);
        m_renderConverter = nullptr;
    }
    m_captureRing.reset();
    m_playbackRing.reset();
#if TARGET_OS_OSX
    m_builtCaptureDevice = kAudioObjectUnknown;
    m_builtRenderDevice = kAudioObjectUnknown;
#endif
    m_renderSource->ring = nullptr;
    m_renderSource->scratch = nullptr;
}

// ---------------------------------------------------------------------
// IAudioBackend
// ---------------------------------------------------------------------

bool DarwinVpioBackend::openCapture(const QAudioDevice& device,
                                    const QAudioFormat& format)
{
    if (m_failed) return false;

    m_pipeline.sampleRate = format.sampleRate();
    m_pipeline.channels = format.channelCount();
    // 20 ms, the pipeline's frame. Derived rather than passed because
    // QAudioFormat has no notion of a frame size.
    m_pipeline.frameSamples = int(std::lround(format.sampleRate() * 0.02));

#if TARGET_OS_IPHONE
    if (!device.isNull() && !m_captureLoggedIosDeviceNote) {
        m_captureLoggedIosDeviceNote = true;
        qInfo("[voice] VPIO (iOS): ignoring the resolved input device '%s' — "
              "routing is AVAudioSession's and Qt enumerates one microphone",
              qPrintable(device.description()));
    }
#endif
    m_captureDeviceId = QString::fromLatin1(device.id());
    m_captureDeviceDesc = device.description();
    m_captureWanted = true;

    // No ring sizing here — see configureUnit(). By the time a second
    // direction opens, the unit is already running and its callbacks are
    // live, so touching the rings' storage from this thread is not safe.
    if (!ensureUnit()) {
        m_captureWanted = false;
        return false;
    }
    return true;
}

void DarwinVpioBackend::closeCapture()
{
    if (!m_captureWanted) return;
    m_captureWanted = false;
    // The unit keeps running while render is open: it is the echo
    // canceller, and stopping it would stop cancellation for the
    // playback that is still going out. The input callback checks
    // m_captureWanted and simply stops filling the ring.
    if (!m_renderWanted) {
        // Nothing else needs the unit, so stop it FIRST and only then
        // touch the ring. AudioRingBuffer::reset() is documented as
        // valid only with both sides stopped, and teardownUnit() resets
        // both rings itself once the unit is down.
        teardownUnit();
        return;
    }
    // The unit is still running, so the input callback is still live and
    // the ring must not be reset underneath it. Draining is safe — this
    // IS the consumer side — and achieves the same thing.
    m_captureRing.discardAll();
}

qint64 DarwinVpioBackend::readCapture(char* dst, qint64 maxBytes)
{
    if (!m_captureWanted || maxBytes <= 0) return 0;
    return m_captureRing.read(dst, int(std::min<qint64>(maxBytes, INT32_MAX)));
}

bool DarwinVpioBackend::openRender(const QAudioDevice& device,
                                   const QAudioFormat& format, int bufferBytes)
{
    if (m_failed) return false;

    m_pipeline.sampleRate = format.sampleRate();
    m_pipeline.channels = format.channelCount();
    m_pipeline.frameSamples = int(std::lround(format.sampleRate() * 0.02));

    m_renderDeviceId = QString::fromLatin1(device.id());
    m_renderDeviceDesc = device.description();
    m_renderWanted = true;
    // The depth AudioWorker asked for, honoured through bytesFree()
    // rather than through the ring's capacity. The ring is deliberately
    // bigger (kRingFrames) so a late pump has somewhere to put a burst,
    // but reporting all of that as free space would let the worker
    // pre-buffer 160 ms and add every millisecond of it to
    // mouth-to-ear latency.
    m_renderTargetBytes = bufferBytes > 0
                              ? bufferBytes
                              : m_pipeline.frameBytes() * 5;

    // No ring sizing here either — see configureUnit().
    if (!ensureUnit()) {
        m_renderWanted = false;
        return false;
    }
    return true;
}

void DarwinVpioBackend::closeRender()
{
    if (!m_renderWanted) return;
    m_renderWanted = false;
    if (!m_captureWanted) {
        teardownUnit();
        return;
    }
    // Same rule as closeCapture(): the unit is still running for the
    // echo canceller, so the render callback is still live and reset()
    // is not ours to call. We are the PRODUCER on this ring, so we
    // cannot drain it either; the callback will empty it within a
    // buffer or two and then render silence, which is what we want.
}

qint64 DarwinVpioBackend::bytesFree()
{
    if (!m_renderWanted || !m_unitRunning) return 0;
    const int queued = m_playbackRing.used();
    const int room = std::min(m_renderTargetBytes - queued, m_playbackRing.free());
    return room > 0 ? room : 0;
}

qint64 DarwinVpioBackend::writeRender(const char* src, qint64 bytes)
{
    if (!m_renderWanted || !m_unitRunning) return -1;
    return m_playbackRing.write(src, int(std::min<qint64>(bytes, INT32_MAX)));
}

bool DarwinVpioBackend::restartForRouteChange()
{
    if (!m_unitRunning || m_failed) return false;

    // THE DEFAULT IS TO DO NOTHING, and that is the fix rather than a
    // shortcut.
    //
    // This used to tear the unit down and rebuild it on every route
    // change, on the reasoning that "a unit cannot be reconfigured in
    // place". The reasoning is true and the conclusion was wrong: a
    // VoiceProcessingIO unit follows the session's route by itself, and
    // because we set our CLIENT format to 48 kHz mono int16, the unit's
    // own converter absorbs whatever the hardware moves to. There is
    // nothing for us to rebuild.
    //
    // Rebuilding anyway was actively harmful. Activating the unit moves
    // the session's route, that posts a route notification, and the
    // notification asks for another rebuild — ten of them in eight
    // seconds on an iPhone 16 Pro Max on 2026-09-25, each one killing
    // the unit roughly 100 ms after starting it, which is before a VPIO
    // unit delivers its first input callback. Hence a whole session that
    // captured nothing, and finally CoreAudio refusing to start the unit
    // at all ('what' — AVAudioSessionErrorCodeUnspecified).
    //
    // The ONE case that genuinely needs a rebuild is a converter built
    // against a rate that has since moved: if the unit refused our
    // client format we own an AudioConverter for a specific unit rate,
    // and a Bluetooth HFP headset dropping the session to 16 kHz makes
    // that converter wrong. When the plan is Direct there is no such
    // converter and nothing can be stale.
    const bool haveConverter = m_capturePlan.action == FormatAction::Convert
                               || m_renderPlan.action == FormatAction::Convert;
    const double now = currentSessionRate();
    const bool rateMoved =
        m_builtSessionRate > 0.0 && now > 0.0
        && std::abs(now - m_builtSessionRate) > 1.0;

    if (!haveConverter || !rateMoved) {
        if (rateMoved) {
            qInfo("[voice] VPIO: session rate %.0f Hz -> %.0f Hz; the unit's "
                  "own converter absorbs it, not rebuilding",
                  m_builtSessionRate, now);
            m_builtSessionRate = now;
        }
        return false;
    }

    if (!m_rebuildLimiter.allow(QDateTime::currentMSecsSinceEpoch())) {
        // Three in ten seconds is not a user plugging things in, it is a
        // loop. Fail rather than slow down: AudioWorker polls healthy()
        // and moves the call to the Qt backend, which is a working call
        // without echo cancellation instead of a spinning one with it.
        fail(QStringLiteral("rebuilt %1 times in quick succession after route "
                            "changes; giving up on VoiceProcessingIO")
                 .arg(m_rebuildLimiter.maxRebuilds()));
        return false;
    }

    qInfo("[voice] VPIO: rebuilding for a session rate change %.0f Hz -> %.0f Hz "
          "(our converter was built for the old one)",
          m_builtSessionRate, now);
    ++m_rebuildCount;
    teardownUnit();
    if (!ensureUnit()) {
        qWarning("[voice] VPIO could not restart after a route change: %s",
                 qPrintable(m_failureReason));
        return false;
    }
    return true;
}

QString DarwinVpioBackend::diagnostics() const
{
    const uint32_t frames = m_capturedFrames.load(std::memory_order_relaxed);
    const uint32_t callbacks = m_inputCallbacks.load(std::memory_order_relaxed);
    QString peak;
    if (m_capturePlan.action == FormatAction::Direct) {
        peak = QStringLiteral(", peak |sample| %1")
                   .arg(m_capturePeak.load(std::memory_order_relaxed));
    } else {
        // The peak is only sampled on the direct path; saying "0" for a
        // converted stream would read as silence when it means unmeasured.
        peak = QStringLiteral(", peak not measured (converted capture)");
    }
    return QStringLiteral(
               "VPIO: %1 input callbacks, %2 frames captured%3; %4 render "
               "callbacks, %5 ring overruns, %6 underruns; %7 rebuilds")
        .arg(callbacks)
        .arg(frames)
        .arg(peak)
        .arg(m_renderCallbacks.load(std::memory_order_relaxed))
        .arg(m_captureRing.overruns())
        .arg(m_playbackRing.underruns())
        .arg(m_rebuildCount);
}

QString DarwinVpioBackend::describe() const
{
    QString s = QStringLiteral("VoiceProcessingIO");
#if TARGET_OS_IPHONE
    @autoreleasepool {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        s += QStringLiteral(" [session %1 Hz, %2 ms IO buffer]")
                 .arg(session.sampleRate)
                 .arg(session.IOBufferDuration * 1000.0, 0, 'f', 1);
    }
#endif
    s += QStringLiteral(" in=%1 out=%2")
             .arg(m_capturePlan.action == FormatAction::Direct
                      ? QStringLiteral("direct")
                      : QStringLiteral("%1Hz->%2Hz")
                            .arg(m_capturePlan.unit.sampleRate)
                            .arg(m_pipeline.sampleRate))
             .arg(m_renderPlan.action == FormatAction::Direct
                      ? QStringLiteral("direct")
                      : QStringLiteral("%1Hz->%2Hz")
                            .arg(m_pipeline.sampleRate)
                            .arg(m_renderPlan.unit.sampleRate));
    return s;
}

// ---------------------------------------------------------------------
// CoreAudio callbacks — real-time thread
// ---------------------------------------------------------------------

OSStatus DarwinVpioBackend::inputTrampoline(void* refCon,
                                            AudioUnitRenderActionFlags* flags,
                                            const AudioTimeStamp* timeStamp,
                                            UInt32 bus, UInt32 frames,
                                            AudioBufferList*)
{
    return static_cast<DarwinVpioBackend*>(refCon)->onInput(flags, timeStamp,
                                                            bus, frames);
}

OSStatus DarwinVpioBackend::renderTrampoline(void* refCon,
                                             AudioUnitRenderActionFlags*,
                                             const AudioTimeStamp*, UInt32,
                                             UInt32 frames,
                                             AudioBufferList* ioData)
{
    return static_cast<DarwinVpioBackend*>(refCon)->onRender(frames, ioData);
}

OSStatus DarwinVpioBackend::onInput(AudioUnitRenderActionFlags* flags,
                                    const AudioTimeStamp* timeStamp, UInt32 bus,
                                    UInt32 frames)
{
    // Not capturing: do not call AudioUnitRender at all. The unit keeps
    // running for the echo canceller's sake, and the microphone samples
    // it produced are simply not collected.
    if (!m_captureWanted || frames == 0) return noErr;

    // kMaxFramesPerSlice was requested, but a unit is not obliged to
    // honour it. Dropping the block is the only safe answer; writing
    // past the scratch buffer is not an answer at all.
    if (frames > UInt32(kMaxFramesPerSlice)) return noErr;

    const UInt32 unitFrameBytes = UInt32(bytesPerFrame(m_capturePlan.unit));

    AudioBufferList abl;
    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mNumberChannels = UInt32(m_capturePlan.unit.channels);
    abl.mBuffers[0].mDataByteSize = frames * unitFrameBytes;
    abl.mBuffers[0].mData = m_unitCaptureScratch.data();

    const OSStatus st =
        AudioUnitRender(m_unit, flags, timeStamp, bus, frames, &abl);
    if (st != noErr) return st;

    m_inputCallbacks.fetch_add(1, std::memory_order_relaxed);
    m_capturedFrames.fetch_add(frames, std::memory_order_relaxed);

    // Post-AEC, post-NS, post-AGC. This is the point of the file.
    if (m_capturePlan.action == FormatAction::Direct) {
        // Cheapest possible answer to "did the microphone produce
        // anything", carried to the end-of-session log. A peak of 0 over
        // a whole call means silence reached us; no callbacks at all
        // means the unit never ran. Those are different bugs and the log
        // has to tell them apart — it could not, before.
        const int16_t* p =
            reinterpret_cast<const int16_t*>(m_unitCaptureScratch.data());
        const int count = int(frames) * m_capturePlan.unit.channels;
        uint32_t peak = m_capturePeak.load(std::memory_order_relaxed);
        for (int i = 0; i < count; ++i) {
            const int16_t v = p[i];
            const uint32_t a = uint32_t(v < 0 ? -(v + 1) : v);
            if (a > peak) peak = a;
        }
        m_capturePeak.store(peak, std::memory_order_relaxed);

        m_captureRing.write(m_unitCaptureScratch.data(),
                            int(frames * unitFrameBytes));
        return noErr;
    }

    if (!m_captureConverter) return noErr;

    m_captureSource->block = m_unitCaptureScratch.data();
    m_captureSource->blockFrames = frames;
    m_captureSource->consumed = false;

    const UInt32 pipelineFrameBytes = UInt32(m_pipeline.channels * 2);
    UInt32 outFrames = UInt32(m_pipelineCaptureScratch.size()) / pipelineFrameBytes;

    AudioBufferList outAbl;
    outAbl.mNumberBuffers = 1;
    outAbl.mBuffers[0].mNumberChannels = UInt32(m_pipeline.channels);
    outAbl.mBuffers[0].mDataByteSize = outFrames * pipelineFrameBytes;
    outAbl.mBuffers[0].mData = m_pipelineCaptureScratch.data();

    const OSStatus cst =
        AudioConverterFillComplexBuffer(m_captureConverter, captureConverterInput,
                                        m_captureSource.get(), &outFrames,
                                        &outAbl, nullptr);
    // A converter that produced something AND reported an error still
    // produced something; keep it rather than dropping a frame over a
    // "no more input" status.
    if (cst != noErr && outFrames == 0) return noErr;
    if (outFrames > 0) {
        m_captureRing.write(m_pipelineCaptureScratch.data(),
                            int(outFrames * pipelineFrameBytes));
    }
    return noErr;
}

OSStatus DarwinVpioBackend::onRender(UInt32 frames, AudioBufferList* ioData)
{
    m_renderCallbacks.fetch_add(1, std::memory_order_relaxed);
    if (!ioData || ioData->mNumberBuffers == 0) return noErr;

    // The client format was set interleaved, so there is exactly one
    // buffer. Anything else means the unit is running a layout we did
    // not ask for, and the only safe output is silence.
    if (ioData->mNumberBuffers != 1) {
        for (UInt32 i = 0; i < ioData->mNumberBuffers; ++i) {
            if (ioData->mBuffers[i].mData)
                std::memset(ioData->mBuffers[i].mData, 0,
                            ioData->mBuffers[i].mDataByteSize);
        }
        return noErr;
    }

    char* out = static_cast<char*>(ioData->mBuffers[0].mData);
    const int outBytes = int(ioData->mBuffers[0].mDataByteSize);
    if (!out || outBytes <= 0) return noErr;

    if (!m_renderWanted) {
        std::memset(out, 0, size_t(outBytes));
        return noErr;
    }

    int produced = 0;
    if (m_renderPlan.action == FormatAction::Direct) {
        produced = m_playbackRing.read(out, outBytes);
    } else if (m_renderConverter) {
        const UInt32 unitFrameBytes = UInt32(bytesPerFrame(m_renderPlan.unit));
        UInt32 wantFrames = frames;
        if (wantFrames * unitFrameBytes > UInt32(outBytes))
            wantFrames = UInt32(outBytes) / unitFrameBytes;

        AudioBufferList outAbl;
        outAbl.mNumberBuffers = 1;
        outAbl.mBuffers[0].mNumberChannels = UInt32(m_renderPlan.unit.channels);
        outAbl.mBuffers[0].mDataByteSize = wantFrames * unitFrameBytes;
        outAbl.mBuffers[0].mData = out;

        UInt32 gotFrames = wantFrames;
        const OSStatus cst = AudioConverterFillComplexBuffer(
            m_renderConverter, renderConverterInput, m_renderSource.get(),
            &gotFrames, &outAbl, nullptr);
        if (cst != noErr && gotFrames == 0) gotFrames = 0;
        produced = int(gotFrames * unitFrameBytes);
    }

    if (produced < outBytes) {
        // Underrun. Silence, not the previous block: a repeated frame is
        // an audible buzz where a gap is a click, and a buzz sounds like
        // a broken app rather than a dropped packet.
        std::memset(out + produced, 0, size_t(outBytes - produced));
        m_playbackRing.noteUnderrun();
        m_renderUnderrunFrames.fetch_add(1, std::memory_order_relaxed);
    }
    return noErr;
}

} // namespace bsfchat::voice
