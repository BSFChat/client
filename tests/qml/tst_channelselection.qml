import QtQuick
import QtTest
import "../../qml/js/ChannelSelection.js" as ChannelSelection

// Which channel-sidebar row is highlighted.
//
// This exercises qml/js/ChannelSelection.js — the file ChannelList.qml
// imports — not a copy of its rules. Same constraint as
// tst_videostage.qml and tst_videowindows.qml: ChannelList.qml imports
// the BSFChat module, which is compiled into the app binary and cannot
// be loaded from a test executable, so the delegate half (the colours,
// the accent stripe) is out of reach here and the PREDICATE is what is
// pinned.
//
// The owner's report, verbatim: "When we join a voice chat and have it
// focussed, the text chat we were previously in is no longer
// highlighted (it currently is)."
TestCase {
    id: tc
    name: "ChannelSelection"

    // A ServerConnection snapshot. The real one is a QObject with these
    // three Q_PROPERTYs; nothing in ChannelSelection.js needs more.
    function view(text, voice, viewingVoice) {
        return {
            activeRoomId: text,
            activeVoiceRoomId: voice,
            viewingVoiceRoom: viewingVoice
        };
    }

    function textRow(id)  { return { roomId: id, isVoice: false }; }
    function voiceRow(id) { return { roomId: id, isVoice: true }; }

    // ── 1. Reading text, not in voice ────────────────────────────────

    function test_text_row_selected_when_reading_text() {
        var v = view("!general:x", "", false);
        verify(ChannelSelection.textRowSelected("!general:x", v));
        verify(!ChannelSelection.textRowSelected("!random:x", v));
        compare(ChannelSelection.selectedRoomId(v), "!general:x");
    }

    // ── 2. The bug: in voice AND looking at it ───────────────────────
    //
    // activeRoomId still points at the text channel — that is deliberate,
    // it is how leaving voice returns you to what you were reading — so
    // the ONLY thing that can tell the two rows apart is
    // viewingVoiceRoom.

    function test_voice_focused_drops_the_text_highlight() {
        var v = view("!general:x", "!lounge:x", true);
        verify(!ChannelSelection.textRowSelected("!general:x", v),
               "the text channel must NOT stay highlighted");
        verify(ChannelSelection.voiceRowSelected("!lounge:x", v));
        compare(ChannelSelection.selectedRoomId(v), "!lounge:x");
    }

    // ── 3. In voice but reading text ─────────────────────────────────
    //
    // The orthogonal case, and the reason the fix is not simply "voice
    // wins when connected": you can be in a call and reading a channel.

    function test_in_voice_but_reading_text() {
        var v = view("!general:x", "!lounge:x", false);
        verify(ChannelSelection.textRowSelected("!general:x", v));
        verify(!ChannelSelection.voiceRowSelected("!lounge:x", v),
               "a connected-but-unviewed voice room is not the selected row");
        compare(ChannelSelection.selectedRoomId(v), "!general:x");
    }

    // ── 4. Switching back restores the text highlight ────────────────

    function test_switch_back_restores_text() {
        var inVoice  = view("!general:x", "!lounge:x", true);
        var backToText = view("!general:x", "!lounge:x", false);
        verify(!ChannelSelection.rowSelected(textRow("!general:x"), inVoice));
        verify(ChannelSelection.rowSelected(voiceRow("!lounge:x"), inVoice));
        verify(ChannelSelection.rowSelected(textRow("!general:x"), backToText));
        verify(!ChannelSelection.rowSelected(voiceRow("!lounge:x"), backToText));
    }

    // ── 5. Never two rows at once ────────────────────────────────────
    //
    // The invariant the owner actually reported the absence of. Swept
    // over every combination of the four states a sidebar can be in.

    function test_at_most_one_row_is_ever_selected_data() {
        return [
            { tag: "text only",        v: view("!a:x", "",     false) },
            { tag: "text + voice idle",v: view("!a:x", "!b:x", false) },
            { tag: "voice focused",    v: view("!a:x", "!b:x", true)  },
            { tag: "voice, no text",   v: view("",     "!b:x", true)  },
            { tag: "nothing open",     v: view("",     "",     false) }
        ];
    }

    function test_at_most_one_row_is_ever_selected(data) {
        var rows = [textRow("!a:x"), voiceRow("!b:x"),
                    textRow("!c:x"), voiceRow("!d:x")];
        var n = 0;
        for (var i = 0; i < rows.length; i++)
            if (ChannelSelection.rowSelected(rows[i], data.v)) n++;
        verify(n <= 1, data.tag + ": " + n + " rows highlighted");
    }

    // ── 6. Degenerate inputs ─────────────────────────────────────────
    //
    // No server connected, an unsynced room with an empty id, and a
    // viewingVoiceRoom that arrives undefined rather than false — all of
    // which reach these functions in practice, none of which may light a
    // row up.

    function test_no_server_selects_nothing() {
        verify(!ChannelSelection.textRowSelected("!a:x", null));
        verify(!ChannelSelection.voiceRowSelected("!b:x", null));
        verify(!ChannelSelection.rowSelected(textRow("!a:x"), undefined));
        compare(ChannelSelection.selectedRoomId(null), "");
    }

    function test_empty_room_id_never_matches() {
        verify(!ChannelSelection.textRowSelected("", view("", "", false)),
               "an empty id must not match an empty activeRoomId");
        verify(!ChannelSelection.voiceRowSelected("", view("", "", true)));
        verify(!ChannelSelection.rowSelected(null, view("!a:x", "", false)));
    }

    function test_undefined_viewing_flag_reads_as_text() {
        var v = { activeRoomId: "!a:x", activeVoiceRoomId: "!b:x" };
        verify(ChannelSelection.textRowSelected("!a:x", v));
        verify(!ChannelSelection.voiceRowSelected("!b:x", v));
        verify(!ChannelSelection.viewingVoice(v));
    }
}
