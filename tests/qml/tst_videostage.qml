import QtQuick
import QtTest
import "../../qml/js/VideoStage.js" as VideoStage

// The voice room's video stage: which feeds exist, which one is on the
// big panel, and where every tile lands.
//
// This exercises qml/js/VideoStage.js — the file VoiceRoom.qml imports —
// not a copy of its rules. What it cannot reach is the QML half:
// VoiceRoom.qml imports the BSFChat module, which is compiled into the
// app binary and cannot be loaded from a test executable (the same
// constraint documented at the top of tst_transportbarinput.qml). So the
// structural invariant — that every VideoOutput lives in a delegate whose
// identity survives a selection change, which is what stops the picture
// blinking when you click a thumbnail — is guarded by a source scan in
// tests/test_qml_hygiene.cpp instead.
TestCase {
    id: tc
    name: "VideoStage"

    function feed(user, kind, started) {
        return {
            key: VideoStage.feedKey(user, kind),
            userId: user,
            kind: kind,
            streamId: VideoStage.streamIdFor(kind),
            isSelf: false,
            started: started
        };
    }

    function keysOf(feeds) {
        var out = [];
        for (var i = 0; i < feeds.length; ++i) out.push(feeds[i].key);
        return out;
    }

    // ── 1. Grid shape ────────────────────────────────────────────────
    //
    // The owner's request in one case each: "when there are multiple
    // users with webcam on, they should display side by side, so that
    // all can be seen at once (including self)".

    function test_grid_dims_data() {
        return [
            { tag: "1",  n: 1, cols: 1, rows: 1 },
            { tag: "2",  n: 2, cols: 2, rows: 1 },
            { tag: "3",  n: 3, cols: 2, rows: 2 },
            { tag: "4",  n: 4, cols: 2, rows: 2 },
            { tag: "5",  n: 5, cols: 3, rows: 2 },
            { tag: "6",  n: 6, cols: 3, rows: 2 },
            { tag: "7",  n: 7, cols: 3, rows: 3 },
        ];
    }

    function test_grid_dims(data) {
        var d = VideoStage.gridDims(data.n);
        compare(d.cols, data.cols, "columns for " + data.n);
        compare(d.rows, data.rows, "rows for " + data.n);
        // Every feed has a cell. A grid that cannot hold all of them is
        // the bug this whole change exists to fix.
        verify(d.cols * d.rows >= data.n, "grid too small for " + data.n);
    }

    function test_grid_dims_empty() {
        var d = VideoStage.gridDims(0);
        compare(d.cols, 0);
        compare(d.rows, 0);
    }

    // Two feeds share the width evenly and sit on one row — side by
    // side, which is the literal request.
    function test_grid_two_side_by_side() {
        var a = VideoStage.gridCell(0, 2, 1000, 600, 20);
        var b = VideoStage.gridCell(1, 2, 1000, 600, 20);
        compare(a.width, 490);
        compare(b.width, 490);
        compare(a.height, 600);
        compare(a.y, b.y, "same row");
        compare(a.x, 0);
        compare(b.x, 510);
    }

    // A short final row is centred rather than left-packed.
    function test_grid_short_last_row_is_centred() {
        var third = VideoStage.gridCell(2, 3, 1000, 600, 20);
        var cw = (1000 - 20) / 2;
        compare(third.width, cw);
        compare(third.x, (1000 - cw) / 2, "lone tile centred");
        verify(third.y > 0, "second row");
    }

    // Past six the columns stop growing and the tiles shrink.
    function test_grid_beyond_six_shrinks() {
        var six = VideoStage.gridCell(0, 6, 900, 600, 0);
        var seven = VideoStage.gridCell(0, 7, 900, 600, 0);
        compare(seven.width, six.width, "same column count");
        verify(seven.height < six.height, "rows added, tiles shrink");
    }

    // ── 2. Which feeds exist ─────────────────────────────────────────

    function test_merge_orders_screens_then_cameras() {
        var merged = VideoStage.mergeFeeds([], [
            { userId: "@zoe:x",  kind: VideoStage.CAMERA },
            { userId: "@abe:x",  kind: VideoStage.SCREEN },
            { userId: "@abe:x",  kind: VideoStage.CAMERA },
        ], 1);
        compare(keysOf(merged), ["screen|@abe:x", "camera|@abe:x", "camera|@zoe:x"]);
        // One user can be on camera AND sharing at the same time; those
        // are two feeds, and the keys must not collide.
        compare(merged[0].streamId, 0);
        compare(merged[1].streamId, 1);
    }

    // The `started` stamp is what "most recently" means, so it has to
    // survive unrelated churn — otherwise every join would re-age the
    // whole call and move the stage under people.
    function test_merge_preserves_started() {
        var first = VideoStage.mergeFeeds([], [
            { userId: "@abe:x", kind: VideoStage.CAMERA },
        ], 1);
        var second = VideoStage.mergeFeeds(first, [
            { userId: "@abe:x", kind: VideoStage.CAMERA },
            { userId: "@zoe:x", kind: VideoStage.CAMERA },
        ], 2);
        compare(second[0].started, 1, "existing feed keeps its age");
        compare(second[1].started, 2, "new feed is younger");
    }

    function test_merge_drops_duplicates_and_junk() {
        var merged = VideoStage.mergeFeeds([], [
            { userId: "@abe:x", kind: VideoStage.CAMERA },
            { userId: "@abe:x", kind: VideoStage.CAMERA },
            { userId: "", kind: VideoStage.CAMERA },
            null,
        ], 1);
        compare(merged.length, 1);
    }

    // sameFeeds is the guard that stops VoiceRoom reassigning its feed
    // list — and so destroying every VideoOutput — when nothing changed.
    function test_same_feeds_is_membership_only() {
        var a = VideoStage.mergeFeeds([], [{ userId: "@abe:x", kind: VideoStage.CAMERA }], 1);
        var b = VideoStage.mergeFeeds(a, [{ userId: "@abe:x", kind: VideoStage.CAMERA }], 9);
        verify(VideoStage.sameFeeds(a, b), "re-observing the same feed is not a change");
        var c = VideoStage.mergeFeeds(a, [
            { userId: "@abe:x", kind: VideoStage.CAMERA },
            { userId: "@zoe:x", kind: VideoStage.CAMERA },
        ], 2);
        verify(!VideoStage.sameFeeds(a, c), "a new feed is a change");
    }

    // ── 3. Default selection ─────────────────────────────────────────

    function test_default_selection_prefers_newest_screen() {
        var feeds = [
            feed("@abe:x", VideoStage.SCREEN, 1),
            feed("@bea:x", VideoStage.SCREEN, 5),
            feed("@cid:x", VideoStage.CAMERA, 9),
        ];
        compare(VideoStage.defaultSelection(feeds), "screen|@bea:x",
                "newest screen share wins over a newer camera");
    }

    function test_default_selection_falls_to_newest_camera() {
        var feeds = [
            feed("@abe:x", VideoStage.CAMERA, 1),
            feed("@bea:x", VideoStage.CAMERA, 5),
        ];
        compare(VideoStage.defaultSelection(feeds), "camera|@bea:x");
    }

    function test_default_selection_empty() {
        compare(VideoStage.defaultSelection([]), "");
        compare(VideoStage.defaultSelection(null), "");
    }

    // ── 4. Stage mode ────────────────────────────────────────────────

    function test_stage_mode_single_feed_fills() {
        var feeds = [feed("@abe:x", VideoStage.CAMERA, 1)];
        compare(VideoStage.stageMode(feeds, ""), "single");
        compare(VideoStage.stageKeys(feeds, "").length, 1);
    }

    // Cameras only: everyone on the stage at once, self included. No
    // strip picking, because there is nothing to pick between.
    function test_stage_mode_cameras_only_is_a_grid() {
        var feeds = [
            feed("@abe:x", VideoStage.CAMERA, 1),
            feed("@bea:x", VideoStage.CAMERA, 2),
            feed("@me:x",  VideoStage.CAMERA, 3),
        ];
        compare(VideoStage.stageMode(feeds, ""), "grid");
        compare(VideoStage.stageKeys(feeds, "").length, 3, "all three on stage");
    }

    // Add a screen share and the stage becomes a single panel again,
    // with everything else demoted to the strip.
    function test_stage_mode_with_screen_is_focus() {
        var feeds = [
            feed("@abe:x", VideoStage.CAMERA, 1),
            feed("@bea:x", VideoStage.CAMERA, 2),
            feed("@cid:x", VideoStage.SCREEN, 3),
        ];
        compare(VideoStage.stageMode(feeds, ""), "focus");
        compare(VideoStage.stageKeys(feeds, ""), ["screen|@cid:x"]);
        compare(VideoStage.stageIndexOf(feeds, "", "camera|@abe:x"), -1,
                "cameras go to the strip");
        // …and clicking a camera in the strip puts it on the stage.
        compare(VideoStage.stageKeys(feeds, "camera|@abe:x"), ["camera|@abe:x"]);
    }

    function test_stage_mode_empty() {
        compare(VideoStage.stageMode([], ""), "empty");
        compare(VideoStage.stageKeys([], "").length, 0);
    }

    // ── 5. Selection fallback ────────────────────────────────────────
    //
    // "A feed that stops is removed and, if it was selected, selection
    // falls back to auto."

    function test_selection_survives_unrelated_change() {
        var feeds = [
            feed("@abe:x", VideoStage.SCREEN, 1),
            feed("@bea:x", VideoStage.CAMERA, 2),
        ];
        compare(VideoStage.resolveSelection(feeds, "camera|@bea:x"), "camera|@bea:x");
    }

    function test_selection_falls_back_when_its_feed_stops() {
        var before = [
            feed("@abe:x", VideoStage.SCREEN, 1),
            feed("@bea:x", VideoStage.CAMERA, 2),
        ];
        var after = VideoStage.mergeFeeds(before, [
            { userId: "@abe:x", kind: VideoStage.SCREEN },
        ], 3);
        compare(VideoStage.resolveSelection(after, "camera|@bea:x"), "screen|@abe:x",
                "the stage does not go blank when the picked feed ends");
    }

    function test_selection_of_nothing_is_empty_string() {
        compare(VideoStage.resolveSelection([], "camera|@bea:x"), "");
    }

    // Escape: clearing the explicit pick returns to the auto rule.
    function test_empty_selection_means_auto() {
        var feeds = [
            feed("@abe:x", VideoStage.SCREEN, 1),
            feed("@bea:x", VideoStage.CAMERA, 2),
        ];
        compare(VideoStage.resolveSelection(feeds, ""), VideoStage.defaultSelection(feeds));
    }

    // ── 6. Arrow keys ────────────────────────────────────────────────

    function test_move_selection_walks_strip_order() {
        var feeds = [
            feed("@abe:x", VideoStage.SCREEN, 1),
            feed("@bea:x", VideoStage.CAMERA, 2),
            feed("@cid:x", VideoStage.CAMERA, 3),
        ];
        compare(VideoStage.moveSelection(feeds, "", 1), "camera|@bea:x",
                "right from the auto pick (the screen share) is the next tile");
        compare(VideoStage.moveSelection(feeds, "camera|@bea:x", 1), "camera|@cid:x");
        compare(VideoStage.moveSelection(feeds, "camera|@bea:x", -1), "screen|@abe:x");
    }

    function test_move_selection_clamps() {
        var feeds = [
            feed("@abe:x", VideoStage.SCREEN, 1),
            feed("@bea:x", VideoStage.CAMERA, 2),
        ];
        compare(VideoStage.moveSelection(feeds, "screen|@abe:x", -1), "screen|@abe:x",
                "does not wrap off the left");
        compare(VideoStage.moveSelection(feeds, "camera|@bea:x", 1), "camera|@bea:x",
                "does not wrap off the right");
        compare(VideoStage.moveSelection([], "", 1), "");
    }

    // ── 7. Strip slots ───────────────────────────────────────────────

    function test_strip_slots_fit_and_centre() {
        // Three thumbnails, plenty of room: each gets maxWidth and the
        // run is centred.
        var n = 3, gap = 10, maxW = 160, stripW = 1000;
        var total = n * maxW + (n - 1) * gap;
        for (var i = 0; i < n; ++i) {
            var s = VideoStage.stripSlot(i, n, stripW, gap, maxW);
            compare(s.width, maxW, "slot " + i + " width");
            compare(s.x, (stripW - total) / 2 + i * (maxW + gap), "slot " + i + " x");
        }
    }

    function test_strip_slots_shrink_rather_than_overflow() {
        // Ten feeds in a narrow strip: they shrink to fit. Nothing may
        // fall off the end — an unreachable thumbnail is an unclickable
        // feed.
        var n = 10, gap = 8, stripW = 600;
        var first = VideoStage.stripSlot(0, n, stripW, gap, 160);
        var last = VideoStage.stripSlot(n - 1, n, stripW, gap, 160);
        verify(first.width < 160, "thumbnails shrank");
        verify(first.x >= -0.001, "first slot is on screen");
        verify(last.x + last.width <= stripW + 0.001, "last slot is on screen");
    }

    function test_strip_slot_empty() {
        var s = VideoStage.stripSlot(0, 0, 600, 8, 160);
        compare(s.width, 0);
    }

    // ── 8. Wording ───────────────────────────────────────────────────

    function test_labels() {
        compare(VideoStage.feedLabel("Abe", VideoStage.SCREEN, false), "Abe — SCREEN SHARE");
        compare(VideoStage.feedLabel("Abe", VideoStage.CAMERA, false), "Abe — CAMERA");
        compare(VideoStage.feedLabel("Abe", VideoStage.SCREEN, true), "You — SCREEN SHARE");
        // S-7's placeholder wording, unchanged from the tiles it replaces.
        compare(VideoStage.placeholderText(VideoStage.SCREEN), "Starting share…");
        compare(VideoStage.placeholderText(VideoStage.CAMERA), "Starting camera…");
    }
}
