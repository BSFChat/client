#pragma once

#include "voice/video/VideoCodec.h"

#include <QtGlobal>

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
//           text / mushy HUD, which is a RESOLUTION problem, so the
//           long edge is defended to the last rung — but judder is
//           what the owner reported and said he would trade sharpness
//           to avoid, so the first step down is a modest resolution
//           cut at full frame rate (see kScreenLadder).
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

    // ---- Knee memory (the sawtooth fix, 2026-09-21) ----------------
    //
    // The field log behind the "sharp but choppy" report shows the law
    // above running as a sawtooth: probe ×1.25 a tick from ~25 Mbps,
    // hit loss around 45-60, cut ×0.70, wait one second, probe ×1.25
    // again — 117 cuts in nine minutes, every peak a burst of damaged
    // frames and keyframe requests. Nothing remembered where the loss
    // had started, so every climb ran into the same wall at full speed.
    //
    // The bitrate at which a trim or cut fired is the KNEE. Below the
    // knee band the probe still sprints (a cut must recover quickly);
    // a sprint lands at the band edge, 85 % of the knee, never past it.
    // Inside the band it creeps: one ×1.03 step every 4 s, so climbing
    // from the band edge back to the knee takes ~22 s instead of the
    // one or two ticks it took before. Against a hard capacity that
    // turns a loss event every ~3 s (each one a burst of damaged frames
    // and keyframe requests) into one every ~20-25 s, while the rate
    // sits at 85-100 % of capacity in between rather than sawtoothing
    // between 70 and 110 %.
    //
    // A knee is forgotten when the rate passes 1.15× it without loss
    // (the path has grown — creeping gets there in ~20 s from the knee)
    // or 30 s after the last loss event, because capacity on WiFi and
    // shared uplinks moves on that timescale and a stale ceiling would
    // pin the share below what the path now carries. Every loss event
    // refreshes it, so against a genuinely fixed ceiling it persists.
    static constexpr double kKneeBand = 0.85;          // "near" = ≥ 85 %
    static constexpr double kKneeForgetAbove = 1.15;
    static constexpr double kKneeProbeFactor = 1.03;
    static constexpr int kKneeProbeEveryTicks = 8;     // 4 s
    // In ticks like every other hysteresis constant here (60 = 30 s),
    // so the unit tests can step it without a clock.
    static constexpr int kKneeMemoryTicks = 60;
    // Only loss at (or above) a rate the path has NOT recently carried
    // cleanly marks a knee. Loss at a rate well below one that was
    // clean a few seconds ago is not the path's capacity — it is a
    // burst (WiFi, a cross-traffic spike) — and letting it set the knee
    // made the knee ratchet: cut at K, recover to 0.85K, next burst sets
    // the knee there, and so on geometrically. With a 20 % burst every
    // 5 s that walked a 1080p share down to 960 px inside 100 s. Such
    // loss is still graded and still cuts (frames were lost); it just
    // teaches the controller nothing about where the wall is. The
    // "recently clean" rate expires on the same 30 s clock as the knee,
    // so a real capacity drop is learned within half a minute.
    static constexpr double kKneeNeedsCleanRatio = 0.95;

    // ---- Loss estimate smoothing -----------------------------------
    //
    // Grading frames exposes a statistical problem the packet law hid:
    // at modest bitrates a 500 ms window holds ~100 packets, so 0.4 %
    // random loss arrives as "0 lost" most ticks and "1 lost" now and
    // then — and one lost packet in a window of ~10 frames reads as
    // ~10 % frame damage whatever the bitrate. Graded raw, every
    // isolated drop is a trim and the rate random-walks downward.
    //
    // So each peer's counts are smoothed (count-weighted EWMA, decay
    // 0.7 per report ≈ 1.4 s time constant), and a single window is
    // allowed to override the smoothed value only when it lost at
    // least kMinLostForRaw packets — enough that it is a burst, not a
    // coincidence. Real congestion loses dozens of packets per window
    // and is still graded on the tick it happens.
    //
    // The smoothed value may HOLD the rate (and so block probing) but
    // never trims or cuts on its own: a trim or cut needs loss in the
    // current window. Without that rule the tail of one burst — loss
    // measured at the rate BEFORE the cut, multiplied by the frame size
    // still in flight — cut again on each of the next five ticks and
    // took a 25 Mbps path to 3 Mbps for a single overshoot.
    static constexpr double kLossEwmaDecay = 0.7;
    static constexpr quint64 kMinLostForRaw = 3;
};

