// Backend selection for VideoEncoder/VideoDecoder::create(). One
// translation unit so the platform #ifdef matrix lives in exactly one
// place:
//   macOS    → VideoToolbox (P3), openh264 fallback
//   iOS      → VideoToolbox, no fallback (see the CMake note)
//   Windows  → Media Foundation (P3)
//   Linux    → openh264
//   Android  → MediaCodec (NDK AMediaCodec), no fallback
//   AV1 lossless → libaom (P7), all desktop platforms
//
// Neither mobile platform has a software fallback and neither should.
// openh264 is a CPU encoder: on a phone that is battery, heat and
// sustained thermal throttling that degrades the whole call, audio
// included. The hardware codec is the only sensible one there, and on
// both platforms it is also the only one compiled in.
//
// H.265/HEVC (S-18) is the first codec whose availability is NOT a
// compile-time property of the build:
//   macOS    → VideoToolbox, but only on Macs with an HEVC media
//              engine, so both directions are probed at runtime.
//   Windows  → Media Foundation, and ONLY if an HEVC MFT is registered
//              (vendor driver or the Store HEVC extension). No new
//              link-time dependency; absent ⇒ never advertised.
//   Linux    → openh264 decodes H.264 only. No HEVC either way, so a
//              Linux build advertises h264 alone and every peer keeps
//              sending it H.264. That is the fallback working, not a
//              gap.
// The two h265*Supported() predicates below are the single source of
// truth for that, read by localCapsJson() (decode → advertised) and by
// the codec selector (encode → may be chosen).

#include "voice/video/VideoEncoder.h"
#include "voice/video/VideoDecoder.h"

#ifdef BSFCHAT_HAVE_OPENH264
#include "voice/video/OpenH264Encoder.h"
#include "voice/video/OpenH264Decoder.h"
#endif
#ifdef BSFCHAT_HAVE_VIDEOTOOLBOX
#include "voice/video/MacVTEncoder.h"
#include "voice/video/MacVTDecoder.h"
#endif
#ifdef BSFCHAT_HAVE_MEDIAFOUNDATION
#include "voice/video/MFEncoder.h"
#include "voice/video/MFDecoder.h"
#endif
#ifdef BSFCHAT_HAVE_MEDIACODEC
#include "voice/video/MediaCodecEncoder.h"
#include "voice/video/MediaCodecDecoder.h"
#endif
#ifdef BSFCHAT_HAVE_AOM
#include "voice/video/AomLosslessEncoder.h"
#include "voice/video/AomLosslessDecoder.h"
#endif

bool VideoEncoder::h265EncodeSupported() {
#ifdef BSFCHAT_HAVE_VIDEOTOOLBOX
    return MacVTEncoder::hevcEncodeSupported();
#elif defined(BSFCHAT_HAVE_MEDIAFOUNDATION)
    return MFEncoder::hevcEncodeSupported();
#else
    return false;
#endif
}

bool VideoDecoder::h265DecodeSupported() {
#ifdef BSFCHAT_HAVE_VIDEOTOOLBOX
    return MacVTDecoder::hevcDecodeSupported();
#elif defined(BSFCHAT_HAVE_MEDIAFOUNDATION)
    return MFDecoder::hevcDecodeSupported();
#else
    return false;
#endif
}

std::unique_ptr<VideoEncoder> VideoEncoder::create(VideoCodecKind kind,
                                                   bool preferHardware) {
    if (kind == VideoCodecKind::H265) {
        // No software fallback on purpose: a software HEVC encoder
        // cannot hold a realtime 1080p30 screen share on the CPU
        // budget left over from capture, scaling and Opus, and the
        // whole point of the codec here is to spend less, not more.
        if (!h265EncodeSupported()) return nullptr;
#ifdef BSFCHAT_HAVE_VIDEOTOOLBOX
        return std::make_unique<MacVTEncoder>();
#elif defined(BSFCHAT_HAVE_MEDIAFOUNDATION)
        return std::make_unique<MFEncoder>(preferHardware);
#else
        return nullptr;
#endif
    }
    if (kind == VideoCodecKind::H264) {
#ifdef BSFCHAT_HAVE_VIDEOTOOLBOX
        if (preferHardware) return std::make_unique<MacVTEncoder>();
#endif
#ifdef BSFCHAT_HAVE_MEDIAFOUNDATION
        // MFEncoder handles its own HW-MFT-then-SW-MFT ladder; the
        // preferHardware=false retry maps to software-only mode.
        return std::make_unique<MFEncoder>(preferHardware);
#endif
#ifdef BSFCHAT_HAVE_MEDIACODEC
        // Android's only backend, so `preferHardware` has nothing to
        // select between — and the VideoSendPipeline retry that passes
        // false lands back here and gets the same encoder. That is the
        // honest answer: there is no software H.264 in this build to
        // fall back to, deliberately.
        if (MediaCodecEncoder::h264EncodeSupported())
            return std::make_unique<MediaCodecEncoder>();
        return nullptr;
#endif
#ifdef BSFCHAT_HAVE_OPENH264
        return std::make_unique<OpenH264Encoder>();
#endif
    }
#ifdef BSFCHAT_HAVE_AOM
    if (kind == VideoCodecKind::Av1Lossless)
        return std::make_unique<AomLosslessEncoder>();
#endif
    Q_UNUSED(preferHardware);
    return nullptr;
}

