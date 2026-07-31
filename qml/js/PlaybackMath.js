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

// ── Card geometry ─────────────────────────────────────────────────────
//
// The size of an inline video card is decided by the event, not by the media:
// the m.video info block carries w/h, so the row can reserve the exact box
// before a single byte is fetched. Getting this from the media instead is what
// makes a channel appear to scroll: the card renders small, the real size
// arrives, contentHeight grows underneath the viewport and the newest message
// slides out of view.
//
// These are shared with MessageBubble.qml, which needs the same number BEFORE
// the card exists in order to reserve the row. Two copies of this arithmetic
// would drift, and the drift would be invisible until a row jumped.
//
// The scale is deliberately identical to the inline-image path in
// MessageBubble.qml, including the 1.0 clamp — a 160x120 clip renders at
// 160x120 rather than being blown up to the cap.

function cardMaxWidth(isMobile)  { return isMobile ? 260 : 400; }
function cardMaxHeight(isMobile) { return isMobile ? 200 : 300; }

function _cardScale(mediaW, mediaH, isMobile) {
    return Math.min(cardMaxWidth(isMobile) / mediaW,
                    cardMaxHeight(isMobile) / mediaH, 1.0);
}

// A server that omits info.w/h leaves us guessing, and the guess has to match
// the image path's: a 4:3 box at the cap width. Anything else means the two
// media types reserve differently for the same missing information.
function cardBoxWidth(mediaW, mediaH, isMobile) {
    if (mediaW > 0 && mediaH > 0)
        return Math.round(mediaW * _cardScale(mediaW, mediaH, isMobile));
    return cardMaxWidth(isMobile);
}

function cardBoxHeight(mediaW, mediaH, isMobile) {
    if (mediaW > 0 && mediaH > 0)
        return Math.round(mediaH * _cardScale(mediaW, mediaH, isMobile));
    return Math.round(cardMaxWidth(isMobile) * 3 / 4);
}

// The filename/size caption under the card. Given a fixed height rather than
// measured from the text so that the row's total is knowable before the card
// is instantiated; VideoPlayerCard pins the row to this number so the two
// cannot disagree. 1.5x the font is enough headroom for ascenders and
// descenders at every size in the theme.
function captionRowHeight(hasCaption, fontPx) {
    return hasCaption ? Math.round(fontPx * 1.5) : 0;
}

// What MessageBubble.qml should reserve for the whole card: the picture box,
// plus the caption row and the spacing above it when there is one.
function cardHeight(mediaW, mediaH, isMobile, hasCaption, fontPx, spacing) {
    var h = cardBoxHeight(mediaW, mediaH, isMobile);
    var caption = captionRowHeight(hasCaption, fontPx);
    if (caption > 0) h += caption + spacing;
    return h;
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
