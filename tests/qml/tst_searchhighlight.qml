import QtQuick
import QtTest
import "../../qml/js/SearchHighlight.js" as SearchHighlight

// Search-hit emphasis (qml/js/SearchHighlight.js, imported by SearchPopup.qml).
// The regression: highlighting ran over the escaped body, so a term could
// land inside an entity ("&<b>amp</b>;") or a tag.
TestCase {
    name: "SearchHighlight"

    function test_plain_match() {
        compare(SearchHighlight.highlight("hello world", ["world"]), "hello <b>world</b>");
    }
    function test_case_insensitive_keeps_original_case() {
        compare(SearchHighlight.highlight("Alice met ALICE", ["alice"]),
                "<b>Alice</b> met <b>ALICE</b>");
    }
    function test_never_inside_an_entity() {
        compare(SearchHighlight.highlight("a & b < c", ["amp", "lt"]), "a &amp; b &lt; c");
    }
    function test_markup_in_the_body_is_text() {
        compare(SearchHighlight.highlight("<b>x</b>", ["x"]),
                "&lt;b&gt;<b>x</b>&lt;/b&gt;");
    }
    function test_escaped_inside_the_match() {
        compare(SearchHighlight.highlight("a<b", ["a<b"]), "<b>a&lt;b</b>");
    }
    function test_overlapping_terms_merge() {
        compare(SearchHighlight.highlight("foobar", ["foo", "oba"]), "<b>fooba</b>r");
    }
    function test_empty_terms_and_body() {
        compare(SearchHighlight.highlight("x", ["", null]), "x");
        compare(SearchHighlight.highlight("", ["x"]), "");
        compare(SearchHighlight.highlight(undefined, ["x"]), "");
    }
}
