#pragma once

#include <QString>

// The user-facing half of the codec decision (S-18), split out from
// VideoCodecSelect.h so that Settings — and anything else that only
// needs to read or write the preference — does not have to pull in
// PeerCaps and with it nlohmann/json.
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

} // namespace videocodec
