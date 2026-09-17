#pragma once

#include "voice/PeerCaps.h"
#include "voice/video/VideoCodec.h"

#include <QList>
#include <QString>

// Which RTP video codec a stream is encoded in, decided ONCE per stream
// for the whole mesh (S-18).
//
// The constraint that shapes everything here: a mesh sender encodes each
// stream EXACTLY ONCE and fans the same access units out to every
// viewer. There is no per-viewer encode — that is an SFU's job, and
// running two encoders for a three-person call would cost more than
// H.265 saves. So the codec is the *intersection* over the whole
// audience, and one viewer that cannot decode H.265 puts everybody back
// on H.264.
//
// Everything below is a pure function of (user preference, local encode
// capability, the viewers' advertised caps) so the truth table can be
// unit tested without a peer connection, a settings store or a call.
namespace videocodec {

// Settings::videoCodecPreference, parsed.
enum class Preference {
    Auto,        // H.265 when it is available to everyone (default)
    PreferHevc,  // same rule — H.265 still requires every viewer to
                 // decode it, because there is no second encode to fall
                 // back to per-viewer. This differs from Auto only in
                 // that it survives a future "Auto also weighs CPU /
                 // battery / measured bitrate" refinement.
    H264Only,    // never negotiate H.265, whatever anyone advertises
};

inline Preference preferenceFromString(const QString& s) {
    if (s.compare(QStringLiteral("preferHevc"), Qt::CaseInsensitive) == 0)
        return Preference::PreferHevc;
    if (s.compare(QStringLiteral("h264Only"), Qt::CaseInsensitive) == 0)
        return Preference::H264Only;
    return Preference::Auto;
}

inline QString preferenceToString(Preference p) {
    switch (p) {
    case Preference::PreferHevc: return QStringLiteral("preferHevc");
    case Preference::H264Only:   return QStringLiteral("h264Only");
    case Preference::Auto:       break;
    }
    return QStringLiteral("auto");
}

// One connected peer, as the selector sees it: its advertised caps and
// whether they have arrived yet.
struct Viewer {
    PeerCaps caps;
    bool capsKnown = false;
};

// Does this peer get a vote?
//
// Only peers actually on the RTP video path do. A peer whose caps are
// known and which cannot take RTP video at all (Android, or any build
// with no H.264 decoder) is served by the legacy JPEG fan-out, which is
// a separate encode entirely — letting it veto H.265 would mean one
// Android in the room pins every desktop viewer to H.264 for nothing.
//
// A peer whose caps have NOT arrived yet does vote, and votes no: it may
// turn out to be an rc.19 build that only speaks H.264, and starting on
// H.265 and rebuilding a second later is worse than starting correctly.
inline bool viewerVotes(const Viewer& v) {
    return !v.capsKnown
        || peerCanReceiveRtpVideo(v.caps, v.capsKnown, videoCodecIdH264())
        || peerCanReceiveRtpVideo(v.caps, v.capsKnown, videoCodecIdH265());
}

// Does this peer's vote permit H.265?
inline bool viewerDecodesHevc(const Viewer& v) {
    return peerCanReceiveRtpVideo(v.caps, v.capsKnown, videoCodecIdH265());
}

// THE RULE. H.265 iff the user has not forbidden it, this build has an
// H.265 encoder, and every voting viewer decodes H.265.
//
// With no viewers at all the condition is vacuously true. That is the
// honest reading of "every viewer supports it", and it costs nothing in
// practice: the send path only encodes once a video-capable peer exists,
// and the first joiner that cannot decode H.265 flips the answer back
// on the very next evaluation.
inline VideoCodecKind select(Preference pref, bool localEncoderAvailable,
                             const QList<Viewer>& viewers) {
    if (pref == Preference::H264Only) return VideoCodecKind::H264;
    if (!localEncoderAvailable) return VideoCodecKind::H264;
    for (const Viewer& v : viewers) {
        if (!viewerVotes(v)) continue;
        if (!viewerDecodesHevc(v)) return VideoCodecKind::H264;
    }
    return VideoCodecKind::H265;
}

} // namespace videocodec
