#pragma once

// What this machine has been PROVEN unable to decode, as opposed to
// what its capability probes claim.
//
// The probes (MacVTDecoder::hevcDecodeSupported / MFDecoder::
// hevcDecodeSupported) answer "is a backend registered", which is not
// the same question as "does a decoder session actually come up". A
// Windows client whose Media Foundation probe found an HEVC MFT that
// then refused to initialise advertised "h265" in its PeerCaps, both
// Mac senders flipped their screen shares to H.265 by payload type,
// and that client logged `no decoder available` for every access unit
// for the rest of the call — two black tiles, forever, with no path
// back because nothing downstream of the probe could contradict it.
//
// This is that contradiction: the first failed decoder init latches a
// process-wide fact, localCapsJson() stops advertising the codec from
// then on (this call and every later one in this process), and
// VoiceEngine re-announces the corrected caps to every connected peer
// so the senders re-select H.264 mid-call.
//
// Deliberately NOT a Qt object and deliberately process-wide: the
// decision is a property of the machine, not of a call, a peer or a
// stream, and it is latched from a decode worker thread and read from
// the main thread.
namespace videohealth {

// True once an H.265 decoder has failed to come up in this process —
// or in a previous run of THIS BUILD, if the hint was persisted.
bool h265DecodeBroken();

// Latch "H.265 decode is broken on this machine" and persist the hint.
//
// Returns true ONLY for the caller that flipped it. That edge is the
// rate limit on the caps re-announcement: a share sends 30 undecodable
// access units a second across two streams and any number of peers,
// and every one of them reaches this function, but exactly one of them
// is allowed to put a message on the wire.
bool markH265DecodeBroken();

// Forget the latch, in this process and on disk. Tests only — there is
// no product path that un-breaks a decoder mid-run, and the persisted
// hint clears itself on a version change (see the .cpp).
void resetForTest();

} // namespace videohealth
