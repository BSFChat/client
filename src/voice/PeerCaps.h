#pragma once

#include <QStringList>
#include <nlohmann/json.hpp>

// Media capabilities a peer advertises in the `bsfchat_caps` field of
// its m.call.invite / m.call.answer content (and refreshes mid-call
// via the 0x04 control channel).
//
// The capability exchange exists because SDP alone can't be used for
// discovery here: legacy clients run libdatachannel with NO_MEDIA, and
// feeding them an offer containing a video m-line breaks the whole
// call. So the initial offer/answer stays audio+datachannel-only, and
// video m-lines are added by renegotiation ONLY after the peer's caps
// prove it can take them.
//
// Codec/profile lists are advertised rather than assumed because the
// encoder/decoder matrix is asymmetric per platform (e.g. openh264 on
// Linux encodes Constrained Baseline only but decodes High): a sender
// picks the best profile in the intersection of its encode caps and
// the peer's decode caps.
struct PeerCaps {
    bool videoRtp = false;             // understands renegotiation + RTP video tracks
    QStringList videoCodecs;           // e.g. {"h264", "h265"} — DECODE side
    QStringList h264ProfilesDecode;    // e.g. {"cb", "high"}
    QStringList h264ProfilesEncode;    // e.g. {"high"}
    QStringList lossless;              // e.g. {"av1-dc"} — AV1 over reliable data channel
    // Understands the separate reliable+ordered "control" data channel
    // (S-2). MUST be advertised before that channel is opened toward a
    // peer: a build without this flag dispatches every non-lossless
    // incoming channel into setupDataChannel(), which would rebind its
    // audio channel to ours and push its Opus down a reliable, ordered
    // channel — head-of-line blocking on the audio path, to fix video.
    bool controlDc = false;

    // True when the peer advertises `codec` as one it can DECODE.
    // Case-insensitive: the field is free-form JSON from another build.
    bool decodes(const QString& codec) const {
        return videoCodecs.contains(codec, Qt::CaseInsensitive);
    }

    static PeerCaps fromJson(const nlohmann::json& j) {
        PeerCaps c;
        if (!j.is_object()) return c;
        c.videoRtp = j.value("video_rtp", 0) != 0;
        c.controlDc = j.value("control_dc", 0) != 0;
        auto strList = [&j](const char* key) {
            QStringList out;
            for (const auto& v : j.value(key, nlohmann::json::array()))
                if (v.is_string())
                    out << QString::fromStdString(v.get<std::string>());
            return out;
        };
        c.videoCodecs        = strList("video_codecs");
        c.h264ProfilesDecode = strList("h264_profiles_decode");
        c.h264ProfilesEncode = strList("h264_profiles_encode");
        c.lossless           = strList("lossless");
        return c;
    }

    nlohmann::json toJson() const {
        auto arr = [](const QStringList& l) {
            nlohmann::json a = nlohmann::json::array();
            for (const auto& s : l) a.push_back(s.toStdString());
            return a;
        };
        return {
            {"video_rtp", videoRtp ? 1 : 0},
            {"control_dc", controlDc ? 1 : 0},
            {"video_codecs", arr(videoCodecs)},
            {"h264_profiles_decode", arr(h264ProfilesDecode)},
            {"h264_profiles_encode", arr(h264ProfilesEncode)},
            {"lossless", arr(lossless)},
        };
    }
};

// The codec identifiers the RTP video path advertises and sends.
// `video_codecs` lists what this build can DECODE; encode capability is
// local knowledge and is never advertised (a peer has no use for it —
// it never asks us to send, it only has to be able to receive).
inline QString videoCodecIdH264() { return QStringLiteral("h264"); }
inline QString videoCodecIdH265() { return QStringLiteral("h265"); }

// ---------------------------------------------------------------------
// Who gets which video path (S-1)
// ---------------------------------------------------------------------
// These two predicates are the whole legacy/RTP routing decision, kept
// here as free functions so they can be unit-tested without a live
// peer connection — the bug they encode was a one-line misjudgement
// that made Android receive NOTHING once a desktop shared.
//
// The old rule keyed "legacy" on whether the RTP TRACK was open. That
// is not the same question:
//   * a peer with video_rtp:1 but no decoder (Android without openh264
//     or libaom) still gets a NEGOTIATED, OPEN track — the m-lines are
//     in the initial offer regardless — so it was classified capable,
//     skipped by the JPEG fan-out, and pushed H.264 it cannot decode;
//   * a peer whose track has not opened yet is genuinely in the
//     transition gap and does need JPEG for those few seconds.
// Decode capability answers the first; track state answers the second;
// both are needed.

// True when `codec` video over RTP will actually be decodable by this
// peer. Unknown caps ⇒ false: before the handshake we assume nothing.
inline bool peerCanReceiveRtpVideo(const PeerCaps& caps, bool capsKnown,
                                   const QString& codec) {
    return capsKnown && caps.videoRtp && caps.decodes(codec);
}

// True when the reliable "control" data channel may be opened toward
// this peer (S-2). Unknown caps, or caps without the flag, mean a build
// that would rebind its AUDIO channel to whatever label it is handed —
// so control traffic stays on the audio channel for those peers, which
// is what every build did before this existed.
inline bool peerUsesControlChannel(const PeerCaps& caps, bool capsKnown) {
    return capsKnown && caps.controlDc;
}

// True when the peer must be served the legacy JPEG stills path:
// either it cannot decode our RTP codec at all, or it can but its
// track for this stream has not opened yet.
inline bool peerNeedsLegacyJpeg(const PeerCaps& caps, bool capsKnown,
                                bool videoTrackOpen, const QString& codec) {
    if (!peerCanReceiveRtpVideo(caps, capsKnown, codec)) return true;
    return !videoTrackOpen;
}
