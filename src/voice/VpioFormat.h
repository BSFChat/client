#pragma once

// Format negotiation between the voice pipeline and a VoiceProcessingIO
// audio unit, decided as a value rather than as a side effect — the same
// split, and for the same reason, as AudioDevicePolicy.h next door.
//
// The problem
// -----------
// AudioWorker is hard-wired to 48 kHz, mono, signed 16-bit, 20 ms
// frames (AudioWorker::kSampleRate / kChannels / kFrameSamples). A VPIO
// unit is not obliged to give us that.
//
//   * On macOS the unit runs at the hardware device's rate. VPIO
//     strongly prefers 48 kHz and most Macs oblige, but a device locked
//     to 44.1 kHz by another application, or an aggregate device, does
//     not.
//   * On iOS the rate is AVAudioSession's, and `setPreferredSampleRate:`
//     is a REQUEST (see IosAudioSession.mm). The older hardware rate is
//     44.1 kHz, and 16 kHz turns up when a Bluetooth HFP headset takes
//     the route — which is exactly the case a speakerphone-echo fix has
//     to survive, because plugging in a headset is the first thing a
//     user does about echo.
//   * A Bluetooth route can also present 2 input channels.
//
// So the honest move is: ask the unit for the pipeline's format, read
// back what it actually accepted, and decide from the answer. That
// decision is what lives here. The three outcomes are Direct (the unit
// took our format — the common case, and no conversion runs at all),
// Convert (we own an AudioConverter for that bus), and Unsupported (the
// numbers are outside what a converter should be asked to do, and the
// caller falls back to the Qt path rather than shipping garbage audio).
//
// Deliberately free of CoreAudio and of Qt Multimedia: no
// AudioStreamBasicDescription, no AudioUnit, no QAudioFormat. That is
// what lets "a Bluetooth headset dragged the session to 16 kHz mono
// mid-call" be a unit test on a CI box with no sound card, instead of
// something only a phone can answer.

#include <QtGlobal>

#include <cmath>

namespace bsfchat::voice {

// What AudioWorker needs, flattened out of its constants.
struct PipelineAudioFormat {
    double sampleRate = 48000.0;
    int channels = 1;
    int frameSamples = 960;  // one 20 ms frame at the rate above

    int bytesPerSample() const { return 2; }  // Int16, everywhere
    int frameBytes() const { return frameSamples * channels * bytesPerSample(); }
};

// How the samples on a bus are laid out. The pipeline only ever speaks
// packed signed 16-bit; anything else has to go through a converter, and
// anything we cannot name has to send us back to the Qt path rather than
// be guessed at.
enum class SampleType {
    Int16,    // packed signed 16-bit, what AudioWorker uses end to end
    Float32,  // packed 32-bit float, what a CoreAudio unit falls back to
    Other,    // 24-bit, non-interleaved, anything else
};

// What the audio unit says it will actually run at on one bus.
struct StreamAudioFormat {
    double sampleRate = 0.0;
    int channels = 0;
    SampleType sampleType = SampleType::Int16;

    bool operator==(const StreamAudioFormat&) const = default;
};

enum class FormatAction {
    // The unit accepted the pipeline's format verbatim. Nothing to do,
    // and nothing is allowed to be done: an identity AudioConverter is
    // still a copy and still a latency source.
    Direct,
    // Rate and/or channel count differ. The caller owns a converter for
    // this bus.
    Convert,
    // Nothing sane to do. The caller must not open this backend.
    Unsupported,
};

struct FormatPlan {
    FormatAction action = FormatAction::Unsupported;
    // What the unit runs at. Echoed back so the caller can build the
    // converter and size its scratch buffers from one value.
    StreamAudioFormat unit;
    // unit.sampleRate / pipeline.sampleRate. 1.0 for Direct.
    double resampleRatio = 1.0;
    // Upper bound on unit-side frames that one pipeline frame becomes,
    // or that are needed to make one. Ceil plus one, because a
    // resampler's output length for a fixed input length varies by a
    // sample either way as its phase accumulator rolls.
    int unitFramesPerPipelineFrame = 0;
    // Why, for the log. A literal; never owned.
    const char* reason = "";

