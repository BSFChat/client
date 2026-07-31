.pragma library

// Pure helpers for the inline / fullscreen video player (VideoPlayerCard.qml).
//
// These live outside the component for one reason: they are the only parts of
// the player whose behaviour can be checked without a window, a decoder and a
// human with a mouse. Everything else in that file is hit-testing and focus,
// which only a running GUI can prove. See tests/qml/tst_playbackmath.qml.

// mm:ss (or h:mm:ss past the hour) for a millisecond position/duration.
// A not-yet-loaded MediaPlayer reports 0 or -1; both read as "0:00" rather
// than "-1:59".
function formatTime(ms) {
    if (!ms || ms < 0) return "0:00";
    var s = Math.floor(ms / 1000);
    var m = Math.floor(s / 60);
    var sec = s % 60;
    if (m >= 60) {
        var h = Math.floor(m / 60);
        var min = m % 60;
        return h + ":" + (min < 10 ? "0" : "") + min
                 + ":" + (sec < 10 ? "0" : "") + sec;
    }
    return m + ":" + (sec < 10 ? "0" : "") + sec;
}

function formatFileSize(bytes) {
    if (bytes < 1024) return bytes + " B";
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB";
    if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + " MB";
    return (bytes / (1024 * 1024 * 1024)).toFixed(1) + " GB";
}

// Relative seek (arrow keys). Clamped to the media, never negative.
// `durationMs` is 0 or -1 until the media loads — in that state we still allow
// forward seeks (the backend clamps) rather than pinning everything to 0,
// which would make an early key press silently restart the video.
function clampSeek(positionMs, deltaMs, durationMs) {
    var target = (positionMs || 0) + deltaMs;
    if (target < 0) target = 0;
    if (durationMs > 0 && target > durationMs) target = durationMs;
    return Math.round(target);
}

// Where a click on the seek track lands, in milliseconds.
//
// Qt Quick Controls' Slider only moves its handle for a *drag*: a stationary
// click on the groove emits nothing at all (QQuickSlider::handleRelease bails
// unless keepMouseGrab is set). Click-to-seek is therefore ours to implement,
// and this is the part of it worth testing — an off-by-one on the padding maps
// every click a few pixels early and nobody notices until they try to hit a
// specific frame.
//
// `x` is in the slider's own coordinates; the usable track runs from
// `leftPadding` for `availableWidth` pixels. Out-of-range x clamps to the ends.
function seekTargetMs(x, leftPadding, availableWidth, durationMs) {
    if (!(availableWidth > 0) || !(durationMs > 0)) return 0;
    var frac = (x - leftPadding) / availableWidth;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    return Math.round(frac * durationMs);
}

// Which Window.Visibility to put the window back to when the video leaves
// fullscreen.
//
// Callers pass Qt's enum values so this stays free of QtQuick.Window: it is
// `visibilityToRestore(saved, Window.Hidden, Window.Minimized,
// Window.AutomaticVisibility)`.
//
// The rules, all of which are bugs if you skip them:
//   - Maximized must come back Maximized. Assuming Windowed (the obvious
//     implementation) un-maximises a window the user never asked to resize.
//   - FullScreen comes back FullScreen: the window was ALREADY fullscreen
//     before the video expanded (voice-room fullscreen, or the user's own
//     choice), so we changed nothing and must restore nothing.
//   - Hidden / Minimized can't be honoured — the window is visibly on screen
//     playing a video, so a saved value that stale would minimise the whole
//     app the moment the user pressed Escape. Fall back to automatic.
//   - undefined/null (no window at save time) is the same fallback.
function visibilityToRestore(saved, hidden, minimized, automatic) {
    if (saved === undefined || saved === null) return automatic;
    if (saved === hidden || saved === minimized) return automatic;
    return saved;
}
