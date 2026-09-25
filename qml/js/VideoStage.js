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
// appears. It is what "oldest first" means in the feed order below, and
// it is preserved across merges so a feed's age doesn't reset when some
// other feed comes or goes.

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

// Feed order: screen shares first, then cameras; oldest first within a
// kind, key as the final tie-break so the order is total and stable.
// This is the order of the grid AND of the strip, so a feed does not
// jump around when the view switches between them.
//
// Note what this ordering does NOT do any more: it no longer decides
// what is on the stage. Screens sorting first is a reading order, not a
// promotion.
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

// ── Expansion ────────────────────────────────────────────────────────
//
// The grid is the default for ANY mix of feeds. NOTHING is promoted to
// the stage on its own. A screen share used to be — `hasScreen()` picked
// the newest one and the stage became a single panel — and the effect
// was that one person sharing a desktop swept every face in the call
// into a row of thumbnails without anybody asking for it. Expansion is
// now a deliberate click and only a click.
//
// So `selectedKey` means "the feed the user expanded", and "" means "the
// grid". It survives a membership change only while its own feed does: a
// feed that stops while expanded drops the view back to the grid rather
// than handing the stage to some other feed nobody picked.
function expandedKey(feeds, selectedKey) {
    if (selectedKey && hasKey(feeds, selectedKey)) return selectedKey;
    return "";
}

// A click on a tile. Clicking the expanded feed — either filling the
// stage or as its "on stage" chip down in the strip — collapses back to
// the grid; clicking any other feed expands that one instead. Escape is
// the same thing as collapsing, and VoiceRoom writes "" for it directly.
function toggleExpanded(feeds, selectedKey, key) {
    var current = expandedKey(feeds, selectedKey);
    if (!key || !hasKey(feeds, key)) return current;
    return current === key ? "" : key;
}

// What the stage does:
//   "empty"     nothing to show (the classic participant grid takes over)
//   "grid"      every feed at once, each as large as the stage allows —
//               the default, for screens and cameras alike
//   "expanded"  the feed the user clicked fills the stage and every
//               other feed sits in the bottom strip
//
// A lone feed is always "grid": a 1×1 grid already fills the stage, and
// a strip holding one thumbnail of the only feed is a chip that can do
// nothing.
function stageMode(feeds, selectedKey) {
    feeds = feeds || [];
    if (feeds.length === 0) return "empty";
    if (feeds.length > 1 && expandedKey(feeds, selectedKey)) return "expanded";
    return "grid";
}

