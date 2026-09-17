#pragma once

#include <algorithm>
#include <cmath>

// ONE place for every number the rate controller argues with (S-17).
//
// Before this, the thresholds, the resolution ladder and the "is this
// bitrate enough for this size" judgement were three separate hunks of
// arithmetic inside VideoRateController::tick(), and the only way to
// ask "what would it do at 1080p30 with 3 Mbps?" was to run the whole
// Qt object through a timer. Everything here is constexpr-friendly
// pure arithmetic with no Qt, so the policy can be unit tested on its
// own and the controller is reduced to applying it.
//
// Two content profiles, because the right thing to give up is opposite
// for each:
//   SCREEN  is a desktop or a game. The killer artefact is unreadable
//           text / mushy HUD, which is a RESOLUTION problem. Judder is
//           annoying but legible, so fps is spent first and the long
//           edge is defended to the last rung.
//   CAMERA  is a face. A soft face at 30 fps reads as "fine"; a sharp
//           face at 12 fps reads as broken. Resolution goes first.
namespace videorate {

enum class Content { Screen, Camera };

// ---- Control-law thresholds (packet loss fraction, percent) --------
//
// Keyed on LOSS, never on a delivered/sent byte ratio. The byte ratio
// is structurally biased below 1.0 — the two ends sample their counters
// at different instants and the encoder's output is bursty — so on a
// clean LAN it reads 0.77-0.96 and a controller that keys off it can
// only ever descend. Measured over one session on a lossless LAN the
// byte ratio averaged 0.808 with 4 samples out of 4069 above 0.97.
struct Thresholds {
    // Below this the path is carrying everything we give it: probe up.
    static constexpr double kHealthyPct = 2.0;
    // [kHealthyPct, kTrimPct) — real but tolerable loss. HOLD, do not
    // cut. A band wide enough to be a resting place is the whole point:
    // a law with no hold band and a fixed loss reading walks itself to
    // the floor, which is exactly what the old one did.
    // [kTrimPct, kCutPct) — shave gently and let the path answer.
    static constexpr double kTrimPct = 6.0;
    // At or above this — or a keyframe-request storm — cut hard.
    static constexpr double kCutPct = 10.0;
    static constexpr int kKeyframeStorm = 3;

    // Multipliers applied per 500 ms tick.
    static constexpr double kCutFactor = 0.70;
    static constexpr double kTrimFactor = 0.95;
    // Probing is multiplicative and deliberately brisk while far below
    // the configured ceiling: a share that needs 12 s to reach the
    // bitrate the user paid for looks broken for 12 s.
    static constexpr double kProbeFactor = 1.10;
    static constexpr double kFastProbeFactor = 1.25;
    // …"far below" meaning under this fraction of the envelope max.
    static constexpr double kFastProbeBelow = 0.5;
    static constexpr int kProbeFloorKbps = 32;   // additive nudge

    // Hysteresis, in 500 ms ticks.
    static constexpr int kHealthyTicksBeforeProbe = 2;  // 1 s
    static constexpr int kUpshiftTicks = 6;             // 3 s comfortable
    static constexpr int kDwellTicks = 8;               // 4 s after a cut
};

// ---- Quality floors ------------------------------------------------
//
// Expressed as bits per pixel per frame, which is the only form that
// stays right across sizes and frame rates. The anchors:
//   SCREEN  1080p30 gameplay is unusable below ~4 Mbps → 0.0650 bpp,
//           which puts 720p30 at ~1.8 Mbps and 540p30 at ~1.0 Mbps.
//           (Gameplay, not a static desktop: small in-game text and
//           constant full-frame motion, so this sits well above what a
//           spreadsheet would need.)
//   CAMERA  720p30 is fine from ~1.2 Mbps → 0.0435 bpp, i.e. 1080p30
//           at ~2.7 Mbps.
// `kComfort` is the bitrate at which the NEXT size up is worth taking;
// the gap between floor and comfort is what stops the ladder flapping.
inline constexpr double floorBpp(Content c) {
    return c == Content::Screen ? 0.0650 : 0.0435;
}
inline constexpr double comfortBpp(Content c) { return floorBpp(c) * 1.6; }

// 16:9 is close enough — these bands are judgement, not measurement.
inline double pixelsAt(int longEdge) {
    return double(longEdge) * (double(longEdge) * 9.0 / 16.0);
}

// The least bitrate at which `longEdge`@`fps` is worth sending at all.
inline int minKbpsFor(Content c, int longEdge, int fps) {
    return int(floorBpp(c) * pixelsAt(longEdge) * double(std::max(fps, 1))
               / 1000.0);
}
// The bitrate at which `longEdge`@`fps` is comfortable — the bar an
// upshift has to clear, repeatedly, before it happens.
inline int comfortKbpsFor(Content c, int longEdge, int fps) {
    return int(comfortBpp(c) * pixelsAt(longEdge) * double(std::max(fps, 1))
               / 1000.0);
}

// ---- The degradation ladder ----------------------------------------
//
// A flat, ordered list of rungs so "step down" is ++index and there is
// exactly one place that decides whether fps or resolution gives way.
// Rung 0 is always the user's full envelope.
struct Rung {
    double resScale;
    double fpsScale;
};

// Screen: spend all the fps first (30 → 20 → 15), then walk the long
// edge down, because in-game text at 15 fps is readable and the same
// text at 30 fps and half the resolution is not.
inline constexpr Rung kScreenLadder[] = {
    {1.000, 1.0},  {1.000, 2.0 / 3.0}, {1.000, 0.5},
    {0.750, 0.5},  {0.500, 0.5},       {0.375, 0.5}, {0.250, 0.5},
};
// Camera: the opposite order — a soft face beats a stuttering one.
inline constexpr Rung kCameraLadder[] = {
    {1.000, 1.0},  {0.750, 1.0}, {0.500, 1.0}, {0.375, 1.0},
    {0.250, 1.0},  {0.250, 2.0 / 3.0}, {0.250, 0.5},
};
inline constexpr int kLadderRungs = 7;

inline const Rung& rungAt(Content c, int idx) {
    const int i = std::clamp(idx, 0, kLadderRungs - 1);
    return c == Content::Screen ? kScreenLadder[i] : kCameraLadder[i];
}

inline int edgeForRung(Content c, int maxLongEdge, int idx) {
    const int edge = int(double(maxLongEdge) * rungAt(c, idx).resScale);
    return std::max(160, edge & ~1);
}
inline int fpsForRung(Content c, int maxFps, int idx) {
    // 5 fps is the point below which a screen share stops being video
    // and becomes a slideshow; never ladder past it.
    return std::max(5, int(std::lround(double(maxFps)
                                       * rungAt(c, idx).fpsScale)));
}

// What the controller must never emit below at this rung: sending
// 250 kbps of 1080p is strictly worse than sending 250 kbps of 480p,
// and the ladder is what converts one into the other.
inline int rungMinKbps(Content c, int maxLongEdge, int maxFps, int idx) {
    return minKbpsFor(c, edgeForRung(c, maxLongEdge, idx),
                      fpsForRung(c, maxFps, idx));
}

} // namespace videorate
