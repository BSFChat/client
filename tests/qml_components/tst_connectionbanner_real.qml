import QtQuick
import QtQuick.Layouts
import QtTest
// The real module, out of bsfchat-lib's compiled-in resources. Before the
// bsfchat-lib split this line could not be written in a test at all: the QML
// module was attached to the executable, so `import BSFChat` in a test binary
// failed at load with "module BSFChat is not installed".
import BSFChat
// The shipped rules, reached by the same qrc path src/main.cpp loads the
// shell from. Imported by URL rather than by relative source path on purpose:
// if moving the module onto a library ever shifted the resource prefix away
// from :/qt/qml/BSFChat/, this import is the thing that fails, and it fails
// here rather than at runtime on a user's machine.
import "qrc:/qt/qml/BSFChat/qml/js/ConnectionBanner.js" as Banner

// ConnectionBanner.qml — the file that ships, instantiated, driven through
// every connection state, and MEASURED.
//
// WHY THIS FILE EXISTS ALONGSIDE tests/qml/tst_connectionbanner.qml
//
// That one is a replica: it builds a Rectangle of its own and checks that the
// rules in qml/js/ConnectionBanner.js answer correctly. It cannot check that
// ConnectionBanner.qml reads those rules, because it never loads
// ConnectionBanner.qml. The standing substitute for that was text-scraping in
// tests/test_qml_hygiene.cpp — grep the .qml source for the property name and
// declare it wired. A guard of that shape passes over a component whose
// binding is present and wrong.
//
// So the cases here are deliberately the ones a replica CANNOT state:
//
//   * that the shipped component's `visible`, `color`, `Layout.preferredHeight`
//     and button state are driven by those rules and not by something else;
//   * that a hidden banner takes ZERO space off the surface below it,
//     measured as the y of its sibling in a real ColumnLayout, not asserted
//     from a property value;
//   * that the banner re-evaluates when the LIVE connection object's
//     properties change — the `connView` snapshot is a hand-rolled binding
//     over four reads, and a missed dependency in it is invisible to any test
//     that calls the .js functions directly;
//   * that pressing the real Button reaches serverManager.reauthenticateServer
//     with the real active index. Nothing about that wiring is in the .js file.
//
// The environment (AppSettings singleton, serverManager context property) is
// stubbed in tests/qml_components_test_main.cpp; read its header for what is
// faked and why. Theme.qml, ConnectionBanner.js and ConnectionBanner.qml are
// all the shipped files.
TestCase {
    id: tc
    name: "ConnectionBannerReal"
    when: windowShown
    width: 400
    height: 200
    visible: true

    // A real ColumnLayout, because the banner is a LAYOUT ROW of the shell's
    // main column and its whole contract is what it does to that column. The
    // filler below it is the surface the banner must not steal space from.
    ColumnLayout {
        id: column
        anchors.fill: parent
        spacing: 0

        ConnectionBanner {
            id: banner
        }

        Rectangle {
            id: surfaceBelow
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "transparent"
        }
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

    // The re-auth Button. Found by shape rather than by index so that adding
    // a sibling to the row (an accessibility affordance, say) does not
    // silently retarget the assertions onto the wrong item. `checkable` is
    // declared by QQuickAbstractButton and by nothing else in this tree.
    function reauthButton() {
        var d = descendants(banner);
        for (var i = 0; i < d.length; ++i)
            if (d[i].checkable !== undefined && d[i].text !== undefined)
                return d[i];
        return null;
    }

    // The message Text. Every Text in the tree has `elide`; the button's
    // contentItem is excluded by asking whether its parent is a button.
    function messageText() {
        var d = descendants(banner);
        for (var i = 0; i < d.length; ++i) {
            var it = d[i];
            if (it.elide === undefined || it.text === undefined)
                continue;
            if (it.parent && it.parent.checkable !== undefined)
                continue;
            return it;
        }
        return null;
    }

    function conn() {
        return serverManager.activeServer;
    }

    function setState(status, msg, needsReauth, busy) {
        var c = conn();
        c.connectionStatus = status;
        c.syncErrorMessage = msg;
        c.needsReauth = needsReauth;
        c.reauthInProgress = busy;
    }

    function init() {
        serverManager.resetForTest();
        // Settle the Behavior on Layout.preferredHeight before each case, so
        // a measurement is never reading the tail of the previous one.
        tryCompare(banner, "visible", false);
        tryCompare(banner.Layout, "preferredHeight", 0);
    }

    // ── 1. the module loads and the shipped component instantiates ───

    // The refactor's own smoke test. If bsfchat-lib's resources were not
    // linked into this binary, or the qmldir moved, nothing below would even
    // get as far as failing an assertion — so state it as a case of its own
    // rather than letting it surface as sixteen confusing errors.
    function test_a_the_real_component_loaded() {
        verify(banner !== null);
        // The type came from the BSFChat module, not from a local file.
        verify(banner.toString().indexOf("ConnectionBanner") >= 0,
               "expected a ConnectionBanner instance, got " + banner);
        // Theme is the module's singleton, and it resolved against the
        // AppSettings singleton the runner registered.
        verify(Theme.warn !== undefined);
        verify(Theme.danger !== undefined);
        verify(String(Theme.warn) !== String(Theme.danger));
    }

    // ── 2. a healthy connection costs the surface nothing ────────────

    // The measurement, not the property read. A replica can assert that
    // isShowing() is false; only a live layout can assert that the surface
    // below starts at the top of the column.
    function test_b_hidden_banner_takes_no_space() {
        setState(Banner.Connected, "", false, false);
        tryCompare(banner, "visible", false);
        tryCompare(banner.Layout, "preferredHeight", 0);
        tryCompare(surfaceBelow, "y", 0);
        verify(Math.abs(surfaceBelow.height - column.height) < 0.5);
    }

    // ── 3. each state, on the shipped component ──────────────────────

    function test_c_reconnecting_is_amber_and_28_high() {
        setState(Banner.Reconnecting, "", false, false);
        tryCompare(banner, "visible", true);
        tryCompare(banner.Layout, "preferredHeight", 28);
        // The real measurement: the banner occupies 28px of the column and
        // the surface below has moved down by exactly that.
        tryCompare(banner, "height", 28);
        tryCompare(surfaceBelow, "y", 28);
        compare(String(banner.color), String(Theme.warn));
        compare(messageText().text, "Reconnecting to server…");
    }

    function test_d_disconnected_is_red() {
        setState(Banner.Disconnected, "", false, false);
        tryCompare(banner, "visible", true);
        compare(String(banner.color), String(Theme.danger));
        compare(messageText().text,
                "Disconnected — messages won't send until the server is reachable");
    }

    function test_e_expired_prefers_the_servers_own_words() {
        setState(Banner.Expired, "Token revoked by administrator", true, false);
        tryCompare(banner, "visible", true);
        compare(String(banner.color), String(Theme.danger));
        compare(messageText().text, "Token revoked by administrator");
    }

    function test_f_expired_without_a_message_still_says_something() {
        setState(Banner.Expired, "", true, false);
        tryCompare(banner, "visible", true);
        compare(messageText().text,
                "Session expired — sign in again to reconnect");
        // The regression the fallback exists for: a red strip with a button
        // and no text in it.
        verify(messageText().text.length > 0);
    }

    // ── 4. the binding is live ───────────────────────────────────────

    // `connView` in ConnectionBanner.qml is a hand-written snapshot of four
    // property reads. If one of those reads is dropped or moved behind a
    // helper, the binding stops depending on it and the banner freezes on
    // whatever it last showed — with every rule in ConnectionBanner.js still
    // passing its own tests. This walks one live object through four states
    // and requires the shipped component to follow each one.
    function test_g_the_banner_follows_the_live_connection() {
        var c = conn();

        c.connectionStatus = Banner.Reconnecting;
        tryCompare(banner, "visible", true);
        compare(String(banner.color), String(Theme.warn));

        c.connectionStatus = Banner.Disconnected;
        tryCompare(banner, "visible", true);
        compare(String(banner.color), String(Theme.danger));

        // syncErrorMessage alone, with the status unchanged: the fourth read
        // in the snapshot, and the one most easily lost.
        c.connectionStatus = Banner.Expired;
        c.syncErrorMessage = "first";
        tryCompare(messageText(), "text", "first");
        c.syncErrorMessage = "second";
        tryCompare(messageText(), "text", "second");

        c.connectionStatus = Banner.Connected;
        tryCompare(banner, "visible", false);
        tryCompare(surfaceBelow, "y", 0);
    }

    // Losing the server entirely. Both shells mount the banner
    // unconditionally, so a null activeServer has to be survivable on the
    // real component and not only in the rules.
    function test_h_no_active_server_hides_the_banner() {
        setState(Banner.Disconnected, "", false, false);
        tryCompare(banner, "visible", true);
        serverManager.activeServer = null;
        tryCompare(banner, "visible", false);
        tryCompare(surfaceBelow, "y", 0);
    }

    // ── 5. the button, which is pure wiring ──────────────────────────

    function test_i_no_button_when_reauth_is_not_the_answer() {
        setState(Banner.Reconnecting, "", false, false);
        tryCompare(banner, "visible", true);
        var b = reauthButton();
        verify(b !== null);
        tryCompare(b, "visible", false);
    }

    function test_j_the_button_signs_in_again() {
        serverManager.activeServerIndex = 2;
        setState(Banner.Expired, "", true, false);
        tryCompare(banner, "visible", true);

        var b = reauthButton();
        verify(b !== null);
        tryCompare(b, "visible", true);
        tryCompare(b, "enabled", true);
        compare(b.text, "Sign in again");

        compare(serverManager.reauthCalls, 0);
        mouseClick(b);
        // The whole point: the shipped onClicked reaches the shipped slot
        // with the index the manager says is active. There is no .js file
        // that carries this, so no replica test can reach it — until now it
        // was a text-scrape for "reauthenticateServer" in the hygiene test.
        tryCompare(serverManager, "reauthCalls", 1);
        compare(serverManager.lastReauthIndex, 2);
    }

    function test_k_the_button_is_disabled_not_hidden_while_signing_in() {
        setState(Banner.Expired, "", true, true);
        tryCompare(banner, "visible", true);

        var b = reauthButton();
        tryCompare(b, "visible", true);
        tryCompare(b, "enabled", false);
        compare(b.text, "Signing in…");

        // Disabled means disabled: a click must not start a second round trip.
        mouseClick(b);
        compare(serverManager.reauthCalls, 0);
    }

    // ── 6. the strip stays inside the display ────────────────────────

    // The longest line, at phone width. The row is bounded to
    // parent.width - 2*Theme.mobileGutter precisely so this elides instead of
    // running out through the rounded corners; the bound is a binding on the
    // shipped component, so it is only measurable here.
    function test_l_the_longest_line_elides_at_phone_width() {
        var wasWidth = tc.width;
        tc.width = 320;
        setState(Banner.Disconnected, "", false, false);
        tryCompare(banner, "visible", true);

        waitForRendering(column);

        var t = messageText();
        var row = t.parent;
        // tryVerify, not verify. A layout pass is asynchronous: the width
        // set above reaches the RowLayout's children on the next polish, so
        // a bare verify here passes or fails depending on how many frames
        // the platform happened to have rendered — it passed under ctest and
        // failed run directly, which is the worst shape a geometry assertion
        // can have.
        tryVerify(function() {
            return row.width <= column.width - Theme.mobileGutter * 2 + 0.5;
        });
        tryVerify(function() { return t.width > 0 && t.width <= row.width + 0.5; });
        // Elided, not overflowing or wrapped: the text is wider than the
        // space it was given, and Text has been told to cut it.
        compare(t.elide, Text.ElideRight);
        tryVerify(function() { return t.implicitWidth > t.width; });
        tc.width = wasWidth;
    }
}
