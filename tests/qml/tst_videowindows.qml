import QtQuick
import QtTest
import "../../qml/js/VideoWindows.js" as VideoWindows
import "../../qml/js/VideoStage.js" as VideoStage

// Pop-out and fullscreen policy for the voice room's video feeds.
//
// This exercises qml/js/VideoWindows.js — the file VoiceRoom.qml,
// VideoPopoutWindow.qml and VideoFullscreenWindow.qml import — not a
// copy of its rules. Same constraint as tst_videostage.qml: those
// components import the BSFChat module, which is compiled into the app
// binary and cannot be loaded from a test executable, so the QML half
// (the windows themselves, their flags, what Escape does to them) is
// guarded by a source scan in tests/test_qml_hygiene.cpp instead.
TestCase {
    id: tc
    name: "VideoWindows"

    function feed(user, kind, started) {
        return {
            key: VideoStage.feedKey(user, kind),
            userId: user,
            kind: kind,
            streamId: VideoStage.streamIdFor(kind),
            isSelf: false,
            started: started || 1
        };
    }

    // ── 1. Titles ────────────────────────────────────────────────────
    //
    // The owner asked for "<name> — screen" / "<name> — camera",
    // verbatim.

    function test_window_titles() {
        compare(VideoWindows.windowTitle("Abe", VideoWindows.SCREEN, false),
                "Abe — screen");
        compare(VideoWindows.windowTitle("Abe", VideoWindows.CAMERA, false),
                "Abe — camera");
        compare(VideoWindows.windowTitle("Abe", VideoWindows.SCREEN, true),
                "You — screen");
        // A feed whose roster entry has not arrived yet still gets a
        // title — an untitled window on macOS is a blank title bar.
        compare(VideoWindows.windowTitle("", VideoWindows.CAMERA, false),
                "? — camera");
    }

    function test_fullscreen_label() {
        compare(VideoWindows.fullscreenLabel("Abe", VideoWindows.SCREEN, false),
                "Abe · Screen share");
        compare(VideoWindows.fullscreenLabel("Abe", VideoWindows.CAMERA, false),
                "Abe · Camera");
        compare(VideoWindows.fullscreenLabel("Abe", VideoWindows.CAMERA, true),
                "You · Camera");
    }

    // ── 2. Default size ──────────────────────────────────────────────

    function test_default_size_is_960x540_for_16_9() {
        var s = VideoWindows.defaultSize(16 / 9);
        compare(s.width, 960);
        compare(s.height, 540);
    }

    function test_default_size_follows_the_feed_aspect() {
        // A portrait phone camera must not open 960 px wide.
        var portrait = VideoWindows.defaultSize(9 / 16);
        verify(portrait.height > portrait.width, "portrait window is tall");
        fuzzyCompare(portrait.width / portrait.height, 9 / 16, 0.02);

        // An ultrawide share must not open as a 960x411 slot; the AREA
        // is what is held roughly constant, not the width.
        var ultra = VideoWindows.defaultSize(21 / 9);
        fuzzyCompare(ultra.width / ultra.height, 21 / 9, 0.02);
        verify(ultra.width > 960, "ultrawide is wider than the 16:9 default");
        var area = 960 * 540;
        fuzzyCompare(ultra.width * ultra.height / area, 1.0, 0.02);
    }

    function test_default_size_survives_a_feed_with_no_frames_yet() {
        // A feed that has not delivered a frame reports no aspect at all.
        // NaN geometry means a Window that never appears, so every one of
        // these has to land on the 16:9 default instead.
        var bad = [undefined, null, 0, -3, NaN, Infinity];
        for (var i = 0; i < bad.length; ++i) {
            var s = VideoWindows.defaultSize(bad[i]);
            compare(s.width, 960, "width for " + bad[i]);
            compare(s.height, 540, "height for " + bad[i]);
        }
    }

    function test_minimum_size_is_320x180() {
        var m = VideoWindows.minimumSize();
        compare(m.width, 320);
        compare(m.height, 180);
        // Nothing may come out below it, however extreme the aspect.
        var sliver = VideoWindows.defaultSize(0.0001);
        verify(sliver.width >= 320, "clamped width");
        verify(sliver.height >= 180, "clamped height");
    }

    // ── 3. Remembered geometry ───────────────────────────────────────

    function test_geometry_is_remembered_per_kind_not_per_feed() {
        // A feed key carries a user id; per-feed memory would grow a
        // settings entry for every person ever watched.
        compare(VideoWindows.geometryKey(VideoWindows.SCREEN), "screen");
        compare(VideoWindows.geometryKey(VideoWindows.CAMERA), "camera");
    }

    function test_saved_size_wins_over_the_default() {
        var g = VideoWindows.restoreGeometry(
            { width: 1280, height: 720, x: 100, y: 80 }, 16 / 9);
        compare(g.width, 1280);
        compare(g.height, 720);
        compare(g.x, 100);
        compare(g.y, 80);
        verify(g.hasPosition, "a stored position is offered");
    }

    function test_nothing_saved_falls_back_to_the_aspect_default() {
        var g = VideoWindows.restoreGeometry({}, 16 / 9);
        compare(g.width, 960);
        compare(g.height, 540);
        verify(!g.hasPosition, "no position to restore");

        var never = VideoWindows.restoreGeometry(
            { width: -1, height: -1, x: -1, y: -1 }, 16 / 9);
        compare(never.width, 960);
        verify(!never.hasPosition, "-1 is the never-stored sentinel");
    }

    function test_a_corrupt_saved_size_cannot_produce_an_ungrabbable_window() {
        var g = VideoWindows.restoreGeometry({ width: 12, height: 4 }, 16 / 9);
        compare(g.width, 320);
        compare(g.height, 180);
    }

    function test_a_position_on_a_monitor_that_is_gone_is_refused() {
        var laptop = [{ x: 0, y: 0, width: 1440, height: 900 }];
        // Where the window was while the second monitor was plugged in.
        verify(!VideoWindows.positionIsOnScreen(2400, 300, laptop),
               "off-desktop position refused");
        verify(VideoWindows.positionIsOnScreen(100, 100, laptop),
               "on-desktop position accepted");
        // Far enough right that the window would not fit: refused, so a
        // pop-out cannot open as a sliver hanging off the edge.
        verify(!VideoWindows.positionIsOnScreen(1400, 100, laptop),
               "position with no room for a minimum-size window refused");
        // Two monitors, the right-hand one negative-origin above.
        var dual = [{ x: 0, y: 0, width: 1440, height: 900 },
                    { x: 1440, y: -200, width: 1920, height: 1080 }];
        verify(VideoWindows.positionIsOnScreen(2400, 0, dual),
               "second monitor accepted once it is back");
        verify(!VideoWindows.positionIsOnScreen(-50, 0, dual),
               "left of every screen refused");
    }

    function test_pop_outs_cascade_instead_of_stacking_exactly() {
        // Two pop-outs restoring the same remembered position would land
        // on top of each other and read as one window.
        compare(VideoWindows.cascadeOffset(0), 0);
        verify(VideoWindows.cascadeOffset(1) > 0, "second is offset");
        verify(VideoWindows.cascadeOffset(2) > VideoWindows.cascadeOffset(1),
               "third is offset further");
        // Bounded — it must not walk off the screen with eight windows up.
        for (var n = 0; n < 12; ++n)
            verify(VideoWindows.cascadeOffset(n) < 200,
                   "cascade stays bounded at " + n);
    }

    // ── 4. Pop-out bookkeeping ───────────────────────────────────────

    function test_first_request_opens_and_the_second_raises() {
        var abe = feed("@abe:x", VideoWindows.SCREEN);
        var st = VideoWindows.emptyState();
        compare(VideoWindows.count(st), 0);

        var r1 = VideoWindows.requestPopout(st, abe);
        compare(r1.action, "open");
        compare(r1.key, abe.key);
        compare(VideoWindows.count(r1.state), 1);
        verify(VideoWindows.isOpen(r1.state, abe.key), "recorded as open");

        // Clicking pop-out again on the same feed raises the window that
        // is already up rather than opening a second copy of it.
        var r2 = VideoWindows.requestPopout(r1.state, abe);
        compare(r2.action, "raise");
        compare(VideoWindows.count(r2.state), 1);
        verify(r2.state === r1.state,
               "a raise must not hand the Instantiator a new model");
    }

    function test_multiple_feeds_can_be_popped_out_at_once() {
        var abe = feed("@abe:x", VideoWindows.SCREEN);
        var bea = feed("@bea:x", VideoWindows.CAMERA);
        // One peer can be both, and each is its own window.
        var abeCam = feed("@abe:x", VideoWindows.CAMERA);

        var st = VideoWindows.requestPopout(
            VideoWindows.emptyState(), abe).state;
        st = VideoWindows.requestPopout(st, bea).state;
        st = VideoWindows.requestPopout(st, abeCam).state;

        compare(VideoWindows.count(st), 3);
        var keys = VideoWindows.openKeys(st);
        // Open order, oldest first — what the cascade is reckoned from.
        compare(keys[0], abe.key);
        compare(keys[1], bea.key);
        compare(keys[2], abeCam.key);
        // The screen and the camera of one peer really are separate.
        verify(abe.key !== abeCam.key, "one peer, two feeds, two windows");
    }

    function test_a_feed_with_no_key_cannot_open_a_window() {
        var st = VideoWindows.emptyState();
        var r = VideoWindows.requestPopout(st, null);
        compare(r.action, "none");
        compare(VideoWindows.count(r.state), 0);
        r = VideoWindows.requestPopout(st, { userId: "@abe:x" });
        compare(r.action, "none");
    }

    function test_closing_is_idempotent() {
        var abe = feed("@abe:x", VideoWindows.SCREEN);
        var st = VideoWindows.requestPopout(
            VideoWindows.emptyState(), abe).state;

        st = VideoWindows.closePopout(st, abe.key);
        compare(VideoWindows.count(st), 0);

        // The window can close itself (feed ended) at the same moment
        // the user clicks close. The second close is not an error, and
        // must not hand the Instantiator a new array either.
        var again = VideoWindows.closePopout(st, abe.key);
        compare(VideoWindows.count(again), 0);
        verify(again === st, "a no-op close keeps the model identity");
    }

    function test_closing_one_leaves_the_others_open() {
        var abe = feed("@abe:x", VideoWindows.SCREEN);
        var bea = feed("@bea:x", VideoWindows.CAMERA);
        var st = VideoWindows.requestPopout(
            VideoWindows.emptyState(), abe).state;
        st = VideoWindows.requestPopout(st, bea).state;

        st = VideoWindows.closePopout(st, abe.key);
        compare(VideoWindows.count(st), 1);
        verify(VideoWindows.isOpen(st, bea.key), "the other window stays");
        verify(!VideoWindows.isOpen(st, abe.key), "the closed one is gone");
    }

    // ── 5. A pop-out closes when its feed ends ───────────────────────

    function test_a_popout_whose_feed_stopped_is_stale() {
        var abe = feed("@abe:x", VideoWindows.SCREEN);
        var bea = feed("@bea:x", VideoWindows.CAMERA);
        var st = VideoWindows.requestPopout(
            VideoWindows.emptyState(), abe).state;
        st = VideoWindows.requestPopout(st, bea).state;

        // Abe stopped sharing; Bea's camera is still up.
        var stale = VideoWindows.staleKeys(st, [bea]);
        compare(stale.length, 1);
        compare(stale[0], abe.key);

        st = VideoWindows.pruneToFeeds(st, [bea]);
        compare(VideoWindows.count(st), 1);
        verify(VideoWindows.isOpen(st, bea.key), "Bea's window survives");
    }

    function test_leaving_the_channel_closes_every_popout() {
        var st = VideoWindows.requestPopout(
            VideoWindows.emptyState(),
            feed("@abe:x", VideoWindows.SCREEN)).state;
        st = VideoWindows.requestPopout(
            st, feed("@bea:x", VideoWindows.CAMERA)).state;

        // No feeds at all is what leaving the voice channel looks like.
        compare(VideoWindows.staleKeys(st, []).length, 2);
        compare(VideoWindows.count(VideoWindows.pruneToFeeds(st, [])), 0);
    }

    function test_pruning_a_healthy_state_keeps_its_identity() {
        // The feed rescan runs four times a second while anyone is
        // sharing. If a no-op prune returned a copy, the Instantiator
        // would destroy and rebuild every pop-out window that often.
        var abe = feed("@abe:x", VideoWindows.SCREEN);
        var st = VideoWindows.requestPopout(
            VideoWindows.emptyState(), abe).state;
        var same = VideoWindows.pruneToFeeds(st, [abe]);
        verify(same === st, "unchanged state keeps its reference");
    }

    // ── 6. Which window owns a fullscreen request ────────────────────

    function test_fullscreen_belongs_to_the_popout_when_there_is_one() {
        var abe = feed("@abe:x", VideoWindows.SCREEN);
        var st = VideoWindows.emptyState();

        // Not popped out: the voice room opens the fullscreen window.
        compare(VideoWindows.fullscreenOwner(st, abe.key), "room");

        // Popped out: the pop-out goes fullscreen instead. Two
        // fullscreen windows of one feed, stacked on one screen, is the
        // failure this rule exists to prevent.
        st = VideoWindows.requestPopout(st, abe).state;
        compare(VideoWindows.fullscreenOwner(st, abe.key), "popout");

        // Closing the pop-out hands ownership back.
        st = VideoWindows.closePopout(st, abe.key);
        compare(VideoWindows.fullscreenOwner(st, abe.key), "room");
    }

    function test_fullscreen_ownership_is_per_feed() {
        var abe = feed("@abe:x", VideoWindows.SCREEN);
        var bea = feed("@bea:x", VideoWindows.CAMERA);
        var st = VideoWindows.requestPopout(
            VideoWindows.emptyState(), abe).state;
        compare(VideoWindows.fullscreenOwner(st, abe.key), "popout");
        compare(VideoWindows.fullscreenOwner(st, bea.key), "room");
    }

    // ── 7. Overlay auto-hide ─────────────────────────────────────────

    function test_overlay_fades_after_two_idle_seconds() {
        var t0 = 10000;
        verify(VideoWindows.overlayVisible(t0, t0), "visible on the move");
        verify(VideoWindows.overlayVisible(t0, t0 + 1999), "still visible at 2s");
        verify(!VideoWindows.overlayVisible(t0, t0 + 2001), "hidden after 2s");
        // A fresh move brings it back.
        verify(VideoWindows.overlayVisible(t0 + 5000, t0 + 5001), "back on move");
    }

    function test_overlay_starts_visible_before_any_movement() {
        // Opening fullscreen with the mouse perfectly still must still
        // show the exit button, or the only way out is the keyboard.
        verify(VideoWindows.overlayVisible(0, 99999), "visible with no moves");
        verify(VideoWindows.overlayVisible(undefined, 99999), "and with none at all");
    }

    // ── 8. Surface routing ───────────────────────────────────────────
    //
    // Three components attach a VideoOutput to a feed now (the in-room
    // tile, the pop-out, the fullscreen window) and a self-feed routed
    // to the registry is a permanently black window: our own preview
    // never travels the remote-peer path. Fakes stand in for the
    // controllers and the registry — the real ones are C++ context
    // properties that only the app binary has.

    function fakeRegistry() {
        return {
            calls: [],
            attachOutput: function(userId, streamId, sink) {
                this.calls.push([userId, streamId, sink]);
            }
        };
    }

    function fakeController() {
        return {
            sinks: [],
            forwardTo: function(sink) { this.sinks.push(sink); }
        };
    }

    function test_a_remote_feed_attaches_to_the_registry() {
        var reg = fakeRegistry();
        var ss = fakeController();
        var cam = fakeController();
        var sink = { name: "popout-sink" };

        var how = VideoWindows.attachFeed(
            feed("@abe:x", VideoWindows.SCREEN), sink, reg, ss, cam);

        compare(how, "registry");
        compare(reg.calls.length, 1);
        compare(reg.calls[0][0], "@abe:x");
        compare(reg.calls[0][1], VideoStage.streamIdFor(VideoWindows.SCREEN));
        compare(reg.calls[0][2], sink);
        // The local controllers were not touched.
        compare(ss.sinks.length, 0);
        compare(cam.sinks.length, 0);
    }

    function test_our_own_feeds_go_to_their_controller_not_the_registry() {
        var reg = fakeRegistry();
        var ss = fakeController();
        var cam = fakeController();

        var mine = feed("@me:x", VideoWindows.SCREEN);
        mine.isSelf = true;
        compare(VideoWindows.attachFeed(mine, { n: 1 }, reg, ss, cam),
                "local-screen");
        compare(ss.sinks.length, 1);
        compare(reg.calls.length, 0);

        var myCam = feed("@me:x", VideoWindows.CAMERA);
        myCam.isSelf = true;
        compare(VideoWindows.attachFeed(myCam, { n: 2 }, reg, ss, cam),
                "local-camera");
        compare(cam.sinks.length, 1);
        compare(reg.calls.length, 0);
    }

    function test_a_missing_controller_or_registry_is_not_an_error() {
        // Neither controller exists on every platform (src/main.cpp) and
        // there is no active server before a connection. Attaching has
        // to be a no-op, not an exception that abandons the rest of the
        // window's construction half-done.
        var mine = feed("@me:x", VideoWindows.CAMERA);
        mine.isSelf = true;
        compare(VideoWindows.attachFeed(mine, { n: 1 }, null, null, null),
                "none");
        compare(VideoWindows.attachFeed(
                    feed("@abe:x", VideoWindows.SCREEN), { n: 1 },
                    null, null, null), "none");
        compare(VideoWindows.attachFeed(null, { n: 1 },
                                        fakeRegistry(), null, null), "none");
        compare(VideoWindows.attachFeed(
                    feed("@abe:x", VideoWindows.SCREEN), null,
                    fakeRegistry(), null, null), "none");
    }

    function test_the_same_feed_can_be_attached_to_several_surfaces() {
        // This is the whole point of the registry fan-out
        // (tests/test_video_registry.cpp): the tile stays live while the
        // feed is also in a pop-out and a fullscreen window.
        var reg = fakeRegistry();
        var abe = feed("@abe:x", VideoWindows.SCREEN);
        VideoWindows.attachFeed(abe, { n: "tile" }, reg, null, null);
        VideoWindows.attachFeed(abe, { n: "popout" }, reg, null, null);
        VideoWindows.attachFeed(abe, { n: "fullscreen" }, reg, null, null);
        compare(reg.calls.length, 3);
        verify(reg.calls[0][2] !== reg.calls[1][2], "three distinct sinks");
    }
}
