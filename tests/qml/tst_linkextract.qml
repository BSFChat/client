import QtQuick
import QtTest
import "../../qml/js/LinkExtract.js" as LinkExtract

// Link-preview URL detection (qml/js/LinkExtract.js, imported by
// MessageBubble.qml). The regression: the old pattern stopped at "(".
TestCase {
    name: "LinkExtract"

    function test_balanced_parentheses_are_kept() {
        compare(LinkExtract.previewUrls("see https://en.wikipedia.org/wiki/Foo_(bar) now", 2),
                ["https://en.wikipedia.org/wiki/Foo_(bar)"]);
    }
    function test_sentence_parenthesis_is_dropped() {
        compare(LinkExtract.previewUrls("(see https://x.example/a_(b))", 2),
                ["https://x.example/a_(b)"]);
        compare(LinkExtract.previewUrls("(https://x.example/page)", 2),
                ["https://x.example/page"]);
    }
    function test_trailing_punctuation_is_dropped() {
        compare(LinkExtract.previewUrls("Go to https://x.example/a. Then https://y.example/b!", 2),
                ["https://x.example/a", "https://y.example/b"]);
    }
    function test_dedupe_and_cap() {
        compare(LinkExtract.previewUrls("https://a.example https://a.example https://b.example https://c.example", 2),
                ["https://a.example", "https://b.example"]);
    }
    function test_nothing() {
        compare(LinkExtract.previewUrls("", 2), []);
        compare(LinkExtract.previewUrls("no links", 2), []);
        compare(LinkExtract.previewUrls("https://", 2), []);
    }
}
