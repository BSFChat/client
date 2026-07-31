import QtQuick
import QtQuick.Window
import QtTest
import "../../qml/js/PlaybackMath.js" as PlaybackMath

// The testable half of the video player: time formatting, seek clamping,
// click-to-seek mapping and the fullscreen visibility restore rule.
//
// What this file does NOT cover, because a unit test cannot: whether a click
// on the control bar reaches the control rather than the surface underneath.
// That is hit-testing in a live window, and the bug it caused (clicking beside
// the scrubber collapsed fullscreen) is guarded by structure — the
// `*BarBlocker` MouseAreas in VideoPlayerCard.qml — not by this test.
TestCase {
    name: "PlaybackMath"

    function test_formatTime_data() {
        return [
            { tag: "zero",        ms: 0,        expect: "0:00" },
            { tag: "unloaded",    ms: -1,       expect: "0:00" },
            { tag: "undefined",   ms: undefined,expect: "0:00" },
            { tag: "sub-second",  ms: 400,      expect: "0:00" },
            { tag: "seconds",     ms: 9000,     expect: "0:09" },
            { tag: "pads",        ms: 65000,    expect: "1:05" },
            { tag: "minutes",     ms: 599000,   expect: "9:59" },
            { tag: "an hour",     ms: 3600000,  expect: "1:00:00" },
            { tag: "past an hour",ms: 3725000,  expect: "1:02:05" },
        ];
    }
    function test_formatTime(d) {
        compare(PlaybackMath.formatTime(d.ms), d.expect);
    }

    function test_formatFileSize_data() {
        return [
            { tag: "bytes",  n: 512,               expect: "512 B" },
            { tag: "kb",     n: 2048,              expect: "2.0 KB" },
            { tag: "mb",     n: 5 * 1024 * 1024,   expect: "5.0 MB" },
            { tag: "gb",     n: 3 * 1024 * 1024 * 1024, expect: "3.0 GB" },
        ];
    }
    function test_formatFileSize(d) {
        compare(PlaybackMath.formatFileSize(d.n), d.expect);
    }

    // Arrow-key seeking. The interesting cases are both ends: a back-seek
    // near the start must not go negative (the backend treats a negative
    // position as an error, not as zero) and a forward seek near the end
    // must land on the duration rather than past it.
    function test_clampSeek_data() {
        return [
            { tag: "forward",       pos: 10000, delta: 5000,  dur: 60000, expect: 15000 },
            { tag: "back",          pos: 10000, delta: -5000, dur: 60000, expect: 5000 },
            { tag: "back past 0",   pos: 2000,  delta: -5000, dur: 60000, expect: 0 },
            { tag: "past the end",  pos: 59000, delta: 5000,  dur: 60000, expect: 60000 },
            { tag: "at zero",       pos: 0,     delta: -5000, dur: 60000, expect: 0 },
            // Duration is unknown until the media loads; a forward seek then
            // is passed through rather than pinned to 0, which would restart
            // the video on an early key press.
            { tag: "unloaded fwd",  pos: 1000,  delta: 5000,  dur: 0,     expect: 6000 },
            { tag: "unloaded back", pos: 1000,  delta: -5000, dur: -1,    expect: 0 },
        ];
    }
    function test_clampSeek(d) {
        compare(PlaybackMath.clampSeek(d.pos, d.delta, d.dur), d.expect);
    }

    // Click-to-seek. x is in slider coordinates; the usable track starts at
    // leftPadding and runs availableWidth pixels.
    function test_seekTargetMs_data() {
        return [
            { tag: "track start",  x: 10,  lp: 10, aw: 100, dur: 60000, expect: 0 },
            { tag: "halfway",      x: 60,  lp: 10, aw: 100, dur: 60000, expect: 30000 },
            { tag: "track end",    x: 110, lp: 10, aw: 100, dur: 60000, expect: 60000 },
            // Padding must be subtracted, not ignored: at x == 0 with a
            // 10px left padding the click is before the track, not at 10%.
            { tag: "before track", x: 0,   lp: 10, aw: 100, dur: 60000, expect: 0 },
            { tag: "past track",   x: 999, lp: 10, aw: 100, dur: 60000, expect: 60000 },
            { tag: "no duration",  x: 60,  lp: 10, aw: 100, dur: 0,     expect: 0 },
            { tag: "no width",     x: 60,  lp: 10, aw: 0,   dur: 60000, expect: 0 },
        ];
    }
    function test_seekTargetMs(d) {
        compare(PlaybackMath.seekTargetMs(d.x, d.lp, d.aw, d.dur), d.expect);
    }

    // Leaving fullscreen. Uses the real Window enum values, since the whole
    // point of the function is to map them.
    function test_visibilityToRestore_data() {
        return [
            // A maximised window comes back maximised — the naive "restore to
            // Windowed" un-maximises a window the user never resized.
            { tag: "maximized", saved: Window.Maximized, expect: Window.Maximized },
            { tag: "windowed",  saved: Window.Windowed,  expect: Window.Windowed },
            // Already fullscreen before the video expanded: we changed
            // nothing, so we restore nothing.
            { tag: "fullscreen", saved: Window.FullScreen, expect: Window.FullScreen },
            // Can't honour these: the window is on screen playing a video, so
            // obeying a stale Minimized would minimise the app on Escape.
            { tag: "minimized", saved: Window.Minimized, expect: Window.AutomaticVisibility },
            { tag: "hidden",    saved: Window.Hidden,    expect: Window.AutomaticVisibility },
            { tag: "automatic", saved: Window.AutomaticVisibility,
              expect: Window.AutomaticVisibility },
            { tag: "unset",     saved: undefined,        expect: Window.AutomaticVisibility },
        ];
    }
    function test_visibilityToRestore(d) {
        compare(PlaybackMath.visibilityToRestore(
                    d.saved, Window.Hidden, Window.Minimized,
                    Window.AutomaticVisibility),
                d.expect);
    }
}
