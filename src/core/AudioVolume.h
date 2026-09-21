#pragma once

// The volume settings' meaning: what a stored percentage does to the
// audio, and how values stored by older builds are carried forward.
//
// Pure and header-only so that Settings (GUI thread), AudioEngine and
// the unit tests share one definition without any of them linking the
// others.
//
// History
// -------
// audio/inputVolume and audio/outputVolume have existed since the Client
// Settings popup was added (3614afa), as 0..100 sliders — and nothing in
// the client ever read them. The sliders were visible, did nothing, and
// were hidden in edff6cf ("controls that do nothing"). They are now wired
// into the audio worker, and the output range extends to 200% because
// the question that prompted this was literally "are we not offering
// 100% output volume?" — the practical answer users want is a way to go
// louder than the source.
//
// Taper
// -----
// gain = (percent / 100)^2. Square law, for three reasons:
//   * 100% is exactly unity, so the default changes nothing;
//   * perceived loudness goes roughly as amplitude^0.6, so a square-law
//     slider is close to linear in how loud it *sounds* — 50% is -12 dB,
//     which reads as about half as loud, where a linear-amplitude slider
//     does nothing audible over its top half and everything in its
//     bottom tenth;
//   * 200% is +12 dB, a little over twice as loud, which is the useful
//     amount of boost for a quiet sender and not so much that every peak
//     lives in the limiter.
// The same taper serves input, output and per-user volume.
//
// Boost cannot clip harshly: everything after a gain stage goes through
// a PeakLimiter (voice/VoiceGain.h) with a -1 dBFS ceiling, so a boosted
// peak is turned down smoothly rather than flat-topped.
//
// Migration (schema 1 -> 2)
// -------------------------
// A stored value other than 100 was set while the slider did nothing:
// whoever dragged output to 30 heard no change and so has no idea it is
// there. Honouring it now would make their audio suddenly 21 dB quieter
// — the exact complaint this change fixes. So on first run of a build
// that applies the settings, any legacy value is reset to 100% (unity),
// once, and audio/volumeSchema records that it happened. From schema 2
// on, the stored value is honoured as-is.

#include <algorithm>
#include <optional>

namespace bsfchat::audio {

inline constexpr int kVolumeMin = 0;
inline constexpr int kVolumeUnity = 100;
inline constexpr int kVolumeMax = 200;

// Written to audio/volumeSchema once the migration below has run.
inline constexpr int kVolumeSchema = 2;

inline int clampVolume(int percent) {
    return std::clamp(percent, kVolumeMin, kVolumeMax);
}

inline float volumeToGain(int percent) {
    const float p = static_cast<float>(clampVolume(percent)) / kVolumeUnity;
    return p * p;
}

// What to keep for a volume key given what is stored and the schema it
// was stored under (0 or 1 for any build before this one — the key did
// not exist). nullopt means "nothing stored", which is unity either way.
inline int migrateStoredVolume(std::optional<int> stored, int storedSchema) {
    if (!stored) return kVolumeUnity;
    if (storedSchema < kVolumeSchema) return kVolumeUnity;
    return clampVolume(*stored);
}

} // namespace bsfchat::audio
