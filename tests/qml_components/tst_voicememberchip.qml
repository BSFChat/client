import QtQuick
import QtTest
// The real module, out of bsfchat-lib's compiled-in resources.
import BSFChat
// The shipped rules, by the same qrc path src/main.cpp loads from. Importing
// by URL rather than by relative source path also proves the .js is actually
// IN the module's resources — a .js imported by a component but missing from
// CMakeLists.txt loads fine from disk in a replica test and fails at runtime
// on a user's machine, which is how MainSurface.js once shipped a crash.
import "qrc:/qt/qml/BSFChat/qml/js/VoiceLevels.js" as VoiceLevels

// VoiceMemberChip.qml — the chip in VoiceRoom's bottom member strip, the one
// on screen while somebody is sharing their screen. Instantiated, driven, and
// measured.
//
// WHAT THIS TEST IS FOR
//
// The chip used to be an inline delegate in VoiceRoom.qml whose ring read
// `modelData.speaking === true`. Nothing has ever written a `speaking` key —
// not ServerConnection::buildVoiceMembers(), not the protocol — so the
// expression was `undefined === true` and the ring never lit for anybody, ever.
// It was found by reading the code; no test could have caught it, because no
// test could instantiate the component, and a text-scrape for "speaking" in
// tests/test_qml_hygiene.cpp would have passed happily over the broken
// binding. That is the exact failure shape the bsfchat-lib split was for.
//
// So the cases here are deliberately the ones no rules test can state:
//
//   * that the ring follows the LIVE level — micLevel for us, peerLevel() for
//     everyone else — and not a row key;
//   * that a row asserting `speaking: true` with no audio behind it lights
//     nothing, which is the dead binding stated as a test;
//   * that the remote path re-evaluates on peerLevelChanged. peerLevel is an
//     invokable, so a binding that calls it without depending on the signal
//     freezes on its first answer and looks correct in a screenshot;
//   * that the signal is matched by user id, so one peer talking does not ring
//     the whole strip;
//   * that a peer we have no media connection to — the roster rows for a
//     channel we are not in — rings nothing rather than defaulting to on.
//
// The environment (AppSettings singleton, serverManager context property) is
// stubbed in tests/qml_components_test_main.cpp. Theme.qml, VoiceLevels.js and
// VoiceMemberChip.qml are all the shipped files.
TestCase {
    id: tc
    name: "VoiceMemberChipReal"
    when: windowShown
    width: 200
    height: 120
    visible: true

    readonly property string selfId: "@me:example.org"
    readonly property string peerId: "@them:example.org"

    VoiceMemberChip {
        id: chip
        width: 56
        height: 88
    }

    // ── helpers ──────────────────────────────────────────────────────

    function descendants(item) {
        var out = [];
        if (!item || item.children === undefined)
            return out;
        for (var i = 0; i < item.children.length; ++i) {
            var c = item.children[i];
            out.push(c);
            out = out.concat(descendants(c));
        }
        return out;
    }

    function byName(name) {
        var d = descendants(chip);
        for (var i = 0; i < d.length; ++i)
            if (d[i].objectName === name)
                return d[i];
        return null;
    }

    function ring() { return byName("speakingRing"); }
    function glyph() { return byName("statusGlyph"); }

    function conn() { return serverManager.activeServer; }

    // A row of the shape ServerConnection::buildVoiceMembers() actually
    // produces: user_id, displayName, the media flags, peerState,
    // connectionPath — and NO speaking key, because there isn't one.
    function row(userId, muted, deafened) {
        return {
            user_id: userId,
            displayName: userId === tc.selfId ? "Me" : "Them",
            muted: muted === true,
            deafened: deafened === true,
            screenSharing: false,
            cameraOn: false,
            peerState: "connected",
            connectionPath: "direct"
        };
    }

    function init() {
        serverManager.resetForTest();
        chip.member = row(tc.peerId, false, false);
        tryCompare(ring(), "visible", false);
    }

    // ── 1. the shipped component loads out of the real module ────────

    function test_a_the_real_component_loaded() {
        verify(chip !== null);
        verify(chip.toString().indexOf("VoiceMemberChip") >= 0,
               "expected a VoiceMemberChip instance, got " + chip);
        // The ring and the glyph are the two things every case below finds by
        // name. Say so once, here, rather than letting a rename surface as a
        // dozen null-dereferences.
        verify(ring() !== null, "no item named speakingRing in the chip");
        verify(glyph() !== null, "no item named statusGlyph in the chip");
        // The .js resolved from the module's resources, not from disk.
        verify(VoiceLevels.MIC_FLOOR > 0);
        verify(VoiceLevels.PEER_FLOOR > 0);
    }

    // ── 2. the dead binding, stated as a test ────────────────────────

    // A row that claims to be speaking, with no audio anywhere: nothing lights.
    // If somebody re-adds a `speaking` key to voiceMembers and wires the ring
    // back to it, this fails — which is the intent. The key would be carried by
    // the 5-second voiceMembers poll, and a speaking indicator quantised to 5
    // seconds is worse than no indicator at all.
    function test_b_a_speaking_key_on_the_row_lights_nothing() {
        var r = row(tc.peerId, false, false);
        r.speaking = true;
        chip.member = r;
        // Give the binding every chance to be wrong.
        wait(50);
        compare(chip.speaking, false);
        compare(ring().visible, false);
    }

    // And the other half: no key on the row, real audio, ring on. Together
    // these two say the ring is driven by the level and by nothing else.
    function test_c_the_ring_follows_the_live_peer_level() {
        verify(chip.member.speaking === undefined);
        conn().setPeerLevel(tc.peerId, VoiceLevels.PEER_FLOOR + 0.2);
        tryCompare(chip, "speaking", true);
        tryCompare(ring(), "visible", true);
        tryVerify(function() { return ring().opacity > 0.5; });

        // And back down. peerLevel decays to zero at playout when a peer stops
        // sending, so the ring going out has to be driven by the same path.
        conn().setPeerLevel(tc.peerId, 0);
        tryCompare(chip, "speaking", false);
        tryCompare(ring(), "visible", false);
    }

    // The invokable trap. A binding that calls peerLevel() without reading
    // _levelGen evaluates once and never again: the level can climb all it
    // likes and the ring stays dark. Two successive changes, so a chip that
    // happened to catch the first one by luck still fails.
    function test_d_the_remote_path_re_evaluates_on_the_signal() {
        conn().setPeerLevel(tc.peerId, VoiceLevels.PEER_FLOOR + 0.3);
        tryCompare(chip, "speaking", true);
        conn().setPeerLevel(tc.peerId, 0);
        tryCompare(chip, "speaking", false);
        conn().setPeerLevel(tc.peerId, VoiceLevels.PEER_FLOOR + 0.3);
        tryCompare(chip, "speaking", true);
    }

    // The signal carries a user id and the chip has to respect it. A handler
    // that bumped its counter on every peerLevelChanged would light the whole
    // strip whenever one person spoke — and would still pass every case above.
    function test_e_another_peer_talking_does_not_ring_this_one() {
        conn().setPeerLevel("@someone-else:example.org", 0.9);
        wait(50);
        compare(chip.speaking, false);
        compare(ring().visible, false);
    }

    // A peer we have no media connection to. This is the ordinary case for the
    // sidebar roster of a voice channel we are not in: peerLevel() answers 0,
    // and the absence of a level is the honest answer, not a default of "on".
    function test_f_a_peer_with_no_level_rings_nothing() {
        chip.member = row("@stranger:example.org", false, false);
        wait(50);
        compare(chip.level, 0);
        compare(chip.speaking, false);
        compare(ring().visible, false);
    }

    // ── 3. ourselves, which is a different level on a different scale ─

    function test_g_our_own_ring_follows_micLevel() {
        chip.member = row(tc.selfId, false, false);
        tryCompare(chip, "isSelf", true);
        compare(chip.speaking, false);

        conn().micLevel = VoiceLevels.MIC_FLOOR + 0.1;
        tryCompare(chip, "speaking", true);
        tryCompare(ring(), "visible", true);

        conn().micLevel = 0;
        tryCompare(chip, "speaking", false);
    }

    // Our own level must not be read through peerLevel(), and a peer's must
    // not be read through micLevel. Crossing the two is an easy mistake to
    // make and invisible in a call where you are the only one talking.
    function test_h_the_two_levels_do_not_cross() {
        chip.member = row(tc.selfId, false, false);
        tryCompare(chip, "isSelf", true);
        // A loud entry under OUR id in the peer table. We are not in it in
        // production — we do not decode ourselves — so this must do nothing.
        conn().setPeerLevel(tc.selfId, 0.9);
        wait(50);
        compare(chip.speaking, false);

        // And the reverse: a hot mic does not ring a remote peer's chip.
        chip.member = row(tc.peerId, false, false);
        tryCompare(chip, "isSelf", false);
        conn().micLevel = 0.9;
        wait(50);
        compare(chip.speaking, false);
    }

    // ── 4. mute and deafen, which are not symmetrical ────────────────

    // micLevel is taken from the raw capture frame BEFORE the mute check that
    // decides whether to encode, so our own level rises normally while muted.
    // A ring on that is the app telling us we are talking to people who cannot
    // hear us.
    function test_i_muted_does_not_ring_however_loud_we_are() {
        chip.member = row(tc.selfId, true, false);
        conn().micLevel = 0.9;
        wait(50);
        compare(chip.speaking, false);
        compare(ring().visible, false);
        // The glyph is the honest feedback here, and it is on.
        compare(glyph().visible, true);
    }

    function test_j_a_muted_peer_does_not_ring_either() {
        chip.member = row(tc.peerId, true, false);
        conn().setPeerLevel(tc.peerId, 0.9);
        wait(50);
        compare(chip.speaking, false);
        compare(ring().visible, false);
    }

    // Deafening stops PLAYBACK only — AudioWorker drops the mixed output and
    // leaves capture running — so a deafened person is still perfectly audible
    // to everybody else. Their ring must stay on. This is the case that keeps
    // the plausible "deafened means silent too" tidy-up from landing.
    function test_k_a_deafened_peer_still_rings_because_we_can_hear_them() {
        chip.member = row(tc.peerId, false, true);
        conn().setPeerLevel(tc.peerId, VoiceLevels.PEER_FLOOR + 0.3);
        tryCompare(chip, "speaking", true);
        tryCompare(ring(), "visible", true);
        // And the glyph says deafened, not muted.
        compare(glyph().visible, true);
    }

    // ── 5. the floors are the shared ones, on the right scale ────────

    // Straddling each floor on the chip itself, not on the .js. A component
    // that inlined its own number would pass tst_voicelevels.qml and fail here.
    function test_l_the_chip_uses_the_shared_floors() {
        chip.member = row(tc.peerId, false, false);
        conn().setPeerLevel(tc.peerId, VoiceLevels.PEER_FLOOR - 0.005);
        wait(50);
        compare(chip.speaking, false);
        conn().setPeerLevel(tc.peerId, VoiceLevels.PEER_FLOOR + 0.005);
        tryCompare(chip, "speaking", true);

        chip.member = row(tc.selfId, false, false);
        conn().micLevel = VoiceLevels.MIC_FLOOR - 0.005;
        wait(50);
        compare(chip.speaking, false);
        conn().micLevel = VoiceLevels.MIC_FLOOR + 0.005;
        tryCompare(chip, "speaking", true);
    }

    // ── 6. survivable edges ──────────────────────────────────────────

    // Losing the server. The strip is inside VoiceRoom, which main.qml keeps
    // mounted, so a null activeServer reaches these chips in the ordinary
    // course of switching servers.
    function test_m_no_active_server_is_survivable() {
        conn().setPeerLevel(tc.peerId, 0.9);
        tryCompare(chip, "speaking", true);
        serverManager.activeServer = null;
        tryCompare(chip, "level", 0);
        tryCompare(chip, "speaking", false);
        tryCompare(ring(), "visible", false);
    }

    // A ListView hands its delegate an undefined modelData during teardown.
    function test_n_a_member_of_nothing_is_survivable() {
        chip.member = undefined;
        wait(50);
        compare(chip.userId, "");
        compare(chip.speaking, false);
        compare(ring().visible, false);
        // Still renders an initial, rather than an empty box or a crash.
        compare(chip.dispName, "?");
    }

    // ── 7. what the chip says out loud ───────────────────────────────

    // Read through chip.Accessible.name rather than tryCompare(chip,
    // "Accessible.name", ...): tryCompare resolves a property by NAME on the
    // object and cannot walk into a grouped attached property, so the
    // tryCompare form silently reads `undefined` and compares that instead.
    function expectName(expected) {
        tryVerify(function() { return chip.Accessible.name === expected; },
                  1000, "accessible name was '" + chip.Accessible.name
                        + "', expected '" + expected + "'");
    }

    function test_o_the_chip_names_itself_for_a_screen_reader() {
        chip.member = row(tc.peerId, false, false);
        expectName("Them");

        chip.member = row(tc.selfId, false, false);
        expectName("Me, you");

        chip.member = row(tc.peerId, true, false);
        expectName("Them, muted");

        // Deafened wins over muted — it is the stronger statement and saying
        // both is noise.
        chip.member = row(tc.peerId, true, true);
        expectName("Them, deafened");
    }

    // Deliberate: the ring is driven by a raw level with no hold, so it
    // flickers off in the gaps between words. A ring the eye integrates is
    // fine; a name a screen reader re-announces every time it changes is not.
    // Releasing speech needs a hold longer than an inter-word gap — which is
    // what MemberListModel::kSpeakingHoldMs is for. Until that is reachable
    // here, the name deliberately leaves speech out, and this case is why.
    function test_p_the_name_does_not_stutter_while_somebody_talks() {
        chip.member = row(tc.peerId, false, false);
        conn().setPeerLevel(tc.peerId, 0.9);
        tryCompare(chip, "speaking", true);
        compare(chip.Accessible.name, "Them");
        verify(chip.Accessible.name.indexOf("speaking") < 0);
    }
}
