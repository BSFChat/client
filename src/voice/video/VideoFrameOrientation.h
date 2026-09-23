#pragma once

// Which way is up on the wire.
//
// A phone camera's sensor has ONE orientation and it is not the UI's. On
// iOS the back camera's native buffer is landscape-right whatever the
// user is holding; on Android it is landscape with a per-device sensor
// offset. Qt hides this for DISPLAY only: the capture backend stamps a
// presentation rotation (and, for a front/selfie camera, a mirror flag)
// onto every QVideoFrame, and QVideoSink/VideoOutput applies both when
// it draws. The PIXELS are untouched.
//
// Everything in this client that reads a QVideoFrame for DISPLAY
// therefore gets it right for free, and everything that reads the pixels
// gets it wrong: FrameConverter::toI420 feeds the encoder, and the
// legacy JPEG path calls QVideoFrame::toImage(). Neither consults the
// metadata, so a phone in portrait sends a 90°-rotated picture and the
// far end — which receives an H.264 bitstream with no orientation
// metadata anywhere in it — has no way to recover.
//
// There are three places this could be fixed and only one right one:
//
//   * At capture, by asking AVFoundation to rotate. It can, but it costs
//     a real rotation in the capture pipeline on every frame whether or
//     not anyone is watching, and Qt does not expose the control.
//   * At the far end, by sending the angle in a side channel. That is
//     what RTP's CVO header extension (urn:3gpp:video-orientation) does,
//     and it is the "proper" answer — but it needs every receiver to
//     implement it, and this mesh has desktop peers on three platforms
//     plus a legacy JPEG path. A rotation the old peers ignore is a
//     rotation that does not happen.
//   * At the sender, by rotating the pixels before encode. One place,
//     every receiver benefits including the JPEG ones, and the cost is
//     a libyuv plane rotation on an already-downscaled frame.
//
// The third. This header is the single source of truth for the decision
// so the encode path and the JPEG path cannot disagree, and so the
// mirroring rule below is written down once rather than rediscovered.

namespace videoorient {

// Normalise any angle — negative, over-wound, or off-axis — to one of
// {0, 90, 180, 270}. Off-axis angles round to the nearest quadrant
// because the only thing downstream can do with 37° is refuse the
// frame, and a slightly-wrong upright picture beats no picture.
constexpr int normalise(int deg) {
    int d = deg % 360;
    if (d < 0) d += 360;
    // Round to nearest quadrant, ties away from zero: 45 -> 90.
    return ((d + 45) / 90 % 4) * 90;
}

// Degrees of CLOCKWISE rotation to apply to the captured PIXELS so the
// encoded frame arrives upright at the far end.
//
// This is the same angle the presentation metadata carries, not its
// inverse: Qt's rotation says "turn this clockwise by N to show it the
// right way up", and baking the picture the right way up means doing
// exactly that turn.
constexpr int wireRotation(int presentationRotationDeg) {
    return normalise(presentationRotationDeg);
}

// True when applying `deg` swaps width and height.
constexpr bool swapsAxes(int deg) {
    const int d = normalise(deg);
    return d == 90 || d == 270;
}

// Whether to mirror the picture horizontally for the wire.
//
// ALWAYS FALSE, and the argument is deliberately ignored rather than
// absent, so that a reader who has the mirror flag in hand finds the
// answer here instead of assuming it was overlooked.
//
// A front camera is mirrored for the LOCAL preview because people
// expect their own image to behave like a bathroom mirror — raise your
// left hand, the left side of the picture moves. That is a property of
// looking at yourself, not a property of the picture. The far end is
// looking at you, not at themselves, and a mirrored feed shows them
// your lanyard and your T-shirt print backwards. Every video caller
// (FaceTime, WebRTC's own convention) sends un-mirrored and previews
// mirrored, and Qt already gives us both for free: the preview goes
// through QVideoSink, which applies the flag, and the encoder gets the
// raw pixels, which never had it applied.
//
// So the wire wants the pixels exactly as captured. Doing nothing is
// the correct action; this function exists to say so out loud.
constexpr bool mirrorForWire(bool presentationMirrored) {
    (void)presentationMirrored;
    return false;
}

} // namespace videoorient
