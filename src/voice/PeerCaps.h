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
    QStringList videoCodecs;           // e.g. {"h264"}
    QStringList h264ProfilesDecode;    // e.g. {"cb", "high"}
    QStringList h264ProfilesEncode;    // e.g. {"high"}
    QStringList lossless;              // e.g. {"av1-dc"} — AV1 over reliable data channel

    static PeerCaps fromJson(const nlohmann::json& j) {
        PeerCaps c;
        if (!j.is_object()) return c;
        c.videoRtp = j.value("video_rtp", 0) != 0;
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
            {"video_codecs", arr(videoCodecs)},
            {"h264_profiles_decode", arr(h264ProfilesDecode)},
            {"h264_profiles_encode", arr(h264ProfilesEncode)},
            {"lossless", arr(lossless)},
        };
    }
};
