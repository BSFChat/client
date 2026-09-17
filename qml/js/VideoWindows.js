.pragma library

// Window policy for the voice room's video feeds: which feed is popped
// out, which window a fullscreen request belongs to, and the geometry
// every one of those windows opens at.
//
// Same reason this is a .js library and not expressions in the QML as
// qml/js/VideoStage.js, whose header explains it at length: the BSFChat
// QML module is compiled into the app binary, so no test target can
// instantiate VoiceRoom.qml, VideoPopoutWindow.qml or
// VideoFullscreenWindow.qml. Everything here is a pure function over
// plain objects and arrays, and tests/qml/tst_videowindows.qml exercises
// THIS file — the one the components import.
//
// A "feed" is the object VideoStage.mergeFeeds() produces:
//     { key, userId, kind, streamId, isSelf, started }
// and nothing here invents one or mutates one.
//
// The pop-out STATE is a plain array of records, oldest first:
//     [ { key, userId, kind, streamId, isSelf } ]
// An array rather than a map because it is fed straight to an
// Instantiator as a model, and because "oldest first" is what makes the
// cascade below deterministic. Every function returns a NEW array when
// anything changed and the SAME array when nothing did — QML only
// re-runs bindings when the reference changes, so returning a fresh copy
// on a no-op would respawn every window.

var SCREEN = "screen";
var CAMERA = "camera";

// ── Wording ──────────────────────────────────────────────────────────

// The pop-out window's title bar. Lower case after the dash on purpose:
// this is an OS window title, not the SCREEN SHARE / CAMERA pill the
// stage draws (VideoStage.feedLabel), and macOS title bars read better
// without the shouting.
function windowTitle(displayName, kind, isSelf) {
    var who = isSelf ? "You" : (displayName || "?");
    return who + " — " + (kind === SCREEN ? "screen" : "camera");
}

// The line the fullscreen overlay shows. Same identity, spelled the way
// the stage spells it, because fullscreen is a view of the stage feed.
function fullscreenLabel(displayName, kind, isSelf) {
    var who = isSelf ? "You" : (displayName || "?");
    return who + " · " + (kind === SCREEN ? "Screen share" : "Camera");
}

// ── Geometry ─────────────────────────────────────────────────────────

var MIN_WIDTH = 320;
var MIN_HEIGHT = 180;
// Half of 1920x1080. A screen share is usually 16:9 and this is the
// largest window that still leaves the app usable beside it.
var DEFAULT_WIDTH = 960;
var DEFAULT_HEIGHT = 540;

function minimumSize() {
    return { width: MIN_WIDTH, height: MIN_HEIGHT };
}

// A window of roughly DEFAULT_WIDTH x DEFAULT_HEIGHT's AREA, shaped to
// the feed's aspect ratio. Matching the area rather than the width is
// what stops a 21:9 ultrawide share opening as a 960x411 letterbox slot
// and a portrait phone camera opening 960 px tall.
//
// `aspect` is width/height. Anything non-finite or non-positive — which
// is what a feed that has not delivered a frame yet reports — falls back
// to the 16:9 default rather than producing NaN geometry, because a
// Window with NaN width does not appear at all.
function defaultSize(aspect) {
    if (!(aspect > 0) || !isFinite(aspect))
        aspect = DEFAULT_WIDTH / DEFAULT_HEIGHT;
    var area = DEFAULT_WIDTH * DEFAULT_HEIGHT;
    var w = Math.round(Math.sqrt(area * aspect));
    var h = Math.round(w / aspect);
    return clampSize(w, h);
}

// Never below the minimum, and never a fractional or NaN dimension.
function clampSize(w, h) {
    w = Math.round(w);
    h = Math.round(h);
    if (!(w >= MIN_WIDTH) || !isFinite(w)) w = MIN_WIDTH;
    if (!(h >= MIN_HEIGHT) || !isFinite(h)) h = MIN_HEIGHT;
    return { width: w, height: h };
}

