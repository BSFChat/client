import QtQuick
import QtTest
import "../../qml/js/WindowGeometry.js" as WindowGeometry

// Clamping the window size restored from settings.
//
// The defect: qml/main.qml restored the stored size with no screen check at
// all, while checking the stored POSITION against the screen two lines
// later. A size saved on a large display and restored on a small one opened
// a window taller than the panel, putting the bottom of both side panels
// and the composer off-screen — unrecoverable from inside the app, because
// every control that could resize the window is itself below the fold.
//
// Reported from a Steam Deck: 1280x800, "I can only see the top half of
// each" side panel.
TestCase {
    id: tc
    name: "WindowGeometry"

    // Steam Deck, restoring a size saved on an ordinary desktop.
    function test_a_a_taller_stored_height_is_cut_to_the_screen() {
        var g = WindowGeometry.clampToScreen(1920, 1080, 1280, 800, 800, 500);
        compare(g.height, 800, "height must not exceed the screen");
        compare(g.width, 1280, "width must not exceed the screen");
    }

    function test_b_a_size_that_already_fits_is_untouched() {
        var g = WindowGeometry.clampToScreen(1100, 700, 1280, 800, 800, 500);
        compare(g.width, 1100);
        compare(g.height, 700);
    }

    // The minimum wins over the clamp. A window smaller than its own
    // minimum is not a state Qt holds, so returning one would be a lie.
    function test_c_the_minimum_beats_a_smaller_screen() {
        var g = WindowGeometry.clampToScreen(1000, 600, 640, 400, 800, 500);
        compare(g.width, 800, "minimumWidth must survive the clamp");
        compare(g.height, 500, "minimumHeight must survive the clamp");
    }

    // 0 means "no Screen attached yet", not "a zero-pixel display".
    function test_d_an_unknown_screen_does_not_collapse_the_window() {
        var g = WindowGeometry.clampToScreen(1100, 700, 0, 0, 800, 500);
        compare(g.width, 1100);
        compare(g.height, 700);
    }

    // No stored geometry: the caller must fall through to its own defaults
    // rather than be handed a zero-size window.
    function test_e_absent_or_junk_geometry_returns_null() {
        compare(WindowGeometry.clampToScreen(0, 0, 1280, 800, 800, 500), null);
        compare(WindowGeometry.clampToScreen(-1, 700, 1280, 800, 800, 500), null);
        compare(WindowGeometry.clampToScreen(undefined, undefined,
                                             1280, 800, 800, 500), null);
        compare(WindowGeometry.clampToScreen(NaN, NaN, 1280, 800, 800, 500), null);
    }

    function test_f_the_result_is_whole_pixels() {
        var g = WindowGeometry.clampToScreen(1100.6, 700.4, 1280, 800, 800, 500);
        compare(g.width, 1101);
        compare(g.height, 700);
    }
}
