.pragma library

// Process-wide OpenGraph unfurl cache, shared by every LinkPreview delegate.
//
// `.pragma library` is the point: without it each import gets its own copy of
// this scope. LinkPreview previously kept the cache in a `property var _cache`
// on the component, so it was per-INSTANCE — and message delegates are
// destroyed and recreated constantly (scrolling out of the pool, a sidebar
// rebuild, a room switch). Every recreation refetched a URL the app had
// already unfurled, hit the network again from the user's own IP, and reflowed
// the timeline when the card resolved a second time. A library scope outlives
// all of them.
//
// Stored value is either a parsed metadata object or `null`, and `null` is
// meaningful: it records "this URL has nothing preview-worthy, or the fetch
// failed", which is what stops a dead link being retried on every repaint.
// `undefined` — i.e. absent — is the only thing that triggers a fetch, so the
// three states must not be collapsed.

// Cap on distinct URLs held. Each entry is a handful of short strings, so this
// is a memory ceiling for a long-lived session rather than a tuning knob: a
// busy server can put thousands of distinct links through one session, and an
// unbounded object would keep every one of them (and its description text)
// alive until quit.
var MAX_ENTRIES = 200;

var _entries = ({});   // url -> { value: <parsed|null> }
var _order = [];       // insertion order, oldest first — eviction queue

// Undefined when the URL has never been fetched; otherwise the parsed
// metadata, or null for a known-unusable URL. Callers must distinguish
// `undefined` from `null` with `!==`, not truthiness.
function lookup(url) {
    var e = _entries[url];
    return e === undefined ? undefined : e.value;
}

// True once a URL has an outcome recorded, of either kind.
function has(url) {
    return _entries[url] !== undefined;
}

// Record an outcome. Re-storing a known URL refreshes its value in place and
// does NOT re-queue it for eviction — entries are evicted oldest-first by when
// they were first learned, which keeps the queue the same length as the map.
function store(url, value) {
    if (_entries[url] !== undefined) {
        _entries[url].value = value;
        return;
    }
    _entries[url] = { value: value };
    _order.push(url);
    while (_order.length > MAX_ENTRIES) {
        var oldest = _order.shift();
        delete _entries[oldest];
    }
}

// Test seam. Nothing in the app calls this: the cache is correct to keep for
// the life of the process, and a "clear" button would only cause refetches.
function _reset() {
    _entries = ({});
    _order = [];
}

function _size() {
    return _order.length;
}