// Settings stores one remembered size/position PER KIND, not per feed:
// a feed key contains a user id, so per-feed memory would accumulate a
// settings entry for every person you have ever watched. Screen shares
// and cameras want very different windows, which is the split that
// actually earns its keep.
function geometryKey(kind) {
    return kind === SCREEN ? SCREEN : CAMERA;
}

// Fold what Settings handed back into a concrete size. A saved size wins
// over the aspect-derived default — the user resized it on purpose — but
// is still clamped, so a corrupt or stale settings file cannot produce a
// 12-pixel window with no way to grab its edge.
//
// `saved` is { width, height, x, y } with -1 or missing meaning "never
// stored". Position is reported separately: a size is always usable, a
// position may be off the edge of a monitor that is no longer attached,
// and the caller decides whether to trust it.
function restoreGeometry(saved, aspect) {
    saved = saved || {};
    var size = (saved.width > 0 && saved.height > 0)
        ? clampSize(saved.width, saved.height)
        : defaultSize(aspect);
    var hasPosition = (saved.x !== undefined && saved.y !== undefined
                       && saved.x > -1 && saved.y > -1);
    return {
        width: size.width,
        height: size.height,
        x: hasPosition ? Math.round(saved.x) : 0,
        y: hasPosition ? Math.round(saved.y) : 0,
        hasPosition: hasPosition
    };
}

// A saved position is only honoured if it actually lands on a screen.
// Unplugging the second monitor otherwise reopens the pop-out at
// x: 2400 on a 1440-wide laptop, i.e. nowhere. `screens` is a list of
// { x, y, width, height } in virtual-desktop coordinates. The test is
// deliberately loose — the window's top-left inside some screen, with a
// margin so a title bar cannot end up under the menu bar — because being
// slightly off a corner is fine and being on a dead monitor is not.
function positionIsOnScreen(x, y, screens) {
    screens = screens || [];
    for (var i = 0; i < screens.length; ++i) {
        var s = screens[i];
        if (x >= s.x && y >= s.y
            && x <= s.x + s.width - MIN_WIDTH
            && y <= s.y + s.height - MIN_HEIGHT)
            return true;
    }
    return false;
}

// Two pop-outs that restored the same remembered position would land
// exactly on top of each other and read as one window. Step each one
// down-right by a title bar's worth. Bounded: past the fourth the
// cascade restarts rather than walking off the screen.
var CASCADE_STEP = 28;
var CASCADE_WRAP = 4;
function cascadeOffset(openCount) {
    openCount = Math.max(0, Math.floor(openCount || 0));
    return CASCADE_STEP * (openCount % CASCADE_WRAP);
}

// ── Pop-out bookkeeping ──────────────────────────────────────────────

function emptyState() {
    return [];
}

function indexOfKey(state, key) {
    state = state || [];
    for (var i = 0; i < state.length; ++i)
        if (state[i].key === key) return i;
    return -1;
}

function isOpen(state, key) {
    return indexOfKey(state, key) >= 0;
}

function openKeys(state) {
    state = state || [];
    var out = [];
    for (var i = 0; i < state.length; ++i) out.push(state[i].key);
    return out;
}

function count(state) {
    return (state || []).length;
}

// One pop-out per feed. Asking for a second one on the same feed RAISES
// the window that is already up — the alternative is a second window
// showing the identical picture, which is never what the click meant.
//
// Returns { state, action, key }: action is "open" when a window has to
// be created, "raise" when one exists, "none" when the feed is not
// something that can be popped out at all.
function requestPopout(state, feed) {
    state = state || [];
    if (!feed || !feed.key)
        return { state: state, action: "none", key: "" };
    if (isOpen(state, feed.key))
        return { state: state, action: "raise", key: feed.key };
    var next = state.slice();
    next.push({
        key: feed.key,
        userId: feed.userId,
        kind: feed.kind,
        streamId: feed.streamId,
        isSelf: feed.isSelf === true
    });
    return { state: next, action: "open", key: feed.key };
}