// ---- Frame damage (why packet loss alone is the wrong input) --------
//
// The receiver drops a whole access unit on ANY RTP hole and freezes
// until the next keyframe (VideoReceivePipeline, "loss-suspect AU
// dropped, waiting for keyframe"); there is no NACK/RTX on the mesh
// path. So the viewer's experience is set by the fraction of FRAMES
// that lose at least one packet, and that depends on how many packets
// a frame is spread over:
//
//     damage = 1 - (1 - p)^n        p = packet loss, n = packets/frame
//
// At 1 % packet loss: n = 3 (a small P-frame) damages 3 % of frames;
// n = 150 (1080p at 30 Mbps / 20 fps — the field case) damages 78 %.
// The old law graded p directly, so it called 0.4 % loss "healthy"
// and kept climbing while most frames were being thrown away. That is
// exactly "really good quality, but very choppy": every frame that
// arrives is big and sharp, and most of them never arrive.
//
// Grading damage instead of p keeps the existing thresholds (they now
// mean "percent of frames lost", which is what they were always meant
// to protect) and costs nothing where frames are small: with n = 1 the
// two are identical, which is also what an unknown n defaults to.
//
// n is MEASURED on the send side (mean RTP packets per encoded access
// unit over the window), not derived from the target bitrate: a static
// desktop at a 20 Mbps target emits 2-packet P-frames and must not be
// punished as if every frame were 150 packets. The mean slightly
// OVER-states damage when an IDR is in the window (1-(1-p)^n is
// concave in n); for a controller whose failure mode is "too
// aggressive", erring that way is the safe side.
inline double frameDamagePct(double packetLossPct, double packetsPerFrame) {
    const double p = std::clamp(packetLossPct / 100.0, 0.0, 1.0);
    const double n = std::max(packetsPerFrame, 1.0);
    return 100.0 * (1.0 - std::pow(1.0 - p, n));
}

// ---- Sender capacity (the frame-rate-aware half) --------------------
//
// Second input to the controller, alongside the receiver reports: can
// THIS MACHINE produce the frames it is being asked for? Fed from
// videosend::Window each tick. What it does depends on what failed:
//
//   ENCODE-BOUND  the worker's depth-1 slot overflows, or convert +
//                 encode takes ~a whole frame interval. Cost is roughly
//                 proportional to PIXELS (libyuv scale + encoder
//                 motion search), so shed resolution and keep the
//                 frame rate: the owner asked for smooth over sharp,
//                 and 1440 px at 30 fps is smoother than 1920 px at an
//                 uneven 19. Only after the resolution steps run out
//                 does the frame rate come down.
//   CAPTURE-BOUND a polling capturer delivered fewer frames than asked.
//                 Pixels cannot help (the capturer grabs at source
//                 size before we ever scale), so ask for a frame rate
//                 it can actually sustain: a steady 20 fps looks better
//                 than a nominal 30 that arrives as an uneven 19-27, and
//                 the timer stops queueing screenshots it cannot serve.
//   CADENCE       frames arrived but were overwritten before a push.
//                 Structural (two timers beating), fixed in
//                 ScreenShareController by pushing on arrival; logged,
//                 and it scales the bitrate, but it never moves the
//                 caps — neither lever would help.
//
// Independently of which lever moves, the BITRATE follows the frame
// rate actually being sent: target × (sent fps / asked fps). This is
// the owner's "scale bitrate down as the FPS tanks", and with the
// frame-damage model above it is not just thrift — a bitrate held
// constant while frames get scarcer makes every surviving frame bigger,
// i.e. spread over more packets, i.e. MORE likely to be lost. Keeping
// bytes-per-frame constant keeps fragility constant.
struct SenderPolicy {
    // Downshift when fewer than this share of the asked frames go out,
    // for kDownTicks consecutive evaluations with positive evidence.
    // 0.80 sits well clear of the 0.90 "meeting" line in
    // videosend::Window, so a sender hovering at 85-90 % neither
    // triggers nor counts as recovered — that gap IS the hysteresis.
    static constexpr double kDeficitRatio = 0.80;
    // 4 ticks = 2 s. Long enough to ride out an IDR (the slowest frame
    // an encoder emits), a GC-style main-thread stall or a window
    // being dragged; short enough that a genuinely overloaded share
    // stops stuttering within a couple of seconds.
    static constexpr int kDownTicks = 4;
    // Upshift needs predicted headroom — the NEXT level's estimated
    // cost under this share of its frame interval — sustained for the
    // wait below. 0.60 leaves room for scene changes (a full-screen
    // cut costs 2-3× a static frame) without bouncing straight back.
    static constexpr double kUpHeadroom = 0.60;
    // First upshift attempt waits 10 s; each attempt that is followed
    // by a downshift within kFailedUpWindowTicks doubles the wait, to
    // at most 60 s. Every step rebuilds the encoder session and costs
    // an IDR (~150-400 KB at 1080p), so a loop that retried every few
    // seconds would itself be a source of stutter. With 10 s minimum
    // and doubling, a machine that simply cannot sustain the higher
    // level pays at most one failed attempt per minute: 2 s of stutter
    // plus two IDRs, i.e. ~3 % of the time.
    static constexpr int kUpWaitTicks = 20;            // 10 s
    static constexpr int kUpWaitMaxTicks = 120;        // 60 s
    static constexpr int kFailedUpWindowTicks = 20;    // 10 s
    // After ANY sender-driven change, hold still this long so the
    // windows measure the new configuration, not the transition.
    static constexpr int kDwellTicks = 20;             // 10 s
    // EWMA weight per 500 ms window: time constant ≈ 1.4 s. A 30 fps
    // window holds 15 frames, so single-window rates move in steps of
    // 2 fps; smoothing over ~3 windows removes that quantisation
    // without making the 2 s trigger meaningfully slower.
    static constexpr double kEwmaAlpha = 0.3;
    // The bitrate never scales below this share of the network
    // allowance: at 5 fps of an asked 30, a 1/6 budget would starve
    // the frames that do go out.
    static constexpr double kMinBitrateScale = 0.25;
    // Bitrate scaling is quantised so the encoder is not re-targeted
    // for every 0.3 fps wobble.
    static constexpr double kBitrateScaleStep = 0.05;
};

