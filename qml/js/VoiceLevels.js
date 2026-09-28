.pragma library

// When an audio level counts as "this person is talking".
//
// Every speaking ring in the app is driven by one of exactly two numbers
// out of ServerConnection:
//
//     activeServer.micLevel            — ourselves
//     activeServer.peerLevel(userId)   — a remote peer
//
// The level has one source of truth. The PREDICATE over it did not: the
// same question was asked with `level > 0.04` in ParticipantTile.qml and
// `micLevel > 0.05` in VoiceParticipantList.qml, and a third time in C++
// as MemberListModel::kSpeakingLevelFloor. This file is that predicate,
// once.
//
// THE TWO FLOORS ARE NOT A DUPLICATION — THEY ARE TWO SCALES.
//
// This is the part that makes "unify on one constant" the wrong fix, and
// it is worth stating plainly because the two numbers look
// interchangeable and are not:
//
//   * micLevel is PERCEPTUAL. AudioWorker::processCaptureFrame() takes the
//     frame's RMS, converts to dBFS and maps -60 dBFS → 0, -10 dBFS → 1,
//     linear in dB between. So 0.05 on this scale is about -57 dBFS —
//     deliberately very sensitive, because a ring that fails to light
//     when you speak reads as a broken microphone. AudioWorker.cpp
//     documents 0.05 as the design point in so many words.
//
//   * peerLevel is LINEAR. AudioWorker::renderMixedFrame() takes the peak
//     sample magnitude of the decoded frame over 32768, EWMA-smoothed —
//     no dB mapping at all. 0.04 on this scale is roughly -28 dBFS, some
//     30 dB louder than the mic floor.
//
// Feeding one number to both would therefore change what "speaking"
// means for one of them by about a factor of thirty. So: two floors, each
// named after the scale it belongs to, and a caller that says which one
// it is asking about rather than picking a number.
//
// Pure functions over plain numbers, for the reason ChannelSelection.js
// gives: the property reads stay in QML so the bindings capture their own
// dependencies, and this file only ever sees values a test can hand it.

// Perceptual, dB-mapped: the floor our OWN mic level has to clear.
// Matches the value AudioWorker.cpp documents as the point the ring
// lights, and the one MemberListModel uses.
var MIC_FLOOR = 0.05;

// Linear peak magnitude: the floor a REMOTE peer's decoded level has to
// clear. The value ParticipantTile has always shipped for remote tiles.
var PEER_FLOOR = 0.04;

// Guards a level that arrived as undefined — peerLevel() answers 0 for an
// unknown peer, but a member row can be built before any connection
// exists at all, and NaN > x is false in a way that silently reads as
// "silent" rather than as a fault.
function _num(level) {
    var n = Number(level);
    return isFinite(n) ? n : 0;
}

// Our own mic. `true` only above the perceptual floor.
function micSpeaking(level) {
    return _num(level) > MIC_FLOOR;
}

// A remote peer's decoded audio. `true` only above the linear floor.
function peerSpeaking(level) {
    return _num(level) > PEER_FLOOR;
}

// The one call a member row makes. Anything other than an explicit `true`
// for isSelf is treated as a remote peer, so a row whose isSelf binding has
// not resolved yet is held to the stricter floor rather than the looser one.
//
// MUTE IS PART OF THE PREDICATE, not a separate check at each call site,
// because it is what the ring MEANS: not "this person's microphone is
// registering sound" but "this person is being heard". Those come apart for
// ourselves in particular — AudioWorker emits micLevelChanged from the raw
// capture frame BEFORE the `if (!muted)` that decides whether to encode and
// send it, so our own level rises normally while we are muted. A ring on
// that is the app telling us we are talking to people who cannot hear us,
// which is the one thing a speaking indicator must never do.
//
// DEAFENED IS DELIBERATELY NOT HERE. Deafening stops playback only —
// AudioWorker's deafen branch drops the mixed output and leaves capture
// running — so a deafened person is still perfectly audible to everybody
// else, and hiding their ring would be a lie in the other direction.
function speaking(level, isSelf, muted) {
    if (muted === true) return false;
    return isSelf === true ? micSpeaking(level) : peerSpeaking(level);
}

// The floor that applies to a given row, for the callers that scale a ring
// by how far the level is above it rather than only switching it on.
function floorFor(isSelf) {
    return isSelf === true ? MIC_FLOOR : PEER_FLOOR;
}