// Keys currently on the stage, in stage order.
function stageKeys(feeds, selectedKey) {
    feeds = feeds || [];
    var mode = stageMode(feeds, selectedKey);
    var out = [];
    if (mode === "empty") return out;
    if (mode === "expanded") return [expandedKey(feeds, selectedKey)];
    for (var i = 0; i < feeds.length; ++i) out.push(feeds[i].key);
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

// ── Grid shape ───────────────────────────────────────────────────────
//
// "Each as big as possible" is NOT "as few cells as possible". A feed is
// aspect-fit inside its cell, so a cell of the wrong shape is mostly
// letterbox: two feeds on a stage wider than 16:9 are biggest side by
// side, and on a stage narrower than 16:9 they are biggest stacked — the
// same two feeds, the same two cells, a different answer. The shape is
// therefore measured rather than looked up in a table.
//
// For every column count 1…n (rows follows as ceil(n/cols)) we fit a
// TILE_ASPECT rectangle into one cell and keep the largest. Ties go to
// the wider grid: on a stage that is exactly 16:9 the choice between 3×2
// and 2×3 is a genuine dead heat, and faces read left to right.
//
// On a 16:9-or-wider stage this yields exactly what the owner asked for
// — 1 fills, 2 side by side, 3–4 as 2×2, 5–6 as 3×2, 7–9 as 3×3, and
// beyond that the columns grow again so every tile shrinks. On a
// portrait stage it yields the transpose, down to a single column when
// the stage is tall and narrow enough for that to win. It genuinely
// does win there, and a 2-column grid in a phone-shaped stage would be
// half letterbox.
//
// Gaps are deliberately not part of the choice: they are single-digit
// pixels against a stage of several hundred and would never flip an
// answer, and leaving them out keeps the shape a pure function of the
// stage's aspect, which is what makes it testable.
var TILE_ASPECT = 16 / 9;

function _fittedArea(cellW, cellH, tileAspect) {
    if (!(cellW > 0) || !(cellH > 0)) return 0;
    var w = Math.min(cellW, cellH * tileAspect);
    return w * (w / tileAspect);
}

function gridDims(n, stageAspect, tileAspect) {
    n = Math.max(0, Math.floor(n || 0));
    if (n <= 0) return { cols: 0, rows: 0 };
    if (n === 1) return { cols: 1, rows: 1 };
    if (!(stageAspect > 0)) stageAspect = TILE_ASPECT;
    if (!(tileAspect > 0)) tileAspect = TILE_ASPECT;

    var bestCols = 1, bestRows = n, bestArea = -1;
    for (var cols = 1; cols <= n; ++cols) {
        var rows = Math.ceil(n / cols);
        // Unit height: only the stage's ratio matters to the choice.
        var area = _fittedArea(stageAspect / cols, 1 / rows, tileAspect);
        var better = area > bestArea + 1e-9;
        var tied = !better && area > bestArea - 1e-9;
        if (better || tied) {
            bestCols = cols;
            bestRows = rows;
            if (better) bestArea = area;
        }
    }
    return { cols: bestCols, rows: bestRows };
}

// Geometry of the index-th cell. A short final row is centred, so three
// feeds read as two over one rather than two over one-hard-left.
function gridCell(index, n, width, height, gap) {
    var d = gridDims(n, (width > 0 && height > 0) ? width / height : 0);
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

// ── Shrink-wrapping a cell to its picture ────────────────────────────
//
// gridCell() hands every feed a cell of the grid's shape, and the tile
// then aspect-FITS the picture inside it. Whatever is left over is black
// — and it is black INSIDE the tile, inside its border, under its name
// pill and its fullscreen button. On a portrait phone showing one
// landscape screen share that leftover is most of the tile: the owner
// photographed an iPhone 16 Pro Max where a desktop share was a thin
// strip of picture across the middle of a tall black rectangle, roughly
// 60% of the tile drawing nothing. A 1×1 "grid" on a portrait stage is
// simply the wrong shape for 16:9 content, and gridDims() cannot know
// that: it assumes TILE_ASPECT for every feed because it is choosing a
// SHAPE before anything has told it what the pictures actually are.
//
// So the leftover is given back. The tile shrinks to the picture's own
// aspect and centres in the cell it was given, and the space it gives up
// becomes ordinary stage background instead of tile interior. The grid
// itself is untouched — same cells, same positions, same choice of
// columns — which is what keeps this from being a phone fix that
// rearranges the desktop. All it moves is the tile's own edge.
//
// This is NOT cropping and must not become cropping. A screen share is
// the one feed where every pixel may carry text, and filling a portrait
// phone with a landscape desktop means throwing away the sides of it.
// The remaining background above and below a wrapped 16:9 tile on a
// portrait phone is inherent to showing the whole picture; fullscreen is
// the answer to wanting more, and there is a control for it.
//
// `contentAspect` of 0 means "no frame has arrived yet, we do not know".
// The cell is then returned unchanged, so the not-yet-live placeholder
// still centres in the full cell exactly as it did before, and the tile
// resizes once when the first frame lands.
function fitToAspect(cell, contentAspect) {
    if (!cell) return { x: 0, y: 0, width: 0, height: 0 };
    if (!(contentAspect > 0) || !(cell.width > 0) || !(cell.height > 0))
        return cell;
    var w = cell.width;
    var h = cell.height;
    if (contentAspect > w / h) h = w / contentAspect;
    else                       w = h * contentAspect;
    return {
        x: cell.x + (cell.width - w) / 2,
        y: cell.y + (cell.height - h) / 2,
        width: w,
        height: h
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

// Left/right arrows walk WHICH FEED IS EXPANDED, in feed order.
// Clamped, not wrapped: holding an arrow should come to rest at an end
// rather than cycle.
//
// In the grid they do nothing. Every feed is already on screen there, so
// there is nothing to walk between, and making an arrow key expand
// something would be a large unasked-for change of view from a key the
// user may well have meant for the message list.
function moveSelection(feeds, selectedKey, delta) {
    feeds = feeds || [];
    var current = expandedKey(feeds, selectedKey);
    if (!current) return "";
    var index = indexOfKey(feeds, current);
    if (index < 0) return "";
    var next = index + Math.floor(delta || 0);
    if (next < 0) next = 0;
    if (next > feeds.length - 1) next = feeds.length - 1;
    return feeds[next].key;
}

// Which feed a pointerless action — the header's full-screen button, or
// `F` — acts on. The expanded feed when there is one; otherwise the tile
// the pointer is over, because in a grid "this one" can only mean the
// one being looked at; otherwise the first feed, so the key always does
// something rather than nothing.
function focusKeyFor(feeds, selectedKey, hoveredKey) {
    feeds = feeds || [];
    var expanded = expandedKey(feeds, selectedKey);
    if (expanded) return expanded;
    if (hoveredKey && hasKey(feeds, hoveredKey)) return hoveredKey;
    return feeds.length > 0 ? feeds[0].key : "";
}

// The pill drawn over a feed. Kept here so the wording is exercised by
// the tests rather than only by eye.
function feedLabel(displayName, kind, isSelf) {
    var who = isSelf ? "You" : (displayName || "?");
    return who + " — " + (kind === SCREEN ? "SCREEN SHARE" : "CAMERA");
}

// ── "Nobody is receiving this" ───────────────────────────────────────
//
// We are capturing, and no peer channel is carrying the frames. Either
// there is nobody there to carry them to, or there is and the transport
// is not doing it — a real difference, and the wording is the only place
// the user is told which.
//
// THIS USED TO BE DRAWN ON THE PICTURE. It was a Rectangle inside
// VideoFeedTile anchored top-left with a margin, so on the owner's
// iPhone the yellow "No one else is in the channel" pill sat across the
// top-left corner of his own live camera preview — over his face, in the
// screenshot meant for the App Store. It is the same family of mistake
// as the one qml/js/ConnectionBanner.js was written for, and that file
// already states the rule this now follows: "A row and not an overlay:
// an item anchored over the column composites over live video." So
// VoiceRoom mounts this as a row above the stage, the stage is that much
// shorter, and nothing is painted over any feed.
//
// Hoisting it also deduplicates it. The badge was per-tile, so a user
// sharing their screen AND their camera into an empty channel got the
// identical pill twice, once on each tile — while the fact it states,
// "there is nobody else here", is a property of the ROOM and was never
// per-feed at all.
//
// `view` is a plain snapshot built in QML so the binding's dependencies
// are captured there, the same shape and the same reason as
// MainSurface.js's and ConnectionBanner.js's:
//
//     { screenCapturing, screenTransmitting,
//       cameraCapturing, cameraTransmitting, voiceMemberCount }
//
// `=== false` on the transmitting flags is load-bearing and is carried
// over verbatim from the old badge: not every build's capture controller
// exposes `transmitting`, and on those it is undefined. Testing it
// truthily would turn "this build cannot tell you" into a permanent
// accusation that the transport is broken.
function transmitWarning(view) {
    if (!view) return "";
    var stalled =
        (view.screenCapturing === true && view.screenTransmitting === false)
        || (view.cameraCapturing === true && view.cameraTransmitting === false);
    if (!stalled) return "";
    // Alone in the channel is not a fault, it is an empty room. Only
    // claim a transmission problem when somebody should be seeing this.
    return view.voiceMemberCount > 1
        ? "Not visible to others"
        : "No one else is in the channel";
}

// Placeholder wording while a stream is announced but not yet live
// (S-7). Our own preview is never in this state: the local sink is
// already producing frames by the time `active` flips.
function placeholderText(kind) {
    return kind === SCREEN ? "Starting share…" : "Starting camera…";
}