VideoEncoder::Caps VideoEncoder::queryCaps(VideoCodecKind kind) {
    if (kind == VideoCodecKind::H265) {
        if (!h265EncodeSupported()) return {};
        return {true, false, false};   // hardware, not lossless, no H.264 High axis
    }
    if (kind == VideoCodecKind::H264) {
#if defined(BSFCHAT_HAVE_VIDEOTOOLBOX) || defined(BSFCHAT_HAVE_MEDIAFOUNDATION)
        return {true, false, true};
#elif defined(BSFCHAT_HAVE_MEDIACODEC)
        // Hardware, but NOT High profile — see h264EncodeProfiles below.
        // Runtime-probed: `hardware` here means "there is a hardware
        // encoder", and on a device with no AVC encoder at all the
        // honest answer is an empty Caps, not a claim create() cannot
        // honour.
        if (!MediaCodecEncoder::h264EncodeSupported()) return {};
        return {true, false, false};
#elif defined(BSFCHAT_HAVE_OPENH264)
        return {false, false, false};
#endif
    }
#ifdef BSFCHAT_HAVE_AOM
    if (kind == VideoCodecKind::Av1Lossless) return {false, true, false};
#endif
    return {};
}

QStringList VideoEncoder::h264EncodeProfiles() {
#if defined(BSFCHAT_HAVE_VIDEOTOOLBOX) || defined(BSFCHAT_HAVE_MEDIAFOUNDATION)
    // Platform encoders emit High (preferred — materially better
    // bits-per-quality for the near-lossless tier) or Baseline.
    return {QStringLiteral("high"), QStringLiteral("cb")};
#elif defined(BSFCHAT_HAVE_MEDIACODEC)
    // Constrained Baseline only, and this is a CLAIM, not a limit.
    //
    // negotiatedH264Profile() is a whole-call decision: it picks High
    // only when every peer can decode it AND the local encoder claims
    // it. Claiming High from Android would mean a configure() that asks
    // for it, and asking an Android encoder for a profile means also
    // supplying a matching level, having no NDK capability query below
    // API 36 to check either against, and getting a hard configure
    // failure on the devices that refuse — i.e. no video at all from
    // that phone, to fix a bitrate inefficiency. The encoder is left on
    // its default (Baseline or Constrained Baseline everywhere in
    // practice) and we advertise what we are sure of.
    //
    // Cost of the conservative claim: a call with a phone in it encodes
    // Baseline on every sender. That is the correct trade against a
    // black tile, and it is what a Linux peer already does to the same
    // call today.
    if (!MediaCodecEncoder::h264EncodeSupported()) return {};
    return {QStringLiteral("cb")};
#elif defined(BSFCHAT_HAVE_OPENH264)
    return {QStringLiteral("cb")};
#else
    return {};
#endif
}

namespace {
VideoDecoder::Factory& decoderFactoryOverride() {
    static VideoDecoder::Factory f;
    return f;
}
} // namespace

void VideoDecoder::setFactoryForTest(Factory f) {
    decoderFactoryOverride() = std::move(f);
}

std::unique_ptr<VideoDecoder> VideoDecoder::create(VideoCodecKind kind,
                                                   bool preferHardware) {
    // One null check per stream — decoders are built once each, not
    // once per access unit.
    if (auto& f = decoderFactoryOverride()) return f(kind, preferHardware);
    if (kind == VideoCodecKind::H265) {
        if (!h265DecodeSupported()) return nullptr;
#ifdef BSFCHAT_HAVE_VIDEOTOOLBOX
        return std::make_unique<MacVTDecoder>();
#elif defined(BSFCHAT_HAVE_MEDIAFOUNDATION)
        return std::make_unique<MFDecoder>();
#else
        return nullptr;
#endif
    }
    if (kind == VideoCodecKind::H264) {
#ifdef BSFCHAT_HAVE_VIDEOTOOLBOX
        if (preferHardware) return std::make_unique<MacVTDecoder>();
#endif
#ifdef BSFCHAT_HAVE_MEDIAFOUNDATION
        return std::make_unique<MFDecoder>();
#endif
#ifdef BSFCHAT_HAVE_MEDIACODEC
        if (MediaCodecDecoder::h264DecodeSupported())
            return std::make_unique<MediaCodecDecoder>();
        return nullptr;
#endif
#ifdef BSFCHAT_HAVE_OPENH264
        return std::make_unique<OpenH264Decoder>();
#endif
    }
#ifdef BSFCHAT_HAVE_AOM
    if (kind == VideoCodecKind::Av1Lossless)
        return std::make_unique<AomLosslessDecoder>();
#endif
    Q_UNUSED(preferHardware);
    return nullptr;
}

QStringList VideoDecoder::h264DecodeProfiles() {
#if defined(BSFCHAT_HAVE_VIDEOTOOLBOX) || defined(BSFCHAT_HAVE_MEDIAFOUNDATION)
    return {QStringLiteral("cb"), QStringLiteral("high")};
#elif defined(BSFCHAT_HAVE_MEDIACODEC)
    // Both, unlike the encode side, and the asymmetry is deliberate.
    //
    // The Android CDD has required an AVC decoder handling Baseline AND
    // High profile up to the device's supported resolution since API 21;
    // every hardware AVC decoder shipped in the last decade does High.
    // Declining to claim it here would not make the phone safer — it
    // would drag the WHOLE call down to Baseline, because
    // negotiatedH264Profile() needs unanimity among viewers, so the
    // desktops would stop sending High to each other the moment a phone
    // joined. That is a real, permanent quality cost on every other
    // participant to hedge against a device class that does not exist.
    //
    // Still gated on the probe, because the list feeds localCapsJson()
    // and advertising a codec create() would then refuse is the exact
    // failure this port exists to remove — just pointing the other way.
    if (!MediaCodecDecoder::h264DecodeSupported()) return {};
    return {QStringLiteral("cb"), QStringLiteral("high")};
#elif defined(BSFCHAT_HAVE_OPENH264)
    // openh264 decodes CB/Main/High progressive.
    return {QStringLiteral("cb"), QStringLiteral("high")};
#else
    return {};
#endif
}
