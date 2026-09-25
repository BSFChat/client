import QtQuick
import QtTest
import "../../qml/js/VoiceRosterView.js" as VoiceRosterView

// Which roster the channel sidebar draws under a voice channel.
//
// The defect: the call header said "2 in call" and the drawer, the same
// minute, badged the same channel `1` and listed only the remote user. The
// local user was missing from the drawer. Two transports of the same
// server-side fact, and only one of them had it.
//
// This exercises qml/js/VoiceRosterView.js — the file ChannelList.qml
// imports, not a copy of its rules. What it cannot reach is the QML half:
// ChannelList.qml imports the BSFChat module, which is compiled into the app
// binary and cannot be loaded from a test executable (the constraint
// documented at the top of tst_videostage.qml and tst_channelselection.qml).
TestCase {
    id: tc
    name: "VoiceRosterView"

    function who(id) {
        return { user_id: id, displayName: id, muted: false, deafened: false,
                 cameraOn: false, screenSharing: false };
    }

    function ids(rows) {
        var out = [];
        for (var i = 0; i < rows.length; ++i) out.push(rows[i].user_id);
        return out;
    }

    // ── 1. The photographed case ─────────────────────────────────────

    // The drawer's own roster has lost the local user; the connected roster,
    // which is what the header counts, has both. In the room we are IN, the
    // header wins — and the badge and the names under it move together,
    // because they are the same vector.
    function test_the_room_we_are_in_shows_the_roster_the_header_counts() {
        var view = {
            roomId: "!streams:bsf",
            activeVoiceRoomId: "!streams:bsf",
            connectedRoster: [who("@josh:bsf"), who("@me:bsf")],
            foldedRoster: [who("@josh:bsf")]
        };
        compare(ids(VoiceRosterView.rosterFor(view)), ["@josh:bsf", "@me:bsf"]);
        // The number the badge draws. "2 in call" and a badge of 2.
        compare(VoiceRosterView.rosterCount(view), 2);
    }

    // ── 2. Every other channel is untouched ──────────────────────────
    //
    // The sync-folded roster is the ONLY answer for a channel we are not in,
    // and it is the reason the drawer can show "who is already in there"
    // before you join. Substituting the connected roster into some other
    // room's row would put the people in our call into a channel they are
    // not in, which is a far louder bug than the one being fixed.

    function test_another_channel_keeps_its_own_roster() {
        var view = {
            roomId: "!lounge:bsf",
            activeVoiceRoomId: "!streams:bsf",
            connectedRoster: [who("@josh:bsf"), who("@me:bsf")],
            foldedRoster: [who("@ana:bsf")]
        };
        compare(ids(VoiceRosterView.rosterFor(view)), ["@ana:bsf"]);
        compare(VoiceRosterView.rosterCount(view), 1);
    }

    function test_an_empty_channel_stays_empty_while_we_are_in_another() {
        var view = {
            roomId: "!lounge:bsf",
            activeVoiceRoomId: "!streams:bsf",
            connectedRoster: [who("@josh:bsf"), who("@me:bsf")],
            foldedRoster: []
        };
        compare(VoiceRosterView.rosterCount(view), 0);
    }

    function test_not_in_any_call_changes_nothing_anywhere() {
        var view = {
            roomId: "!streams:bsf",
            activeVoiceRoomId: "",
            connectedRoster: [],
            foldedRoster: [who("@josh:bsf")]
        };
        compare(ids(VoiceRosterView.rosterFor(view)), ["@josh:bsf"]);
    }

    // ── 3. The windows where the poll has nothing ────────────────────
    //
    // The connected roster is empty before the first poll lands and again
    // after teardown. Preferring it blindly would blank a roster the fold
    // had got right — the drawer would flicker to empty on join and on
    // leave, which is worse than the miscount being fixed.

    function test_an_empty_poll_does_not_blank_a_roster_we_have() {
        var view = {
            roomId: "!streams:bsf",
            activeVoiceRoomId: "!streams:bsf",
            connectedRoster: [],
            foldedRoster: [who("@josh:bsf")]
        };
        compare(ids(VoiceRosterView.rosterFor(view)), ["@josh:bsf"]);
    }

    // The join reply deliberately excludes the caller (the server sends
    // "other active voice members"), so for the first poll interval after
    // joining the connected roster is the others only. That is the header's
    // own behaviour and the drawer now simply matches it — the two agreeing
    // is the property under test, not the number.
    function test_the_drawer_matches_the_header_even_while_it_lags() {
        var view = {
            roomId: "!streams:bsf",
            activeVoiceRoomId: "!streams:bsf",
            connectedRoster: [who("@josh:bsf")],
            foldedRoster: [who("@josh:bsf"), who("@me:bsf")]
        };
        compare(VoiceRosterView.rosterCount(view), 1);
    }

    // ── 4. Nothing is ever invented ──────────────────────────────────
    //
    // This rule PICKS a source. It does not synthesise a participant, and in
    // particular it does not manufacture a self row when neither source has
    // one — that would paper over a genuine "our m.call.member never
    // propagated" fault, which matters because in that state remote users
    // cannot see us either and the drawer would be the only place it showed.

    function test_two_empty_sources_stay_empty() {
        compare(VoiceRosterView.rosterCount({
            roomId: "!streams:bsf",
            activeVoiceRoomId: "!streams:bsf",
            connectedRoster: [],
            foldedRoster: []
        }), 0);
    }

    function test_every_row_returned_came_from_a_real_source() {
        var view = {
            roomId: "!streams:bsf",
            activeVoiceRoomId: "!streams:bsf",
            connectedRoster: [who("@josh:bsf"), who("@me:bsf")],
            foldedRoster: [who("@josh:bsf")]
        };
        var out = VoiceRosterView.rosterFor(view);
        for (var i = 0; i < out.length; ++i) {
            var found = false;
            for (var j = 0; j < view.connectedRoster.length; ++j)
                if (view.connectedRoster[j].user_id === out[i].user_id) found = true;
            for (var k = 0; k < view.foldedRoster.length; ++k)
                if (view.foldedRoster[k].user_id === out[i].user_id) found = true;
            verify(found, out[i].user_id + " was invented by the view rule");
        }
    }

    // ── 5. The badge is sized from the list, never counted apart ─────

    function test_the_count_is_always_the_length_of_the_list() {
        var views = [
            { roomId: "!a:b", activeVoiceRoomId: "!a:b",
              connectedRoster: [who("@1:b"), who("@2:b")], foldedRoster: [who("@1:b")] },
            { roomId: "!a:b", activeVoiceRoomId: "!c:b",
              connectedRoster: [who("@1:b")], foldedRoster: [who("@9:b"), who("@8:b")] },
            { roomId: "!a:b", activeVoiceRoomId: "", connectedRoster: [], foldedRoster: [] }
        ];
        for (var i = 0; i < views.length; ++i)
            compare(VoiceRosterView.rosterCount(views[i]),
                    VoiceRosterView.rosterFor(views[i]).length);
    }

    // ── 6. Nothing crashes the sidebar ───────────────────────────────
    //
    // This runs inside a delegate that is built and rebuilt constantly, and a
    // throw here takes the whole channel list with it. Every field can be
    // absent mid-teardown.

    function test_missing_and_null_inputs() {
        compare(VoiceRosterView.rosterFor(null).length, 0);
        compare(VoiceRosterView.rosterFor(undefined).length, 0);
        compare(VoiceRosterView.rosterFor({}).length, 0);
        compare(VoiceRosterView.rosterFor({ roomId: "!a:b" }).length, 0);
        compare(VoiceRosterView.rosterCount({
            roomId: "!a:b", activeVoiceRoomId: "!a:b" }), 0);
        // A row with no roomId must never be mistaken for the active one,
        // however empty activeVoiceRoomId is.
        compare(VoiceRosterView.rosterFor({
            roomId: "", activeVoiceRoomId: "",
            connectedRoster: [who("@1:b")], foldedRoster: [] }).length, 0);
    }
}