    bool usable() const { return action != FormatAction::Unsupported; }
};

// The widest rate ratio worth handing to a converter. 8x covers every
// real case (8 kHz narrowband HFP up to 48 kHz is 6x, 96 kHz down to
// 48 kHz is 2x) and rejects the values that mean something has gone
// wrong rather than something unusual has been plugged in.
inline constexpr double kMaxResampleRatio = 8.0;
// More than this many channels on a voice bus means we have
// misidentified the bus, not that someone has an 8-mic array.
inline constexpr int kMaxUnitChannels = 8;

// The format to ASK the unit for. Always the pipeline's own: the unit's
// client-side converter is better placed to do the work than we are,
// and when it accepts, the Convert path never runs.
inline StreamAudioFormat desiredClientFormat(const PipelineAudioFormat& pipeline)
{
    return StreamAudioFormat{pipeline.sampleRate, pipeline.channels,
                             SampleType::Int16};
}

// What to do given the format the unit actually reported after the set.
inline FormatPlan planConversion(const StreamAudioFormat& accepted,
                                 const PipelineAudioFormat& pipeline)
{
    FormatPlan plan;
    plan.unit = accepted;

    if (!(accepted.sampleRate > 0.0) || !std::isfinite(accepted.sampleRate)) {
        plan.action = FormatAction::Unsupported;
        plan.reason = "unit reported a non-positive or non-finite sample rate";
        return plan;
    }
    if (accepted.channels <= 0 || accepted.channels > kMaxUnitChannels) {
        plan.action = FormatAction::Unsupported;
        plan.reason = "unit reported an implausible channel count";
        return plan;
    }
    if (accepted.sampleType == SampleType::Other) {
        // Not "convert harder": a layout we cannot name is a layout we
        // cannot describe to a converter either, and the fallback to Qt
        // is a working call rather than a guess.
        plan.action = FormatAction::Unsupported;
        plan.reason = "unit reported a sample layout we do not handle";
        return plan;
    }
    if (!(pipeline.sampleRate > 0.0) || pipeline.channels <= 0
        || pipeline.frameSamples <= 0) {
        plan.action = FormatAction::Unsupported;
        plan.reason = "pipeline format is not valid";
        return plan;
    }

    const double ratio = accepted.sampleRate / pipeline.sampleRate;
    if (ratio > kMaxResampleRatio || ratio < 1.0 / kMaxResampleRatio) {
        plan.action = FormatAction::Unsupported;
        plan.resampleRatio = ratio;
        plan.reason = "sample-rate ratio outside the supported range";
        return plan;
    }

    plan.resampleRatio = ratio;
    // +1 for the resampler's phase: a fixed input block does not always
    // produce the same output length, and a buffer sized on the exact
    // ceiling truncates on the frames where it produces one more.
    plan.unitFramesPerPipelineFrame =
        static_cast<int>(std::ceil(pipeline.frameSamples * ratio)) + 1;

    if (accepted == desiredClientFormat(pipeline)) {
        plan.action = FormatAction::Direct;
        plan.resampleRatio = 1.0;
        plan.unitFramesPerPipelineFrame = pipeline.frameSamples;
        plan.reason = "unit accepted the pipeline format";
        return plan;
    }

    plan.action = FormatAction::Convert;
    if (accepted.sampleType != SampleType::Int16)
        plan.reason = "unit runs in 32-bit float";
    else if (accepted.channels != pipeline.channels)
        plan.reason = qFuzzyCompare(accepted.sampleRate, pipeline.sampleRate)
                          ? "channel count differs"
                          : "sample rate and channel count differ";
    else
        plan.reason = "sample rate differs";
    return plan;
}

// How much ring capacity one direction needs, in bytes of PIPELINE
// format (the rings only ever hold converted audio, so the unit's rate
// does not enter).
//
// `frames` is the depth in 20 ms frames. It has to cover the worst
// jitter between the CoreAudio callback cadence and the 10 ms pump —
// the unit may hand us a 5 ms buffer or a 40 ms one depending on the
// route, and a Bluetooth connect changes it mid-call. Rounded up to a
// power of two by AudioRingBuffer anyway; this function only has to be
// honest about the minimum.
inline int ringCapacityBytes(const PipelineAudioFormat& pipeline, int frames)
{
    if (frames < 2) frames = 2;
    return pipeline.frameBytes() * frames;
}

} // namespace bsfchat::voice
