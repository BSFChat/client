.pragma library

// Layout arithmetic and selection policy for the voice room's video
// stage (VoiceRoom.qml → VideoFeedTile.qml).
//
// Why a .js library and not just expressions in the QML: the BSFChat QML
// module is compiled into the app binary, so no test target can
// instantiate VoiceRoom.qml (same constraint that produced
// qml/js/TimelineOverlay.js and qml/js/PlaybackMath.js). Everything here
// is a pure function over plain objects, and tests/qml/tst_videostage.qml
// exercises THIS file — the one VoiceRoom.qml imports.
//
// A "feed" is one video surface: a remote screen share, a remote camera,
// our own camera, or our own screen share. The shape is
//
//     { key, userId, kind, streamId, isSelf, started }
//
// and it deliberately carries NO liveness. Liveness changes several times
// a second while a stream settles, and a feed list that changed identity
// on every one of those would hand the Repeater a fresh model object and
// make it destroy and rebuild every delegate — the S-13 flicker that
// VoiceRoom._peersSharing was reworked to avoid. `live` is computed
// inside the delegate off a tick instead; the list only ever changes when
// MEMBERSHIP changes.
//
// `started` is a monotonically increasing tick stamped when a feed first
// appears. It is what "most recently started" means below, and it is
// preserved across merges so a feed's age doesn't reset when some other
// feed comes or goes.

var SCREEN = "screen";
var CAMERA = "camera";

// Stream ids mirror voice::VideoStreamId (src/voice/video/VideoCodec.h):
// Screen = 0, Camera = 1. They are the second argument to
// VideoStreamRegistry::attachOutput / hasLiveVideo / streamStopped.
function streamIdFor(kind) {
    return kind === SCREEN ? 0 : 1;
}

// One user can have a camera AND a screen share up at once, so the key
// has to carry both halves.
function feedKey(userId, kind) {
    return kind + "|" + userId;
}

function _kindRank(kind) {
    return kind === SCREEN ? 0 : 1;
}

// Strip order: screen shares first, then cameras; oldest first within a
// kind, key as the final tie-break so the order is total and stable. The
// stage grid uses the same order, so a feed does not jump around when it
// moves between stage and strip.
function _compareFeeds(a, b) {
    var ka = _kindRank(a.kind);
    var kb = _kindRank(b.kind);
    if (ka !== kb) return ka - kb;
    if (a.started !== b.started) return a.started - b.started;
    if (a.key < b.key) return -1;
    if (a.key > b.key) return 1;
    return 0;
}

// Fold freshly observed feeds into the previous list, keeping the
// `started` stamp of anything that was already there. `raw` entries need
// only { userId, kind, isSelf }.
function mergeFeeds(previous, raw, tick) {
    previous = previous || [];
    raw = raw || [];
    var byKey = {};
    for (var i = 0; i < previous.length; ++i)
        byKey[previous[i].key] = previous[i];

    var out = [];
    var seen = {};
    for (var j = 0; j < raw.length; ++j) {
        var r = raw[j];
        if (!r || !r.userId || !r.kind) continue;
        var key = feedKey(r.userId, r.kind);
        if (seen[key]) continue;   // a peer listed twice is still one feed
        seen[key] = true;
        var old = byKey[key];
        out.push({
            key: key,
            userId: r.userId,
            kind: r.kind,
            streamId: streamIdFor(r.kind),
            isSelf: r.isSelf === true,
            started: old ? old.started : tick
        });
    }
    out.sort(_compareFeeds);
    return out;
}

// Membership comparison — the guard that keeps the stored list's
// identity (and therefore the delegates) when nothing actually changed.
function sameFeeds(a, b) {
    a = a || [];
    b = b || [];
    if (a.length !== b.length) return false;
    for (var i = 0; i < a.length; ++i)
        if (a[i].key !== b[i].key) return false;
    return true;
}

function indexOfKey(feeds, key) {
    feeds = feeds || [];
    for (var i = 0; i < feeds.length; ++i)
        if (feeds[i].key === key) return i;
    return -1;
}

function hasKey(feeds, key) {
    return indexOfKey(feeds, key) >= 0;
}

function hasScreen(feeds) {
    feeds = feeds || [];
    for (var i = 0; i < feeds.length; ++i)
        if (feeds[i].kind === SCREEN) return true;
    return false;
}

// Auto-selection: the most recently started screen share, and only if
// there is no screen share at all, the most recently started camera.
// A screen share is the thing people are being asked to look at.
function defaultSelection(feeds) {
    feeds = feeds || [];
    var best = null;
    var i, f;
    for (i = 0; i < feeds.length; ++i) {
        f = feeds[i];
        if (f.kind !== SCREEN) continue;
        if (!best || f.started >= best.started) best = f;
    }
    if (best) return best.key;
    for (i = 0; i < feeds.length; ++i) {
        f = feeds[i];
        if (f.kind !== CAMERA) continue;
        if (!best || f.started >= best.started) best = f;
    }
    return best ? best.key : "";
}

