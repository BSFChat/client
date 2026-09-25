import QtQuick
import QtQuick.Layouts
import QtTest
import "../../qml/js/MainSurface.js" as MainSurface

// Which surface owns the mobile shell's main column.
//
// Two halves, proved two different ways, and the second one is the reason
// this file exists rather than just a handful of table cases:
//
//   1. WHICH page, for every state — qml/js/MainSurface.js, the file
//      MobileMain.qml imports and calls, so the rules exercised here are
//      literally the shipped ones.
//
//   2. WHY it has to be pages at all. Section 3 builds a REPLICA of the old
//      structure — a StackLayout whose child carries its own `visible:`
//      binding, plus a sibling reading that child's `.visible` — and
//      measures both ways it fails. Then it builds the new structure and
//      measures that neither is reachable. Without those cases the fix
//      reads like a refactor and the next person un-does it.
//
//      A replica, not MobileMain.qml itself: that component imports the
//      BSFChat module, which is compiled into the application binary and
//      cannot be loaded from a test executable (same constraint as
//      tst_channelselection.qml and tst_timelineoverlay.qml). So these
//      cases prove the Qt behaviour the structure relies on; the guard that
//      MobileMain.qml is still wired this way is
//      theMobileMainColumnHasExactlyOneWriter() in tests/test_qml_hygiene.cpp.
//
// The owner's report, from a Pixel 6 Pro screenshot on 2026-09-24: in voice
// channel #spikk with two participants, header reading "#penis / BSFChat",
// the words "No channel selected" composited across a live screen-share
// tile and a camera tile.
TestCase {
    id: tc
    name: "MainSurface"
    when: windowShown
    width: 300
    height: 200
    visible: true

    // A ServerConnection snapshot. The real one is a QObject with these two
    // Q_PROPERTYs among many; nothing in MainSurface.js needs more.
    function view(roomId, viewingVoice) {
        return { activeRoomId: roomId, viewingVoiceRoom: viewingVoice };
    }

    // ── 1. The page rules ────────────────────────────────────────────

    function test_no_server_shows_the_empty_state() {
        compare(MainSurface.mainPage(null), MainSurface.Empty);
        compare(MainSurface.mainPage(undefined), MainSurface.Empty);
        verify(MainSurface.showsEmptyState(null));
    }

    function test_server_but_no_channel_shows_the_empty_state() {
        compare(MainSurface.mainPage(view("", false)), MainSurface.Empty);
        // An id that never synced can arrive as undefined rather than "".
        compare(MainSurface.mainPage(view(undefined, false)), MainSurface.Empty);
    }

    function test_a_text_channel_shows_the_timeline() {
        compare(MainSurface.mainPage(view("!general:x", false)), MainSurface.Chat);
        verify(MainSurface.showsChat(view("!general:x", false)));
        verify(!MainSurface.showsEmptyState(view("!general:x", false)));
    }

    // The photographed case: a text room IS active, and the voice view is
    // up. Voice wins, and — the whole point — the empty state does not show.
    function test_voice_view_wins_over_an_active_text_channel() {
        var v = view("!penis:x", true);
        compare(MainSurface.mainPage(v), MainSurface.Voice);
        verify(MainSurface.showsVoice(v));
        verify(!MainSurface.showsEmptyState(v));
        verify(!MainSurface.showsChat(v));
    }

    // And with NO text room at all — join a voice channel on a fresh sign-in
    // without opening a channel first. This is the one the old code got most
    // wrong: no active room, so `!chatView.visible` was true for two separate
    // reasons at once.
    function test_voice_view_wins_with_no_text_channel_at_all() {
        var v = view("", true);
        compare(MainSurface.mainPage(v), MainSurface.Voice);
        verify(!MainSurface.showsEmptyState(v));
    }

    // `viewingVoiceRoom` is read the same way the old `... ? 1 : 0` swap read
    // it: only an explicit true counts.
    function test_only_an_explicit_true_is_the_voice_view() {
        compare(MainSurface.mainPage(view("!g:x", undefined)), MainSurface.Chat);
        compare(MainSurface.mainPage(view("!g:x", false)), MainSurface.Chat);
        compare(MainSurface.mainPage(view("!g:x", 0)), MainSurface.Chat);
    }

    // Exactly one surface, always. Belt and braces over the table above: a
    // future fourth page cannot make two of these true at once.
    function test_exactly_one_surface_for_every_state_data() {
        return [
            { tag: "nothing",        v: null },
            { tag: "no-channel",     v: view("", false) },
            { tag: "channel",        v: view("!g:x", false) },
            { tag: "voice",          v: view("", true) },
            { tag: "voice+channel",  v: view("!g:x", true) }
        ];
    }
    function test_exactly_one_surface_for_every_state(d) {
        var n = (MainSurface.showsChat(d.v) ? 1 : 0)
              + (MainSurface.showsVoice(d.v) ? 1 : 0)
              + (MainSurface.showsEmptyState(d.v) ? 1 : 0);
        compare(n, 1, "exactly one surface must own the column");
    }

    // ── 2. Why it has to be pages ────────────────────────────────────

    // The old structure, reproduced exactly: two pages, the first carrying
    // its own `visible:` binding, and a SIBLING overlay reading that page's
    // `.visible` the way the empty state read `!chatView.visible`.
    Component {
        id: oldShape
        Item {
            width: 200; height: 100
            property bool roomOpen: true
            property bool viewingVoice: false
            property alias chatPage: chatPage
            property alias voicePage: voicePage
            property alias emptyState: emptyState
            StackLayout {
                anchors.fill: parent
                currentIndex: viewingVoice ? 1 : 0
                Item { id: chatPage; visible: roomOpen }
                Item { id: voicePage }
            }
            Item { id: emptyState; anchors.fill: parent; visible: !chatPage.visible }
        }
    }

    // The new structure: three pages, not one of them carrying a `visible:`.
    Component {
        id: newShape
        Item {
            width: 200; height: 100
            property bool roomOpen: true
            property bool viewingVoice: false
            property alias chatPage: chatPage
            property alias voicePage: voicePage
            property alias emptyState: emptyState
            StackLayout {
                anchors.fill: parent
                currentIndex: MainSurface.mainPage({
                    activeRoomId: roomOpen ? "!g:x" : "",
                    viewingVoiceRoom: viewingVoice
                })
                Item { id: chatPage }
                Item { id: voicePage }
                Item { id: emptyState }
            }
        }
    }

    // The behaviour the whole fix rests on. If a future Qt stops writing
    // `visible` on StackLayout children, this case fails and tells you the
    // reasoning in MainSurface.js has expired.
    function test_a_stacklayout_writes_visible_on_its_children() {
        var s = createTemporaryObject(oldShape, tc);
        verify(s);
        compare(s.chatPage.visible, true);
        compare(s.voicePage.visible, false);
        s.viewingVoice = true;
        compare(s.voicePage.visible, true);
        compare(s.chatPage.visible, false,
                "StackLayout no longer hides the non-current page");
    }

    // FAILURE ONE — the screenshot. Flip to voice with a channel still
    // active and the sibling empty state turns itself on over the video.
    function test_the_old_shape_paints_the_empty_state_over_voice() {
        var s = createTemporaryObject(oldShape, tc);
        s.roomOpen = true;             // there IS a channel: #penis
        compare(s.emptyState.visible, false);
        s.viewingVoice = true;         // tap the voice channel: #spikk
        compare(s.voicePage.visible, true);
        compare(s.emptyState.visible, true,
                "the old shape no longer reproduces the reported bug; "
                + "if this stops failing, the replica has drifted from it");
    }

    // FAILURE TWO — the mirror image, and the reason a longer condition
    // would not have been enough. The layout's write does NOT kill the
    // child's declared binding, so a channel arriving while voice is up
    // re-fires it and turns the chat page back on UNDERNEATH nothing: both
    // pages are then visible and the timeline is over the video.
    function test_the_old_shape_can_show_two_pages_at_once() {
        var s = createTemporaryObject(oldShape, tc);
        s.roomOpen = false;
        s.viewingVoice = true;
        compare(s.chatPage.visible, false);
        s.roomOpen = true;             // reconnect restores the last text room
        compare(s.voicePage.visible, true);
        compare(s.chatPage.visible, true,
                "the old shape no longer double-shows; replica has drifted");
    }

    // And neither is reachable once they are all pages of the one layout.
    function test_the_new_shape_shows_exactly_one_page_data() {
        return [
            { tag: "channel",       room: true,  voice: false, page: MainSurface.Chat },
            { tag: "no-channel",    room: false, voice: false, page: MainSurface.Empty },
            { tag: "voice",         room: false, voice: true,  page: MainSurface.Voice },
            { tag: "voice+channel", room: true,  voice: true,  page: MainSurface.Voice }
        ];
    }
    function test_the_new_shape_shows_exactly_one_page(d) {
        var s = createTemporaryObject(newShape, tc);
        s.roomOpen = d.room;
        s.viewingVoice = d.voice;
        var vis = [s.chatPage.visible, s.voicePage.visible, s.emptyState.visible];
        var shown = 0;
        for (var i = 0; i < vis.length; ++i) if (vis[i]) ++shown;
        compare(shown, 1, "pages visible: " + vis);
        compare(vis[d.page], true);
    }

    // The specific transition from the report, end to end on the new shape:
    // reading #penis, join and open voice #spikk, and the empty state stays
    // dark the whole way.
    function test_the_reported_transition_never_shows_the_empty_state() {
        var s = createTemporaryObject(newShape, tc);
        s.roomOpen = true;
        verify(!s.emptyState.visible);
        s.viewingVoice = true;
        verify(!s.emptyState.visible);
        verify(s.voicePage.visible);
        verify(!s.chatPage.visible);
        // ...and a channel arriving mid-call does not punch the timeline
        // through either.
        s.roomOpen = false;
        s.roomOpen = true;
        verify(s.voicePage.visible);
        verify(!s.chatPage.visible);
        verify(!s.emptyState.visible);
    }
}
