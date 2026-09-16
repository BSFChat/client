import QtQuick
import QtTest
import "../../qml/js/LinkPreviewCache.js" as Cache

// The OpenGraph unfurl cache that LinkPreview.qml delegates share
// (qml/js/LinkPreviewCache.js).
//
// The bug this replaced was not a cache-logic bug — the old cache was correct,
// it was just declared as a `property var` on the component, so each of the
// many LinkPreview instances a session creates had its own empty copy and
// refetched every URL the app had already unfurled. That "is it actually
// shared" property is the one thing a unit test cannot show: two imports of a
// `.pragma library` are the same object by construction, and this file only
// has one. What it CAN pin is the contract the sharing is worth having for:
// the three-state lookup (absent / null / value) that decides whether a fetch
// happens at all, and the size cap that keeps a day-long session bounded.
TestCase {
    name: "LinkPreviewCache"

    function init() {
        Cache._reset();
    }

    // Absent and "known to have nothing" are different answers. Collapsing
    // them — a truthiness test rather than has()/undefined — would make every
    // repaint of a message with a dead link re-request that URL.
    function test_absent_is_distinct_from_a_cached_failure() {
        verify(!Cache.has("https://example.com/never-seen"));
        compare(Cache.lookup("https://example.com/never-seen"), undefined);

        Cache.store("https://example.com/dead", null);
        verify(Cache.has("https://example.com/dead"));
        compare(Cache.lookup("https://example.com/dead"), null);
    }

    function test_stores_and_returns_parsed_metadata() {
        var meta = { title: "A page", siteName: "example", description: "",
                     image: "", ready: true };
        Cache.store("https://example.com/a", meta);
        compare(Cache.lookup("https://example.com/a").title, "A page");
        // Distinct URLs do not collide.
        verify(!Cache.has("https://example.com/b"));
    }

    // Re-storing a URL updates it in place. If it re-queued instead, the
    // eviction list would grow past the map and the cap would stop meaning
    // anything.
    function test_restoring_a_url_updates_in_place_without_regrowing() {
        Cache.store("https://example.com/a", { title: "first", ready: true });
        Cache.store("https://example.com/a", { title: "second", ready: true });
        compare(Cache._size(), 1);
        compare(Cache.lookup("https://example.com/a").title, "second");
    }

    // The cap is the point of the rewrite for a long-lived session: a busy
    // server can put thousands of distinct links through one process.
    function test_evicts_oldest_entries_past_the_cap() {
        var n = Cache.MAX_ENTRIES;
        for (var i = 0; i < n; ++i) {
            Cache.store("https://example.com/" + i, { title: "t" + i, ready: true });
        }
        compare(Cache._size(), n);
        verify(Cache.has("https://example.com/0"));

        // One past the cap evicts the oldest, not the newest or a random one.
        Cache.store("https://example.com/overflow", { title: "new", ready: true });
        compare(Cache._size(), n);
        verify(!Cache.has("https://example.com/0"));
        verify(Cache.has("https://example.com/1"));
        verify(Cache.has("https://example.com/overflow"));
    }
}