// The effective selection. An explicit pick survives only while its feed
// does; when that feed stops we fall back to auto rather than leaving the
// stage blank. Passing "" is how Escape returns to auto.
function resolveSelection(feeds, selectedKey) {
    if (selectedKey && hasKey(feeds, selectedKey)) return selectedKey;
    return defaultSelection(feeds);
}

// What the stage does:
//   "empty"  nothing to show (the classic participant grid takes over)
//   "single" one feed, filling the stage
//   "grid"   cameras only — everybody side by side, including us
//   "focus"  a screen share is up, so one feed fills the stage and the
//            rest are pickable from the strip
function stageMode(feeds, selectedKey) {
    feeds = feeds || [];
    if (feeds.length === 0) return "empty";
    if (feeds.length === 1) return "single";
    return hasScreen(feeds) ? "focus" : "grid";
}

// Keys currently on the stage, in stage order.
function stageKeys(feeds, selectedKey) {
    feeds = feeds || [];
    var mode = stageMode(feeds, selectedKey);
    var out = [];
    var i;
    if (mode === "empty") return out;
    if (mode === "grid") {
        for (i = 0; i < feeds.length; ++i) out.push(feeds[i].key);
        return out;
    }
    if (mode === "single") return [feeds[0].key];
    var sel = resolveSelection(feeds, selectedKey);
    if (sel) out.push(sel);
    return out;
}

// Position of `key` on the stage, or -1 when it belongs in the strip.
function stageIndexOf(feeds, selectedKey, key) {
    var keys = stageKeys(feeds, selectedKey);
    for (var i = 0; i < keys.length; ++i)
        if (keys[i] === key) return i;
    return -1;
}

function stageCount(feeds, selectedKey) {
    return stageKeys(feeds, selectedKey).length;
}

// Grid shape for n tiles. 2 is two columns rather than a 2×2 with two
// holes; 3–4 is 2×2; 5–6 is 3×2; past that the column count stays at 3
// and rows are added, so the tiles shrink instead of the grid getting
// unreadably wide.
function gridDims(n) {
    n = Math.max(0, Math.floor(n || 0));
    if (n <= 0) return { cols: 0, rows: 0 };
    if (n === 1) return { cols: 1, rows: 1 };
    if (n === 2) return { cols: 2, rows: 1 };
    if (n <= 4)  return { cols: 2, rows: 2 };
    if (n <= 6)  return { cols: 3, rows: 2 };
    return { cols: 3, rows: Math.ceil(n / 3) };
}

// Geometry of the index-th cell. A short final row is centred, so three
// feeds read as two over one rather than two over one-hard-left.
function gridCell(index, n, width, height, gap) {
    var d = gridDims(n);
    if (d.cols <= 0 || d.rows <= 0)
        return { x: 0, y: 0, width: 0, height: 0 };
    gap = gap || 0;
    var cw = (width - (d.cols - 1) * gap) / d.cols;
    var ch = (height - (d.rows - 1) * gap) / d.rows;
    var col = index % d.cols;
    var row = Math.floor(index / d.cols);
    var inThisRow = Math.min(d.cols, n - row * d.cols);
    var rowWidth = inThisRow * cw + (inThisRow - 1) * gap;
    return {
        x: (width - rowWidth) / 2 + col * (cw + gap),
        y: row * (ch + gap),
        width: cw,
        height: ch
    };
}

// Strip slots. The strip never scrolls: thumbnails shrink to fit so that
// every feed stays clickable without a hidden overflow, which is the
// whole point of the strip.
function stripSlot(index, n, width, gap, maxWidth) {
    if (n <= 0) return { x: 0, width: 0 };
    gap = gap || 0;
    var available = Math.max(0, width - (n - 1) * gap);
    var w = Math.min(maxWidth || available, available / n);
    if (!(w > 0)) w = 0;
    var total = n * w + (n - 1) * gap;
    return { x: (width - total) / 2 + index * (w + gap), width: w };
}

// Left/right arrow keys walk the strip order. Clamped, not wrapped:
// holding an arrow should come to rest at an end rather than cycle.
function moveSelection(feeds, selectedKey, delta) {
    feeds = feeds || [];
    if (feeds.length === 0) return "";
    var current = resolveSelection(feeds, selectedKey);
    var index = indexOfKey(feeds, current);
    if (index < 0) index = 0;
    var next = index + Math.floor(delta || 0);
    if (next < 0) next = 0;
    if (next > feeds.length - 1) next = feeds.length - 1;
    return feeds[next].key;
}

// The pill drawn over a feed. Kept here so the wording is exercised by
// the tests rather than only by eye.
function feedLabel(displayName, kind, isSelf) {
    var who = isSelf ? "You" : (displayName || "?");
    return who + " — " + (kind === SCREEN ? "SCREEN SHARE" : "CAMERA");
}

// Placeholder wording while a stream is announced but not yet live
// (S-7). Our own preview is never in this state: the local sink is
// already producing frames by the time `active` flips.
function placeholderText(kind) {
    return kind === SCREEN ? "Starting share…" : "Starting camera…";
}
