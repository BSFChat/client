import QtQuick
import QtQuick.Layouts
import QtTest
import "../../qml/js/ConnectionBanner.js" as Banner
import "../../qml/js/MainSurface.js" as MainSurface

// What the connection banner says, and — section 3 — why it has to be a row
// of the shell rather than a child of the timeline.
//
// The banner was `syncBanner`, the first row of the ColumnLayout inside
// qml/components/MessageView.qml. MessageView is page 0 of the main column's
// StackLayout on both shells, and a StackLayout writes `visible` on every
// child each time currentIndex changes — so flipping to the voice room took
// the banner off screen with the timeline. A user in a call whose session
// expired or whose connection dropped was told nothing at all, on the one
// surface where a silent disconnect is least explicable. Section 3 measures
// that against a real StackLayout, and measures that a row does not have the
// problem.
//
// A replica, not the shipped components: ConnectionBanner.qml, MessageView
// and both shells import the BSFChat module, which is compiled into the
// application binary and cannot be loaded from a test executable (same
// constraint as tst_mainsurface.qml). So these cases prove the Qt behaviour
// the structure relies on and the rules the component reads; the guard that
// the shells are still wired this way is theConnectionBannerIsAShellRow() in
// tests/test_qml_hygiene.cpp.
TestCase {
    id: tc
    name: "ConnectionBanner"
    when: windowShown
    width: 300
    height: 200
    visible: true

    // A ServerConnection snapshot. The real one is a QObject with these four
    // Q_PROPERTYs among many; nothing in ConnectionBanner.js needs more.
    function view(status, msg, needsReauth, busy) {
        return {
            connectionStatus: status,
            syncErrorMessage: msg,
            needsReauth: needsReauth,
            reauthInProgress: busy
        };
    }

    // ── 1. When the banner shows, and in what tone ───────────────────

    function test_a_healthy_connection_says_nothing() {
        var v = view(Banner.Connected, "", false, false);
        compare(Banner.tone(v), Banner.None);
        verify(!Banner.isShowing(v));
        compare(Banner.message(v), "");
    }

    // No server selected at all — the shells mount the banner unconditionally,
    // so it has to answer for that state rather than rely on a guard outside.
    function test_no_server_says_nothing() {
        verify(!Banner.isShowing(null));
        verify(!Banner.isShowing(undefined));
        compare(Banner.message(null), "");
        verify(!Banner.showsReauth(null));
    }

    function test_reconnecting_is_the_amber_one() {
        var v = view(Banner.Reconnecting, "", false, false);
        compare(Banner.tone(v), Banner.Warn);
        verify(Banner.isShowing(v));
        compare(Banner.message(v), "Reconnecting to server…");
    }

    // Red, not amber. Disconnected and expired are both states the user has
    // to do something about; amber is the one that clears itself.
    function test_disconnected_and_expired_are_both_red_data() {
        return [
            { tag: "disconnected", s: Banner.Disconnected },
            { tag: "expired",      s: Banner.Expired }
        ];
    }
    function test_disconnected_and_expired_are_both_red(d) {
        var v = view(d.s, "Your session expired.", false, false);
        compare(Banner.tone(v), Banner.Danger);
        verify(Banner.isShowing(v));
        verify(Banner.message(v).length > 0);
    }

    // The old rule was `connectionStatus !== 1`, which meant any value the
    // C++ side might grow produced a 28px strip in `color: "transparent"`
    // with no text in it — a silent gap above the timeline that reads as a
    // rendering glitch. Only the three states with something to say show.
    function test_an_unknown_status_shows_nothing_rather_than_an_empty_strip_data() {
        return [
            { tag: "4",         s: 4 },
            { tag: "-1",        s: -1 },
            { tag: "undefined", s: undefined },
            { tag: "string",    s: "reconnecting" }
        ];
    }
    function test_an_unknown_status_shows_nothing_rather_than_an_empty_strip(d) {
        var v = view(d.s, "", false, false);
        compare(Banner.tone(v), Banner.None);
        verify(!Banner.isShowing(v));
    }

    // ── 2. The way out ───────────────────────────────────────────────

    // The expired banner carries the server's own words when it gave any...
    function test_the_expired_banner_prefers_the_servers_own_words() {
        var v = view(Banner.Expired, "Signed out on another device.", true, false);
        compare(Banner.message(v), "Signed out on another device.");
    }

    // ...and says something when it did not. The 401 paths in
    // ServerConnection set status 3 before every one of them has a message,
    // and a red strip with nothing written in it next to a "Sign in again"
    // button reads as a bug in the app, not as an expired session.
    function test_the_expired_banner_is_never_wordless_data() {
        return [
            { tag: "empty",     m: "" },
            { tag: "undefined", m: undefined },
            { tag: "null",      m: null }
        ];
    }
    function test_the_expired_banner_is_never_wordless(d) {
        var v = view(Banner.Expired, d.m, true, false);
        verify(Banner.message(v).length > 0);
        verify(Banner.message(v).toLowerCase().indexOf("sign in again") >= 0);
    }

    // The button is offered exactly when a login round trip is the thing to
    // do — which is an auth question, not a connection-status one.
    function test_the_reauth_button_follows_needsreauth() {
        verify(Banner.showsReauth(view(Banner.Expired, "x", true, false)));
        verify(!Banner.showsReauth(view(Banner.Expired, "x", false, false)));
        // A dropped socket is not something a login fixes.
        verify(!Banner.showsReauth(view(Banner.Disconnected, "", false, false)));
        verify(!Banner.showsReauth(view(Banner.Reconnecting, "", false, false)));
    }

    // Disabled, not hidden, while the round trip is in flight: the button is
    // the only affordance in the strip, and taking it away mid-sign-in would
    // read as the click having destroyed it.
    function test_a_sign_in_in_flight_disables_the_button_without_hiding_it() {
        var v = view(Banner.Expired, "x", true, true);
        verify(Banner.showsReauth(v), "the button must stay on screen");
        verify(!Banner.reauthEnabled(v));
        compare(Banner.reauthLabel(v), "Signing in…");

        var idle = view(Banner.Expired, "x", true, false);
        verify(Banner.reauthEnabled(idle));
        compare(Banner.reauthLabel(idle), "Sign in again");
    }

    // ── 3. Why it has to be a row of the shell ───────────────────────

    // The old structure: the banner inside the page. A ColumnLayout whose
    // first row is the banner, the whole thing being page 0 of the main
    // column's StackLayout — exactly MessageView inside either shell.
    Component {
        id: bannerInThePage
        Item {
            width: 200; height: 100
            property bool viewingVoice: false
            property alias banner: banner
            property alias chatPage: chatPage
            property alias voicePage: voicePage
            StackLayout {
                anchors.fill: parent
                currentIndex: viewingVoice ? MainSurface.Voice : MainSurface.Chat
                Item {
                    id: chatPage
                    ColumnLayout {
                        anchors.fill: parent
                        Item { id: banner; Layout.fillWidth: true
                               Layout.preferredHeight: 28 }
                        Item { Layout.fillWidth: true; Layout.fillHeight: true }
                    }
                }
                Item { id: voicePage }
            }
        }
    }

    // The new structure: the banner is a ROW of the shell's main column, a
    // sibling of the StackLayout — and of the VoiceDock, which was already
    // mounted this way and is the precedent.
    Component {
        id: bannerInTheShell
        Item {
            width: 200; height: 100
            property bool viewingVoice: false
            property alias banner: banner
            property alias chatPage: chatPage
            property alias voicePage: voicePage
            property alias stack: stack
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                Item { id: banner; Layout.fillWidth: true
                       Layout.preferredHeight: 28 }
                StackLayout {
                    id: stack
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: viewingVoice ? MainSurface.Voice : MainSurface.Chat
                    Item { id: chatPage }
                    Item { id: voicePage }
                }
            }
        }
    }

    // THE BUG. Flip to the voice room and the banner goes dark with the
    // timeline that contains it — so a session expiring mid-call, or the
    // socket dropping mid-call, is announced to nobody.
    function test_a_banner_inside_the_page_disappears_with_the_page() {
        var s = createTemporaryObject(bannerInThePage, tc);
        verify(s);
        verify(s.banner.visible);
        s.viewingVoice = true;
        compare(s.voicePage.visible, true);
        compare(s.chatPage.visible, false);
        compare(s.banner.visible, false,
                "the old shape no longer reproduces the reported bug; "
                + "if this stops failing, the replica has drifted from it");
    }

    // And a row of the shell survives the same flip, because nothing the
    // StackLayout writes reaches outside its own children.
    function test_a_banner_that_is_a_row_of_the_shell_survives_the_flip() {
        var s = createTemporaryObject(bannerInTheShell, tc);
        verify(s.banner.visible);
        s.viewingVoice = true;
        compare(s.voicePage.visible, true);
        compare(s.chatPage.visible, false);
        compare(s.banner.visible, true,
                "the banner must be on screen during a call — that is the "
                + "whole reason it moved out of MessageView");
    }

    // A ROW, not an overlay, and this is the case that says why. The banner
    // takes its 28px off the top of the column and the surface below starts
    // underneath it; it never covers a pixel of whatever is showing. An item
    // anchored over the column would sit on top of the voice room's video
    // tiles, which is precisely the defect fix/mobile-voice-overlays closed
    // for the empty state (see qml/js/MainSurface.js).
    function test_the_row_takes_space_from_the_column_and_never_covers_it() {
        var s = createTemporaryObject(bannerInTheShell, tc);
        wait(0);
        compare(s.banner.y, 0);
        compare(s.banner.height, 28);
        // The pages begin where the banner ends...
        compare(s.stack.y, 28);
        compare(s.stack.height, s.height - 28);
        // ...so no part of the banner is over any part of a page.
        verify(s.banner.y + s.banner.height <= s.stack.y,
               "the banner overlaps the surface it sits above");

        // Still true with the voice room up, which is the case that matters.
        s.viewingVoice = true;
        wait(0);
        verify(s.banner.y + s.banner.height <= s.stack.y);
        compare(s.voicePage.height, s.height - 28);
    }
}