// Closing a window the user already closed is not an error — the window
// can close itself (feed ended) at the same moment the user clicks the
// tile's "close pop-out". Unchanged state keeps its identity.
function closePopout(state, key) {
    state = state || [];
    var at = indexOfKey(state, key);
    if (at < 0) return state;
    var next = state.slice();
    next.splice(at, 1);
    return next;
}

// Pop-outs whose feed no longer exists — the share stopped, the peer
// left, we left the channel. The window closes itself; this is how it
// finds out. Feeds come from VideoStage.mergeFeeds(), and an empty feed
// list (left the channel) correctly makes every open pop-out stale.
function staleKeys(state, feeds) {
    state = state || [];
    feeds = feeds || [];
    var live = {};
    for (var i = 0; i < feeds.length; ++i) live[feeds[i].key] = true;
    var out = [];
    for (var j = 0; j < state.length; ++j)
        if (!live[state[j].key]) out.push(state[j].key);
    return out;
}

// The same thing applied. Returns the SAME array when every pop-out is
// still live, so the Instantiator does not rebuild its windows on every
// feed rescan — which runs four times a second while anyone is sharing.
function pruneToFeeds(state, feeds) {
    state = state || [];
    var stale = staleKeys(state, feeds);
    if (stale.length === 0) return state;
    var next = state;
    for (var i = 0; i < stale.length; ++i) next = closePopout(next, stale[i]);
    return next;
}

// ── Which window owns a fullscreen request ───────────────────────────
//
// A feed can be in two places at once, so "make this fullscreen" has to
// name a window. The rule is that fullscreen belongs to the window the
// feed is ALREADY being watched in: if it is popped out, the pop-out
// goes fullscreen (its own toolbar button, and the F key while it has
// focus), and the voice room leaves it alone. Otherwise the voice room
// opens the fullscreen window.
//
// This is what stops the app producing two fullscreen windows of the
// same feed, on the same screen, one on top of the other.
function fullscreenOwner(state, key) {
    return isOpen(state, key) ? "popout" : "room";
}

// ── Fullscreen overlay auto-hide ─────────────────────────────────────

// The chrome (name, kind, exit button) and the mouse cursor fade out
// after this long without mouse movement, and come back on the next
// move. Long enough to find the exit button, short enough that the
// picture is not permanently wearing a label.
var OVERLAY_IDLE_MS = 2000;

function overlayVisible(lastMoveMs, nowMs) {
    if (!(lastMoveMs > 0)) return true;      // never moved: show it
    return (nowMs - lastMoveMs) < OVERLAY_IDLE_MS;
}

// ── Attaching a surface to a feed ────────────────────────────────────
//
// Three sources, wired three different ways, and there is no way around
// it (the note at the top of VideoFeedTile.qml has the long version):
// our own camera and our own screen capture mirror their controller's
// preview sink via forwardTo(), while every remote stream comes out of
// ServerConnection.videoRegistry. Three components now need that routing
// — the in-room tile, the pop-out window and the fullscreen window — and
// a third copy of it is a third place for a self-feed to end up silently
// looking for a remote stream that will never exist.
//
// The controllers are passed IN rather than named here: they are QML
// context properties that only exist on platforms with a capture path
// (src/main.cpp), and a .pragma library has no context scope to read
// them from anyway. Callers hand over whatever their `typeof` guard
// resolved, which may be null.
//
// Returns which path was taken, so a caller can log or assert on it and
// so the routing itself is testable: "local-screen", "local-camera",
// "registry", or "none" when there was nothing to attach to.
function attachFeed(feed, sink, registry, screenShareCtl, cameraCtl) {
    if (!feed || !sink) return "none";
    if (feed.isSelf === true) {
        if (feed.kind === SCREEN) {
            if (!screenShareCtl) return "none";
            screenShareCtl.forwardTo(sink);
            return "local-screen";
        }
        if (!cameraCtl) return "none";
        cameraCtl.forwardTo(sink);
        return "local-camera";
    }
    if (!registry) return "none";
    registry.attachOutput(feed.userId, feed.streamId, sink);
    return "registry";
}
