.pragma library

// Clamping for the window size restored from settings (qml/main.qml's
// Component.onCompleted).
//
// WHY THIS EXISTS
//
// The restore used to be `if (w > 0 && h > 0) { width = w; height = h; }`.
// The stored POSITION was sanity-checked against the screen; the stored
// SIZE was not checked against anything. A size written on one display and
// restored on a smaller one opens a window taller than the screen, and the
// part below the fold is unreachable: the bottom of the channel list, the
// bottom of the member list and the composer are simply not on the panel.
//
// It is not a hypothetical. The owner hit it on a Steam Deck, whose screen
// is 1280x800 — a height restored from any ordinary desktop session is
// taller than that — and reported seeing only the top half of both side
// panels. Nothing inside the app can recover from it, because every
// control that would resize the window is itself off-screen.
//
// The side panels are NOT missing a scroll view, which is the obvious
// first guess: their ListViews already carry `clip: true`, a ThemedScrollBar
// and `Layout.fillHeight`. They were filling a window that extended past
// the display.
//
// Clamp order matters. Screen first, minimum second: a minimum larger than
// the screen must still win, because a window smaller than its own minimum
// is not a state Qt will hold, and we would rather overflow by the few
// pixels the minimum demands than return a size the window immediately
// overrides. On any real display the minimum (800x500) is the smaller of
// the two and the clamp is what takes effect.
function clampToScreen(w, h, availW, availH, minW, minH) {
    var cw = Number(w);
    var ch = Number(h);
    if (!isFinite(cw) || !isFinite(ch) || cw <= 0 || ch <= 0) return null;

    // A screen dimension of 0 means "unknown" (no Screen attached yet),
    // not "a zero-pixel display" — leave that axis alone rather than
    // clamping the window out of existence.
    var aw = Number(availW);
    var ah = Number(availH);
    if (isFinite(aw) && aw > 0) cw = Math.min(cw, aw);
    if (isFinite(ah) && ah > 0) ch = Math.min(ch, ah);

    var mw = Number(minW);
    var mh = Number(minH);
    if (isFinite(mw) && mw > 0) cw = Math.max(cw, mw);
    if (isFinite(mh) && mh > 0) ch = Math.max(ch, mh);

    return { width: Math.round(cw), height: Math.round(ch) };
}
