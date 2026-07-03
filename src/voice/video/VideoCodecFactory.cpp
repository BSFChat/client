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

std::unique_ptr<VideoEncoder> VideoEncoder::create(VideoCodecKind kind,
                                                   bool preferHardware) {
    Q_UNUSED(preferHardware); // hardware backends land in P3
    if (kind == VideoCodecKind::H264) {
#ifdef BSFCHAT_HAVE_OPENH264
        return std::make_unique<OpenH264Encoder>();
#endif
    }
    return nullptr;
}

VideoEncoder::Caps VideoEncoder::queryCaps(VideoCodecKind kind) {
    if (kind == VideoCodecKind::H264) {
#ifdef BSFCHAT_HAVE_OPENH264
        return {false, false, false};
#endif
    }
    return {};
}

QStringList VideoEncoder::h264EncodeProfiles() {
#ifdef BSFCHAT_HAVE_OPENH264
    return {QStringLiteral("cb")};
#else
    return {};
#endif
}

std::unique_ptr<VideoDecoder> VideoDecoder::create(VideoCodecKind kind,
                                                   bool preferHardware) {
    Q_UNUSED(preferHardware);
    if (kind == VideoCodecKind::H264) {
#ifdef BSFCHAT_HAVE_OPENH264
        return std::make_unique<OpenH264Decoder>();
#endif
    }
    return nullptr;
}

QStringList VideoDecoder::h264DecodeProfiles() {
#ifdef BSFCHAT_HAVE_OPENH264
    // openh264 decodes CB/Main/High progressive.
    return {QStringLiteral("cb"), QStringLiteral("high")};
#else
    return {};
#endif
}
