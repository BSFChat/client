// Backend selection for VideoEncoder/VideoDecoder::create(). One
// translation unit so the platform #ifdef matrix lives in exactly one
// place:
//   macOS    → VideoToolbox (P3), openh264 fallback
//   Windows  → Media Foundation (P3)
//   Linux    → openh264
//   AV1 lossless → libaom (P7), all desktop platforms

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

std::unique_ptr<VideoEncoder> VideoEncoder::create(VideoCodecKind kind,
                                                   bool preferHardware) {
    if (kind == VideoCodecKind::H264) {
#ifdef BSFCHAT_HAVE_VIDEOTOOLBOX
        if (preferHardware) return std::make_unique<MacVTEncoder>();
#endif
#ifdef BSFCHAT_HAVE_MEDIAFOUNDATION
        // MFEncoder handles its own HW-MFT-then-SW-MFT ladder; the
        // preferHardware=false retry maps to software-only mode.
        return std::make_unique<MFEncoder>(preferHardware);
#endif
#ifdef BSFCHAT_HAVE_OPENH264
        return std::make_unique<OpenH264Encoder>();
#endif
    }
    Q_UNUSED(preferHardware);
    return nullptr;
}

VideoEncoder::Caps VideoEncoder::queryCaps(VideoCodecKind kind) {
    if (kind == VideoCodecKind::H264) {
#if defined(BSFCHAT_HAVE_VIDEOTOOLBOX) || defined(BSFCHAT_HAVE_MEDIAFOUNDATION)
        return {true, false, true};
#elif defined(BSFCHAT_HAVE_OPENH264)
        return {false, false, false};
#endif
    }
    return {};
}

QStringList VideoEncoder::h264EncodeProfiles() {
#if defined(BSFCHAT_HAVE_VIDEOTOOLBOX) || defined(BSFCHAT_HAVE_MEDIAFOUNDATION)
    // Platform encoders emit High (preferred — materially better
    // bits-per-quality for the near-lossless tier) or Baseline.
    return {QStringLiteral("high"), QStringLiteral("cb")};
#elif defined(BSFCHAT_HAVE_OPENH264)
    return {QStringLiteral("cb")};
#else
    return {};
#endif
}

std::unique_ptr<VideoDecoder> VideoDecoder::create(VideoCodecKind kind,
                                                   bool preferHardware) {
    if (kind == VideoCodecKind::H264) {
#ifdef BSFCHAT_HAVE_VIDEOTOOLBOX
        if (preferHardware) return std::make_unique<MacVTDecoder>();
#endif
#ifdef BSFCHAT_HAVE_MEDIAFOUNDATION
        return std::make_unique<MFDecoder>();
#endif
#ifdef BSFCHAT_HAVE_OPENH264
        return std::make_unique<OpenH264Decoder>();
#endif
    }
    Q_UNUSED(preferHardware);
    return nullptr;
}

QStringList VideoDecoder::h264DecodeProfiles() {
#if defined(BSFCHAT_HAVE_VIDEOTOOLBOX) || defined(BSFCHAT_HAVE_MEDIAFOUNDATION)
    return {QStringLiteral("cb"), QStringLiteral("high")};
#elif defined(BSFCHAT_HAVE_OPENH264)
    // openh264 decodes CB/Main/High progressive.
    return {QStringLiteral("cb"), QStringLiteral("high")};
#else
    return {};
#endif
}