// Resolution steps available to an encode-bound sender, as fractions
// of the envelope's long edge. Stops at 0.5: below that a screen share
// is unreadable and the right answer is fewer frames, not fewer pixels.
inline constexpr double kSenderEdgeScales[] = {1.0, 0.75, 0.625, 0.5};
inline constexpr int kSenderEdgeSteps = 4;

// Frame rates the sender caps to. Snapping to a short list means a
// capturer wobbling between 22 and 24 fps does not rebuild the encoder
// session (an IDR) every time the measurement moves.
inline constexpr int kSenderFpsSteps[] = {60, 50, 40, 30, 25, 20, 15, 12, 10, 8, 5};

// The largest listed rate at or below `fps` (and never below 5).
inline int snapFpsDown(double fps) {
    for (int f : kSenderFpsSteps)
        if (double(f) <= fps + 1e-9) return f;
    return 5;
}
// The next listed rate strictly above `fps`, or `fps` if none.
inline int nextFpsUp(int fps) {
    int up = fps;
    for (int f : kSenderFpsSteps)
        if (f > fps) up = f;
    return up;
}

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
//
// The anchors above are H.264 numbers, measured against H.264. HEVC
// buys roughly 40 % at equal perceived quality on this kind of content
// (more on flat screen regions, less on noisy camera), so its floors
// are 0.6x. This is not cosmetic: the floor is what the ladder uses to
// decide "this bitrate cannot carry this size, step down". Leaving the
// H.264 floors in place under HEVC would make the controller drop
// resolution while the picture was still perfectly sharp — the codec
// win would be spent on a smaller image instead of a better one.
inline constexpr double kHevcFloorScale = 0.6;

inline constexpr double floorBpp(Content c, VideoCodecKind codec) {
    const double h264 = c == Content::Screen ? 0.0650 : 0.0435;
    return codec == VideoCodecKind::H265 ? h264 * kHevcFloorScale : h264;
}
inline constexpr double comfortBpp(Content c, VideoCodecKind codec) {
    return floorBpp(c, codec) * 1.6;
}

// 16:9 is close enough — these bands are judgement, not measurement.
inline double pixelsAt(int longEdge) {
    return double(longEdge) * (double(longEdge) * 9.0 / 16.0);
}

// The least bitrate at which `longEdge`@`fps` is worth sending at all,
// in `codec`.
inline int minKbpsFor(Content c, int longEdge, int fps,
                      VideoCodecKind codec = VideoCodecKind::H264) {
    return int(floorBpp(c, codec) * pixelsAt(longEdge) * double(std::max(fps, 1))
               / 1000.0);
}
// The bitrate at which `longEdge`@`fps` is comfortable — the bar an
// upshift has to clear, repeatedly, before it happens.
inline int comfortKbpsFor(Content c, int longEdge, int fps,
                          VideoCodecKind codec = VideoCodecKind::H264) {
    return int(comfortBpp(c, codec) * pixelsAt(longEdge) * double(std::max(fps, 1))
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

// Screen: the FIRST step is resolution at full frame rate, then the
// two alternate, and the long edge is still defended — it never goes
// below 3/8 while any fps is left to give.
//
// This used to spend all the fps first (30 → 20 → 15) on the grounds
// that in-game text at 15 fps is readable and the same text at half the
// resolution is not. Two things changed that (2026-09-21):
//   * the fps rungs used to be cosmetic — the capture cadence ignored
//     them and only the encoder's expected rate moved — so "spend fps
//     first" never actually cost smoothness. Now that the capture and
//     push cadence follow fps(), it would;
//   * the owner's report was "really good quality, but very choppy",
//     and the stated preference is smooth over sharp.
// A 0.75 step is 1920 → 1440: text stays legible, the bitrate needed
// drops by ~44 %, and motion stays at the rate the user chose.
inline constexpr Rung kScreenLadder[] = {
    {1.000, 1.0},        {0.750, 1.0},  {0.750, 2.0 / 3.0},
    {0.625, 2.0 / 3.0},  {0.500, 0.5},  {0.375, 0.5}, {0.250, 0.5},
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
inline int rungMinKbps(Content c, int maxLongEdge, int maxFps, int idx,
                       VideoCodecKind codec = VideoCodecKind::H264) {
    return minKbpsFor(c, edgeForRung(c, maxLongEdge, idx),
                      fpsForRung(c, maxFps, idx), codec);
}

} // namespace videorate
