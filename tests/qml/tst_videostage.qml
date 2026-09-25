import QtQuick
import QtTest
import "../../qml/js/VideoStage.js" as VideoStage

// The voice room's video stage: which feeds exist, how the grid is
// shaped, which feed (if any) the user has expanded, and where every
// tile lands.
//
// This exercises qml/js/VideoStage.js — the file VoiceRoom.qml imports —
// not a copy of its rules. What it cannot reach is the QML half:
// VoiceRoom.qml imports the BSFChat module, which is compiled into the
// app binary and cannot be loaded from a test executable (the same
// constraint documented at the top of tst_transportbarinput.qml). So the
// structural invariant — that every VideoOutput lives in a delegate whose
// identity survives a layout change, which is what stops the picture
// blinking when you expand a feed — is guarded by a source scan in
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

    function cameras(n) {
        var out = [];
        for (var i = 0; i < n; ++i)
            out.push(feed("@u" + i + ":x", VideoStage.CAMERA, i + 1));
        return out;
    }

    // The area of one 16:9 tile aspect-fit into a cols×rows cell of a
    // width×height stage. This is the quantity the grid rule maximises,
    // written out independently here so the assertions below are a
    // check on VideoStage.js rather than a restatement of it.
    function tileArea(cols, rows, width, height) {
        var cw = width / cols;
        var ch = height / rows;
        var w = Math.min(cw, ch * 16 / 9);
        return w * (w / (16 / 9));
    }

    // ── 1. Grid shape ────────────────────────────────────────────────
    //
    // The owner's request: "with multiple streams/webcam feeds at once,
    // we see a grid of them (each as big as possible)".
    //
    // A landscape stage. At 16:9 and wider this is the shape the owner
    // named case by case: 1 fills, 2 side by side, 3–4 as 2×2, 5–6 as
    // 3×2, 7–9 as 3×3.

    readonly property int landW: 1920
    readonly property int landH: 900

    function test_grid_dims_landscape_data() {
        return [
            { tag: "1", n: 1, cols: 1, rows: 1 },
            { tag: "2", n: 2, cols: 2, rows: 1 },
            { tag: "3", n: 3, cols: 2, rows: 2 },
            { tag: "4", n: 4, cols: 2, rows: 2 },
            { tag: "5", n: 5, cols: 3, rows: 2 },
            { tag: "6", n: 6, cols: 3, rows: 2 },
            { tag: "7", n: 7, cols: 3, rows: 3 },
            { tag: "8", n: 8, cols: 3, rows: 3 },
            { tag: "9", n: 9, cols: 3, rows: 3 },
        ];
    }

    function test_grid_dims_landscape(data) {
        var d = VideoStage.gridDims(data.n, tc.landW / tc.landH);
        compare(d.cols, data.cols, "columns for " + data.n);
        compare(d.rows, data.rows, "rows for " + data.n);
        verify(d.cols * d.rows >= data.n, "grid too small for " + data.n);
    }

    // A portrait stage turns the same counts on their side. The tall
    // narrow case really does want a single column for a while: a
    // 16:9 feed in a 450-wide cell is half letterbox, and one column of
    // full-width feeds beats two columns of postage stamps. The
    // maximality test below is the proof; these are the numbers.

    readonly property int portW: 900
    readonly property int portH: 1600

    function test_grid_dims_portrait_data() {
        return [
            { tag: "1", n: 1, cols: 1, rows: 1 },
            { tag: "2", n: 2, cols: 1, rows: 2 },
            { tag: "3", n: 3, cols: 1, rows: 3 },
            { tag: "4", n: 4, cols: 1, rows: 4 },
            { tag: "5", n: 5, cols: 1, rows: 5 },
            { tag: "6", n: 6, cols: 1, rows: 6 },
            { tag: "7", n: 7, cols: 2, rows: 4 },
            { tag: "8", n: 8, cols: 2, rows: 4 },
            { tag: "9", n: 9, cols: 2, rows: 5 },
        ];
    }

    function test_grid_dims_portrait(data) {
        var d = VideoStage.gridDims(data.n, tc.portW / tc.portH);
        compare(d.cols, data.cols, "columns for " + data.n);
        compare(d.rows, data.rows, "rows for " + data.n);
        verify(d.cols * d.rows >= data.n, "grid too small for " + data.n);
    }

    // The rule itself, rather than its answers: whatever shape is
    // chosen, no other column count gives a bigger tile. Run over a
    // spread of stage shapes — a wide stage, exactly 16:9, a squarer
    // one, and two portraits — so a change to the tie-break or the
    // candidate set cannot quietly make some count worse.
    function test_grid_dims_are_maximal_data() {
        return [
            { tag: "wide 2.13",   w: 1920, h: 900  },
            { tag: "16:9",        w: 1600, h: 900  },
            { tag: "squarish",    w: 1000, h: 600  },
            { tag: "portrait .75",w: 900,  h: 1200 },
            { tag: "portrait .56",w: 900,  h: 1600 },
        ];
    }

    function test_grid_dims_are_maximal(data) {
        for (var n = 1; n <= 12; ++n) {
            var d = VideoStage.gridDims(n, data.w / data.h);
            var chosen = tileArea(d.cols, d.rows, data.w, data.h);
            for (var cols = 1; cols <= n; ++cols) {
                var rows = Math.ceil(n / cols);
                var alt = tileArea(cols, rows, data.w, data.h);
                verify(chosen >= alt - 1e-6,
                       data.tag + ", " + n + " feeds: chose "
                       + d.cols + "x" + d.rows + " (tile " + chosen.toFixed(0)
                       + ") but " + cols + "x" + rows + " gives "
                       + alt.toFixed(0));
            }
        }
    }

    // The two-feed case both ways round, because it is the one the
    // owner called out and the one where the stage's aspect actually
    // flips the answer. The break-even is the tile aspect itself: a
    // stage wider than 16:9 wants them side by side, narrower wants
    // them stacked.
    function test_two_feeds_side_by_side_on_a_wide_stage() {
        var d = VideoStage.gridDims(2, 1920 / 900);
        compare(d.cols, 2, "two columns");
        compare(d.rows, 1, "one row");
        verify(tileArea(2, 1, 1920, 900) > tileArea(1, 2, 1920, 900),
               "side by side really is the larger tile here");
    }

    function test_two_feeds_stacked_on_a_tall_stage() {
        var d = VideoStage.gridDims(2, 900 / 1600);
        compare(d.cols, 1, "one column");
        compare(d.rows, 2, "two rows");
        verify(tileArea(1, 2, 900, 1600) > tileArea(2, 1, 900, 1600),
               "stacked really is the larger tile here");
    }

    // Degenerate inputs: no feeds, and a stage with no size yet (the
    // first binding evaluation before the layout has run).
    function test_grid_dims_empty() {
        var d = VideoStage.gridDims(0, 16 / 9);
        compare(d.cols, 0);
        compare(d.rows, 0);
    }

    function test_grid_dims_without_an_aspect_assumes_landscape() {
        // No aspect (or a zero-sized stage) falls back to 16:9, which
        // is where the owner's case-by-case list applies.
        compare(VideoStage.gridDims(2).cols, 2);
        compare(VideoStage.gridDims(6, 0).cols, 3);
        compare(VideoStage.gridDims(6, 0).rows, 2);
    }

    // ── 2. Grid geometry ─────────────────────────────────────────────

    function test_grid_cells_split_a_wide_stage_evenly() {
        var a = VideoStage.gridCell(0, 2, 1920, 900, 20);
        var b = VideoStage.gridCell(1, 2, 1920, 900, 20);
        compare(a.width, 950);
        compare(b.width, 950);
        compare(a.height, 900, "full height: one row");
        compare(a.y, b.y, "same row");
        compare(a.x, 0);
        compare(b.x, 970);
    }

    // The same two feeds on a portrait stage stack instead, and
    // gridCell follows gridDims without being told twice.
    function test_grid_cells_stack_on_a_portrait_stage() {
        var a = VideoStage.gridCell(0, 2, 900, 1600, 20);
        var b = VideoStage.gridCell(1, 2, 900, 1600, 20);
        compare(a.width, 900, "full width: one column");
        compare(a.x, b.x, "same column");
        verify(b.y > a.y, "second row");
    }

    // A short final row is centred rather than left-packed.
    function test_grid_short_last_row_is_centred() {
        var third = VideoStage.gridCell(2, 3, 1920, 900, 20);
        var cw = (1920 - 20) / 2;
        compare(third.width, cw);
        compare(third.x, (1920 - cw) / 2, "lone tile centred");
        verify(third.y > 0, "second row");
    }

    // Past nine the columns grow again and every tile shrinks, which is
    // the honest answer to a call that has outgrown the stage.
    function test_grid_beyond_nine_shrinks() {
        var nine = VideoStage.gridCell(0, 9, 1920, 900, 0);
        var ten = VideoStage.gridCell(0, 10, 1920, 900, 0);
        verify(ten.width < nine.width, "tiles shrink rather than overflow");
        var d = VideoStage.gridDims(10, 1920 / 900);
        verify(d.cols * d.rows >= 10, "still a cell for every feed");
    }

    function test_grid_cell_of_nothing() {
        var c = VideoStage.gridCell(0, 0, 1920, 900, 20);
        compare(c.width, 0);
        compare(c.height, 0);
    }

    // ── 3. Which feeds exist ─────────────────────────────────────────

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

    // The `started` stamp has to survive unrelated churn — otherwise
    // every join would re-age the whole call and reorder the grid under
    // people mid-sentence.
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

    // ── 4. The grid is the default ───────────────────────────────────
    //
    // The behaviour this whole change exists to produce: nothing is
    // promoted to the stage on its own. A screen share is a feed in the
    // grid like any other until somebody clicks it.

    function test_a_screen_share_does_not_auto_expand() {
        var feeds = [
            feed("@cid:x", VideoStage.SCREEN, 3),
            feed("@abe:x", VideoStage.CAMERA, 1),
            feed("@bea:x", VideoStage.CAMERA, 2),
        ];
        compare(VideoStage.stageMode(feeds, ""), "grid");
        compare(VideoStage.stageKeys(feeds, "").length, 3,
                "the share and both cameras are all on the stage");
        compare(VideoStage.expandedKey(feeds, ""), "",
                "nothing is expanded until it is clicked");
        verify(VideoStage.stageIndexOf(feeds, "", "camera|@abe:x") >= 0,
               "a camera is not pushed to the strip by somebody sharing");
    }

    // Two screen shares at once, which used to be the worst case: the
    // newest one took the stage and the other became a thumbnail.
    function test_two_screen_shares_are_both_in_the_grid() {
        var feeds = [
            feed("@abe:x", VideoStage.SCREEN, 1),
            feed("@bea:x", VideoStage.SCREEN, 5),
        ];
        compare(VideoStage.stageMode(feeds, ""), "grid");
        compare(VideoStage.stageKeys(feeds, ""),
                ["screen|@abe:x", "screen|@bea:x"]);
    }

    function test_cameras_only_is_a_grid_too() {
        var feeds = cameras(3);
        compare(VideoStage.stageMode(feeds, ""), "grid");
        compare(VideoStage.stageKeys(feeds, "").length, 3, "all three on stage");
    }

    // A lone feed fills the stage, and never grows a strip holding one
    // thumbnail of itself.
    function test_one_feed_fills_the_stage() {
        var feeds = cameras(1);
        compare(VideoStage.stageMode(feeds, ""), "grid");
        compare(VideoStage.stageCount(feeds, ""), 1);
        compare(VideoStage.gridDims(1, 1920 / 900).cols, 1);
        // Even asked to expand it, there is nothing to expand it away
        // from, so the view stays the grid and no strip appears.
        compare(VideoStage.stageMode(feeds, feeds[0].key), "grid");
    }

    function test_stage_mode_empty() {
        compare(VideoStage.stageMode([], ""), "empty");
        compare(VideoStage.stageKeys([], "").length, 0);
        compare(VideoStage.stageCount([], ""), 0);
    }

    // ── 5. Expand, collapse, swap ────────────────────────────────────

    function test_clicking_a_tile_expands_it() {
        var feeds = [
            feed("@cid:x", VideoStage.SCREEN, 3),
            feed("@abe:x", VideoStage.CAMERA, 1),
            feed("@bea:x", VideoStage.CAMERA, 2),
        ];
        var sel = VideoStage.toggleExpanded(feeds, "", "camera|@abe:x");
        compare(sel, "camera|@abe:x");
        compare(VideoStage.stageMode(feeds, sel), "expanded");
        compare(VideoStage.stageKeys(feeds, sel), ["camera|@abe:x"],
                "the clicked feed alone fills the stage");
        compare(VideoStage.stageIndexOf(feeds, sel, "screen|@cid:x"), -1,
                "everything else drops to the strip");
        compare(VideoStage.stageIndexOf(feeds, sel, "camera|@bea:x"), -1);
    }

    // Clicking the expanded feed again — on the stage, or as its "on
    // stage" chip in the strip, which is the same key — collapses.
    function test_clicking_the_expanded_feed_collapses_to_the_grid() {
        var feeds = cameras(3);
        var sel = "camera|@u1:x";
        compare(VideoStage.toggleExpanded(feeds, sel, sel), "",
                "same feed again means 'show me everything'");
        compare(VideoStage.stageMode(feeds, ""), "grid");
    }

    function test_clicking_a_different_thumbnail_swaps_the_expanded_feed() {
        var feeds = cameras(3);
        var sel = VideoStage.toggleExpanded(feeds, "camera|@u0:x",
                                            "camera|@u2:x");
        compare(sel, "camera|@u2:x");
        compare(VideoStage.stageKeys(feeds, sel), ["camera|@u2:x"]);
    }

    // Escape is written as "" by VoiceRoom rather than routed through
    // toggleExpanded, so pin that "" really is the grid.
    function test_escape_is_the_grid() {
        var feeds = cameras(3);
        compare(VideoStage.stageMode(feeds, ""), "grid");
        compare(VideoStage.stageCount(feeds, ""), 3);
    }

    // A click on something that is not a feed (a stale key from a
    // delegate mid-teardown) leaves the view exactly as it was.
    function test_clicking_a_feed_that_is_gone_changes_nothing() {
        var feeds = cameras(3);
        compare(VideoStage.toggleExpanded(feeds, "camera|@u1:x", "camera|@ghost:x"),
                "camera|@u1:x");
        compare(VideoStage.toggleExpanded(feeds, "", ""), "");
    }

    // ── 6. A feed that stops while expanded ──────────────────────────
    //
    // Back to the grid — NOT to some other feed. The user expanded that
    // one; promoting a different one would be a view they never asked
    // for, which is the old behaviour in miniature.

    function test_expanded_feed_ending_returns_to_the_grid() {
        var before = [
            feed("@abe:x", VideoStage.SCREEN, 1),
            feed("@bea:x", VideoStage.CAMERA, 2),
        ];
        var after = VideoStage.mergeFeeds(before, [
            { userId: "@abe:x", kind: VideoStage.SCREEN },
        ], 3);
        compare(VideoStage.expandedKey(after, "camera|@bea:x"), "",
                "the stopped feed does not hand the stage to the share");
        compare(VideoStage.stageMode(after, "camera|@bea:x"), "grid");
    }

    // Selection persists across membership changes while its feed
    // survives — a peer joining must not collapse what you are watching.
    function test_expanded_feed_survives_unrelated_membership_changes() {
        var before = cameras(2);
        var after = VideoStage.mergeFeeds(before, [
            { userId: "@u0:x", kind: VideoStage.CAMERA },
            { userId: "@u1:x", kind: VideoStage.CAMERA },
            { userId: "@new:x", kind: VideoStage.CAMERA },
        ], 9);
        compare(VideoStage.expandedKey(after, "camera|@u1:x"), "camera|@u1:x");
        compare(VideoStage.stageMode(after, "camera|@u1:x"), "expanded");
    }

    // Down to one feed while that feed is the expanded one: the strip
    // would hold a single chip of the only picture, so it collapses.
    function test_last_feed_standing_is_a_grid_again() {
        var feeds = cameras(1);
        compare(VideoStage.stageMode(feeds, "camera|@u0:x"), "grid");
    }

    function test_expanded_key_of_nothing() {
        compare(VideoStage.expandedKey([], "camera|@u0:x"), "");
        compare(VideoStage.expandedKey(null, "camera|@u0:x"), "");
    }

    // ── 7. Keyboard ──────────────────────────────────────────────────

    // In the grid the arrows do nothing: every feed is already on
    // screen, so there is nothing to walk between, and expanding one
    // from a key press would be a large unasked-for change of view.
    function test_arrows_do_nothing_in_the_grid() {
        var feeds = cameras(3);
        compare(VideoStage.moveSelection(feeds, "", 1), "",
                "right in the grid is a no-op");
        compare(VideoStage.moveSelection(feeds, "", -1), "",
                "left in the grid is a no-op");
    }

    // With something expanded they walk which feed that is, in feed
    // order (screens first, then cameras, oldest first).
    function test_arrows_walk_the_expanded_feed() {
        var feeds = [
            feed("@abe:x", VideoStage.SCREEN, 1),
            feed("@bea:x", VideoStage.CAMERA, 2),
            feed("@cid:x", VideoStage.CAMERA, 3),
        ];
        compare(VideoStage.moveSelection(feeds, "screen|@abe:x", 1),
                "camera|@bea:x");
        compare(VideoStage.moveSelection(feeds, "camera|@bea:x", 1),
                "camera|@cid:x");
        compare(VideoStage.moveSelection(feeds, "camera|@bea:x", -1),
                "screen|@abe:x");
    }

    // Clamped, not wrapped: holding an arrow comes to rest at an end.
    function test_arrows_clamp() {
        var feeds = [
            feed("@abe:x", VideoStage.SCREEN, 1),
            feed("@bea:x", VideoStage.CAMERA, 2),
        ];
        compare(VideoStage.moveSelection(feeds, "screen|@abe:x", -1),
                "screen|@abe:x", "does not wrap off the left");
        compare(VideoStage.moveSelection(feeds, "camera|@bea:x", 1),
                "camera|@bea:x", "does not wrap off the right");
        compare(VideoStage.moveSelection([], "", 1), "");
        // An expanded key whose feed has gone is already the grid, so
        // the arrows have nothing to walk either.
        compare(VideoStage.moveSelection(feeds, "camera|@ghost:x", 1), "");
    }

    // `F` and the header button: the expanded feed, else whatever the
    // pointer is over, else the first feed — never nothing while there
    // is a feed to full-screen.
    function test_focus_key_prefers_the_expanded_feed() {
        var feeds = cameras(3);
        compare(VideoStage.focusKeyFor(feeds, "camera|@u2:x", "camera|@u0:x"),
                "camera|@u2:x", "expanded beats hovered");
    }

    function test_focus_key_falls_to_the_hovered_tile_in_the_grid() {
        var feeds = cameras(3);
        compare(VideoStage.focusKeyFor(feeds, "", "camera|@u1:x"),
                "camera|@u1:x");
    }

    function test_focus_key_falls_to_the_first_feed() {
        var feeds = cameras(3);
        compare(VideoStage.focusKeyFor(feeds, "", ""), "camera|@u0:x");
        compare(VideoStage.focusKeyFor(feeds, "", "camera|@ghost:x"),
                "camera|@u0:x", "a stale hover key is not a feed");
        compare(VideoStage.focusKeyFor([], "", ""), "");
    }

    // ── 8. Strip slots ───────────────────────────────────────────────

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
        // feed, and the strip is the only way back to the grid apart
        // from Escape.
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

    // ── 9. Shrink-wrapping a cell to its picture ─────────────────────
    //
    // The defect: a desktop screen share received on a portrait phone.
    // gridDims() gives a lone feed a 1×1 grid, so its cell is the whole
    // stage — tall and narrow — and the 16:9 picture was aspect-fit into
    // the middle of it with the rest left black INSIDE the tile. The
    // owner's iPhone 16 Pro Max photo is roughly 60% black tile.
    //
    // fitToAspect() gives that leftover back to the stage. These tests
    // are about the two properties that matter: the picture never
    // shrinks below what the cell could already show (so nothing is made
    // worse), and it is never cropped (so nothing is lost).

    // The phone case, to scale: a 390×620 stage, one 16:9 share.
    function test_a_landscape_share_on_a_portrait_stage_wraps_to_its_picture() {
        var cell = VideoStage.gridCell(0, 1, 390, 620, 12);
        compare(cell.height, 620);   // the tile used to be this tall
        var fit = VideoStage.fitToAspect(cell, 16 / 9);

        // Full width, and exactly as tall as 16:9 needs — no more.
        fuzzyCompare(fit.width, 390, 0.001);
        fuzzyCompare(fit.height, 390 * 9 / 16, 0.001);
        // Centred in the cell it was given.
        fuzzyCompare(fit.x, 0, 0.001);
        fuzzyCompare(fit.y, (620 - 390 * 9 / 16) / 2, 0.001);

        // The point of the exercise, stated as the ratio the bug report
        // is about: how much of the TILE is picture.
        //
        // The picture itself is the same size either way — this does not
        // make the share any bigger, and it is not supposed to. What
        // changes is how much black the tile wraps around it.
        var pictureArea = fit.width * fit.height;
        var before = pictureArea / (cell.width * cell.height);
        var after = pictureArea / (fit.width * fit.height);
        // ~35% — the photographed tile, about 60% of it black. If this
        // figure ever climbs on its own, the grid changed shape and this
        // test is measuring something else.
        verify(before < 0.40, "the unwrapped tile was mostly black, was "
                              + before);
        compare(after, 1);
    }

    // The other orientation, which must work just as well — a portrait
    // phone camera feed inside a wide desktop cell.
    function test_a_portrait_camera_in_a_landscape_cell_wraps_too() {
        var cell = { x: 0, y: 0, width: 800, height: 450 };
        var fit = VideoStage.fitToAspect(cell, 9 / 16);
        fuzzyCompare(fit.height, 450, 0.001);
        fuzzyCompare(fit.width, 450 * 9 / 16, 0.001);
        fuzzyCompare(fit.y, 0, 0.001);
        fuzzyCompare(fit.x, (800 - 450 * 9 / 16) / 2, 0.001);
    }

    // Never bigger than the cell, never a different shape from the
    // source, and always inside the cell it was handed. Swept across a
    // range of cells and content shapes because the two branches of the
    // comparison are easy to write the wrong way round and each one only
    // shows up on one side of it.
    function test_wrapping_never_crops_and_never_overflows() {
        var cells = [
            { x: 0,  y: 0,  width: 390, height: 620 },   // portrait phone
            { x: 30, y: 12, width: 800, height: 450 },   // 16:9 desktop
            { x: 5,  y: 7,  width: 300, height: 300 },   // square
            { x: 0,  y: 0,  width: 1200, height: 260 }   // very wide
        ];
        var aspects = [16 / 9, 4 / 3, 1, 3 / 4, 9 / 16, 21 / 9];
        for (var c = 0; c < cells.length; ++c) {
            for (var a = 0; a < aspects.length; ++a) {
                var cell = cells[c];
                var fit = VideoStage.fitToAspect(cell, aspects[a]);
                var where = "cell " + c + " aspect " + aspects[a];
                verify(fit.width <= cell.width + 0.001, "fits across: " + where);
                verify(fit.height <= cell.height + 0.001, "fits down: " + where);
                verify(fit.x >= cell.x - 0.001, "inside left: " + where);
                verify(fit.y >= cell.y - 0.001, "inside top: " + where);
                verify(fit.x + fit.width <= cell.x + cell.width + 0.001,
                       "inside right: " + where);
                verify(fit.y + fit.height <= cell.y + cell.height + 0.001,
                       "inside bottom: " + where);
                // The shape is the SOURCE's. Any drift here is a crop or
                // a stretch, which is the thing this must never do.
                fuzzyCompare(fit.width / fit.height, aspects[a], 0.001);
                // And it touches at least one pair of the cell's edges —
                // it is the LARGEST such rectangle, not merely a smaller
                // one of the right shape.
                verify(Math.abs(fit.width - cell.width) < 0.001
                       || Math.abs(fit.height - cell.height) < 0.001,
                       "is maximal: " + where);
            }
        }
    }

    // A cell whose shape already matches the picture is returned as it
    // was. This is the desktop case the fix must not disturb.
    function test_a_matching_cell_is_left_alone() {
        var cell = { x: 10, y: 20, width: 1600, height: 900 };
        var fit = VideoStage.fitToAspect(cell, 16 / 9);
        fuzzyCompare(fit.x, 10, 0.001);
        fuzzyCompare(fit.y, 20, 0.001);
        fuzzyCompare(fit.width, 1600, 0.001);
        fuzzyCompare(fit.height, 900, 0.001);
    }

    // No frame has arrived, so there is no shape to wrap to. The cell
    // must come back untouched — this is what keeps the "Starting
    // share…" placeholder centred in the full cell exactly as it was
    // before, and it is the state every tile is in for its first
    // moments.
    function test_an_unknown_aspect_leaves_the_cell_alone() {
        var cell = { x: 3, y: 4, width: 390, height: 620 };
        var zero = VideoStage.fitToAspect(cell, 0);
        compare(zero.width, 390);
        compare(zero.height, 620);
        compare(zero.x, 3);
        compare(zero.y, 4);
        // The shapes a missing sourceRect can actually produce.
        compare(VideoStage.fitToAspect(cell, undefined).height, 620);
        compare(VideoStage.fitToAspect(cell, NaN).height, 620);
        compare(VideoStage.fitToAspect(cell, -2).height, 620);
    }

    // A degenerate cell must not produce NaN geometry — a tile written
    // NaN width disappears and never comes back.
    function test_wrapping_a_collapsed_cell() {
        var flat = VideoStage.fitToAspect(
            { x: 0, y: 0, width: 0, height: 0 }, 16 / 9);
        compare(flat.width, 0);
        compare(flat.height, 0);
        var none = VideoStage.fitToAspect(null, 16 / 9);
        compare(none.width, 0);
        compare(none.height, 0);
    }

    // ── 10. "Nobody is receiving this" ───────────────────────────────
    //
    // Hoisted out of VideoFeedTile, where it was drawn over the top-left
    // corner of the live picture. The predicate is unchanged; what these
    // pin down is the part that is easy to get wrong when moving it —
    // the tri-state `transmitting` flag, and the fact that ONE room-level
    // sentence now covers both feeds.

    function warn(o) { return VideoStage.transmitWarning(o); }

    function test_no_warning_when_nothing_is_being_captured() {
        compare(warn({ screenCapturing: false, screenTransmitting: false,
                       cameraCapturing: false, cameraTransmitting: false,
                       voiceMemberCount: 1 }), "");
        compare(warn(null), "");
        compare(warn(undefined), "");
    }

    function test_no_warning_while_frames_are_going_out() {
        compare(warn({ screenCapturing: true, screenTransmitting: true,
                       cameraCapturing: true, cameraTransmitting: true,
                       voiceMemberCount: 3 }), "");
    }

    function test_alone_in_the_channel_is_not_a_fault() {
        compare(warn({ screenCapturing: true, screenTransmitting: false,
                       cameraCapturing: false, cameraTransmitting: undefined,
                       voiceMemberCount: 1 }),
                "No one else is in the channel");
        // A count of 0 is the same story — the roster has not landed yet.
        compare(warn({ screenCapturing: true, screenTransmitting: false,
                       cameraCapturing: false, cameraTransmitting: undefined,
                       voiceMemberCount: 0 }),
                "No one else is in the channel");
    }

    function test_somebody_is_there_and_is_not_getting_it() {
        compare(warn({ screenCapturing: true, screenTransmitting: false,
                       cameraCapturing: false, cameraTransmitting: undefined,
                       voiceMemberCount: 2 }),
                "Not visible to others");
    }

    // Either capture stalling is enough to raise it, and the camera must
    // be able to raise it on its own — the old per-tile badge made that
    // structurally obvious and a hoisted one can quietly lose it.
    function test_the_camera_alone_can_raise_it() {
        compare(warn({ screenCapturing: false, screenTransmitting: undefined,
                       cameraCapturing: true, cameraTransmitting: false,
                       voiceMemberCount: 2 }),
                "Not visible to others");
    }

    // Both stalled at once is still ONE sentence. Sharing a screen and a
    // camera into an empty channel used to print the identical pill on
    // both tiles.
    function test_two_stalled_feeds_are_one_banner() {
        compare(warn({ screenCapturing: true, screenTransmitting: false,
                       cameraCapturing: true, cameraTransmitting: false,
                       voiceMemberCount: 1 }),
                "No one else is in the channel");
    }

    // `transmitting` is undefined on builds whose capture controller does
    // not report it. That is "cannot tell", not "broken", and it must
    // stay silent — a permanent yellow banner accusing the transport on
    // every such build is worse than no banner at all.
    function test_a_controller_that_cannot_report_stays_silent() {
        compare(warn({ screenCapturing: true, screenTransmitting: undefined,
                       cameraCapturing: true, cameraTransmitting: undefined,
                       voiceMemberCount: 1 }), "");
        compare(warn({ screenCapturing: true, screenTransmitting: null,
                       cameraCapturing: true, cameraTransmitting: null,
                       voiceMemberCount: 1 }), "");
    }

    // ── 11. Wording ──────────────────────────────────────────────────

    function test_labels() {
        compare(VideoStage.feedLabel("Abe", VideoStage.SCREEN, false), "Abe — SCREEN SHARE");
        compare(VideoStage.feedLabel("Abe", VideoStage.CAMERA, false), "Abe — CAMERA");
        compare(VideoStage.feedLabel("Abe", VideoStage.SCREEN, true), "You — SCREEN SHARE");
        // S-7's placeholder wording, unchanged from the tiles it replaces.
        compare(VideoStage.placeholderText(VideoStage.SCREEN), "Starting share…");
        compare(VideoStage.placeholderText(VideoStage.CAMERA), "Starting camera…");
    }
}
