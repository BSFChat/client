import QtQuick
import QtTest
import "../../qml/js/VoiceLevels.js" as VoiceLevels

// When an audio level counts as somebody talking — qml/js/VoiceLevels.js, the
// file ParticipantTile.qml, VoiceMemberChip.qml and VoiceParticipantList.qml
// all import.
//
// The rule used to be written three times with three different numbers
// (`level > 0.04`, `micLevel > 0.05`, and MemberListModel's 0.05 in C++), and
// the two floors that survived are NOT a tidied-up duplicate: micLevel is
// perceptual, dB-mapped with a -60 dBFS floor, and peerLevel is linear peak
// magnitude. The cases below pin that distinction, because collapsing it back
// to one constant is the obvious "cleanup" for the next person to try and it
// would move one of the two thresholds by about 30 dB.
//
// The QML half — that the components read THIS file, and that they read the
// live level rather than a key nobody writes — is in
// tests/qml_components/tst_voicememberchip.qml, on the shipped component.
TestCase {
    id: tc
    name: "VoiceLevels"

    // ── the two floors are different, and that is the point ──────────

    function test_the_two_scales_have_different_floors() {
        // If these are ever made equal, one of the two levels has changed
        // scale and the comment block in VoiceLevels.js is now a lie.
        verify(VoiceLevels.MIC_FLOOR !== VoiceLevels.PEER_FLOOR);
        // Both have to stay inside the 0..1 range both levels are normalised
        // to, and above zero — a floor of 0 lights the ring on room tone.
        verify(VoiceLevels.MIC_FLOOR > 0 && VoiceLevels.MIC_FLOOR < 1);
        verify(VoiceLevels.PEER_FLOOR > 0 && VoiceLevels.PEER_FLOOR < 1);
    }

    function test_floorFor_picks_the_scale_not_the_smaller_number() {
        compare(VoiceLevels.floorFor(true), VoiceLevels.MIC_FLOOR);
        compare(VoiceLevels.floorFor(false), VoiceLevels.PEER_FLOOR);
        // Not "isSelf-ish": anything but an explicit true is a remote peer.
        compare(VoiceLevels.floorFor(undefined), VoiceLevels.PEER_FLOOR);
        compare(VoiceLevels.floorFor(null), VoiceLevels.PEER_FLOOR);
        compare(VoiceLevels.floorFor(1), VoiceLevels.PEER_FLOOR);
    }

    // ── the predicate on each scale ──────────────────────────────────

    function test_a_level_on_the_floor_is_not_speaking() {
        // Strictly above, on both scales. A level exactly at the floor is the
        // silence case: AudioWorker's EWMA parks a quiet room right about
        // there and a ring that latched on at equality would never go out.
        compare(VoiceLevels.micSpeaking(VoiceLevels.MIC_FLOOR), false);
        compare(VoiceLevels.peerSpeaking(VoiceLevels.PEER_FLOOR), false);
        verify(VoiceLevels.micSpeaking(VoiceLevels.MIC_FLOOR + 0.01));
        verify(VoiceLevels.peerSpeaking(VoiceLevels.PEER_FLOOR + 0.01));
    }

    function test_silence_is_not_speaking_on_either_scale() {
        compare(VoiceLevels.micSpeaking(0), false);
        compare(VoiceLevels.peerSpeaking(0), false);
        compare(VoiceLevels.speaking(0, true, false), false);
        compare(VoiceLevels.speaking(0, false, false), false);
    }

    function test_a_loud_level_is_speaking_on_either_scale() {
        verify(VoiceLevels.speaking(0.8, true, false));
        verify(VoiceLevels.speaking(0.8, false, false));
    }

    // The case that makes one shared constant wrong. A level that clears the
    // perceptual mic floor but not the linear peer floor must answer
    // differently depending on whose level it is.
    function test_the_same_number_means_different_things() {
        var between = (VoiceLevels.MIC_FLOOR + VoiceLevels.PEER_FLOOR) / 2;
        var lower = Math.min(VoiceLevels.MIC_FLOOR, VoiceLevels.PEER_FLOOR);
        var higher = Math.max(VoiceLevels.MIC_FLOOR, VoiceLevels.PEER_FLOOR);
        verify(between > lower && between < higher);
        // Above the lower floor, below the higher one: exactly one of the two
        // scales calls this speech.
        verify(VoiceLevels.micSpeaking(between)
               !== VoiceLevels.peerSpeaking(between));
    }

    // ── mute is part of the predicate ────────────────────────────────

    // The regression this exists for: micLevel is measured from the raw
    // capture frame BEFORE the mute check that decides whether to encode, so
    // our own level rises normally while we are muted. A ring on that tells
    // us we are talking to people who cannot hear us.
    function test_muted_is_never_speaking_however_loud() {
        compare(VoiceLevels.speaking(1.0, true, true), false);
        compare(VoiceLevels.speaking(1.0, false, true), false);
    }

    function test_not_muted_takes_anything_but_true_as_unmuted() {
        // A member row's `muted` comes off a JSON object, so undefined is the
        // ordinary case for a server that did not send the flag — and it must
        // read as "not muted", not as "muted".
        verify(VoiceLevels.speaking(0.8, true, undefined));
        verify(VoiceLevels.speaking(0.8, false, null));
        verify(VoiceLevels.speaking(0.8, false, false));
    }

    // Deafening stops PLAYBACK and leaves capture running, so a deafened
    // person is still audible to everybody else. There is deliberately no
    // deafened argument; this case exists so that adding one — the plausible
    // "surely deafened means silent too" change — has to come past a test
    // that says why not.
    function test_there_is_no_deafened_argument() {
        compare(VoiceLevels.speaking.length, 3);
    }

    // ── junk levels read as silence, not as speech ───────────────────

    function test_a_missing_level_is_silence() {
        compare(VoiceLevels.speaking(undefined, false, false), false);
        compare(VoiceLevels.speaking(null, true, false), false);
        compare(VoiceLevels.speaking(NaN, true, false), false);
        compare(VoiceLevels.speaking("", false, false), false);
        // Infinity is not "very loud", it is a fault. It reads as silence for
        // the same reason NaN does: a ring stuck permanently on is the worse
        // failure, because it accuses somebody of talking.
        compare(VoiceLevels.speaking(Infinity, false, false), false);
    }

    function test_a_numeric_string_is_still_a_level() {
        // QML hands a binding whatever the model row holds, and a JSON number
        // that arrived as a string is a real shape to survive.
        verify(VoiceLevels.speaking("0.8", false, false));
        compare(VoiceLevels.speaking("0.0", false, false), false);
    }

    function test_a_negative_level_is_silence() {
        compare(VoiceLevels.speaking(-1, true, false), false);
        compare(VoiceLevels.speaking(-1, false, false), false);
    }
}
