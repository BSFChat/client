import QtQuick
import QtTest
import "../../qml/js/LinkPreviewParse.js" as Parse

// The pure half of the OpenGraph unfurl (qml/js/LinkPreviewParse.js): turning
// a fetched body into preview metadata, and recognising a body that is a
// bot-challenge shim rather than a page.
//
// This code used to live on LinkPreview.qml, where it could not be tested at
// all — the component imports the BSFChat module and only the app binary has
// that. It moved out for a second reason too: a QML XMLHttpRequest callback
// keeps firing after the delegate that installed it is destroyed, and an
// unqualified name in that callback then resolves against a dead scope, which
// is what rc.7 logged as `ReferenceError: _looksLikeChallenge is not defined`.
// A `.pragma library` function cannot reach a component scope even by
// accident, so it cannot fail that way.
//
// What matters here is the decision each helper feeds: parseOg().ready and
// looksLikeChallenge() together decide whether a card renders at all, and a
// wrong answer either shows the user "Just a moment..." as a link title or
// silently drops a preview of a page that had one.
TestCase {
    name: "LinkPreviewParse"

    // The card is rendered only when `ready`, and `ready` is what LinkPreview
    // turns into a cached null ("never fetch this again") when it is false.
    function test_parses_the_opengraph_set() {
        var html = '<html><head>'
            + '<meta property="og:title" content="A Title">'
            + '<meta property="og:description" content="A description">'
            + '<meta property="og:image" content="https://cdn.example.com/i.png">'
            + '<meta property="og:site_name" content="Example">'
            + '</head></html>';
        var og = Parse.parseOg(html, "https://example.com/a");
        compare(og.title, "A Title");
        compare(og.description, "A description");
        compare(og.image, "https://cdn.example.com/i.png");
        compare(og.siteName, "Example");
        verify(og.ready);
    }

    // Both attribute orderings show up in the wild, and a site that emits the
    // content-first shape is not a site without OpenGraph.
    function test_accepts_content_before_property() {
        var html = '<meta content="Reversed" property="og:title">';
        compare(Parse.parseOg(html, "https://example.com/").title, "Reversed");
    }

    function test_falls_back_to_title_and_meta_description() {
        var html = '<html><head><title>  Plain Title  </title>'
            + '<meta name="description" content="Plain description">'
            + '</head></html>';
        var og = Parse.parseOg(html, "https://example.com/a");
        compare(og.title, "Plain Title");
        compare(og.description, "Plain description");
        verify(og.ready);
    }

    // A body with nothing preview-worthy is not an error, but it must not be
    // ready: a card with no title, no description and no image is an empty
    // rectangle in the timeline.
    function test_a_body_with_nothing_useful_is_not_ready() {
        var og = Parse.parseOg("<html><body>just text</body></html>",
                               "https://example.com/a");
        verify(!og.ready);
        compare(og.title, "");
    }

    function test_decodes_the_entities_that_show_up_in_titles() {
        compare(Parse.decodeEntities("Tom &amp; Jerry"), "Tom & Jerry");
        compare(Parse.decodeEntities("&quot;quoted&quot;"), '"quoted"');
        compare(Parse.decodeEntities("it&#39;s &apos;fine&apos;"), "it's 'fine'");
        compare(Parse.decodeEntities("&lt;tag&gt;"), "<tag>");
    }

    // og:image is routinely relative or protocol-relative. An unresolved one
    // is a broken Image in the card, not a missing one.
    function test_absolutizes_image_urls_against_the_fetched_url() {
        compare(Parse.absolutize("https://cdn.example.com/i.png", "https://example.com/a/b"),
                "https://cdn.example.com/i.png");
        compare(Parse.absolutize("//cdn.example.com/i.png", "https://example.com/a/b"),
                "https://cdn.example.com/i.png");
        compare(Parse.absolutize("/i.png", "https://example.com/a/b"),
                "https://example.com/i.png");
        compare(Parse.absolutize("i.png", "https://example.com/a/b"),
                "https://example.com/a/i.png");
    }

    function test_relative_image_in_a_parsed_body_comes_out_absolute() {
        var html = '<meta property="og:title" content="T">'
            + '<meta property="og:image" content="/img/hero.png">';
        var og = Parse.parseOg(html, "https://example.com/post/1");
        compare(og.image, "https://example.com/img/hero.png");
    }

    // The point of the challenge check. A Cloudflare interstitial is valid
    // HTML with a perfectly good <title>, so without this it unfurls — and
    // the user gets a link card reading "Just a moment..." where the page
    // title should be. A desktop client's own fetch cannot pass these; the
    // real fix is a server-side unfurler.
    function test_detects_challenge_pages_by_title() {
        var titles = ["Just a moment...", "Please wait...",
                      "Verify you are human", "Attention Required! | Cloudflare",
                      "Access denied", "Verification", "Checking your browser"];
        for (var i = 0; i < titles.length; ++i) {
            verify(Parse.looksLikeChallenge("<title>" + titles[i] + "</title>"),
                   "not detected: " + titles[i]);
        }
        // Case is not part of the signal.
        verify(Parse.looksLikeChallenge("<TITLE>JUST A MOMENT...</TITLE>"));
    }

    // Vendors also swap the title for something innocuous, so the body
    // markers have to stand on their own.
    function test_detects_challenge_pages_by_body_markers() {
        verify(Parse.looksLikeChallenge('<title>Example</title>'
            + '<script src="/cdn-cgi/challenge-platform/h/b/orchestrate/jsch/v1"></script>'));
        verify(Parse.looksLikeChallenge('<div id="cf-chl-page"></div>'));
        verify(Parse.looksLikeChallenge('<script src="https://client.perimeterx.net/px.js"></script>'));
        verify(Parse.looksLikeChallenge('<script>var _pxAppId = "PX123";</script>'));
        verify(Parse.looksLikeChallenge('<script src="https://js.datadome.co/tags.js"></script>'));
    }

    // The other half of that trade: matching is broad on purpose, but a
    // false positive costs the user a preview they should have had, so an
    // ordinary page must come through clean — including one that merely
    // talks about the words involved.
    function test_ordinary_pages_are_not_challenges() {
        verify(!Parse.looksLikeChallenge(
            '<html><head><title>How we ship releases</title>'
            + '<meta property="og:title" content="How we ship releases">'
            + '</head><body><p>Hello.</p></body></html>'));
        verify(!Parse.looksLikeChallenge(''));
    }

    // A challenge body and an empty body reach LinkPreview the same way —
    // as `parsed === null`, which is what it caches and what hides the card.
    // Checking the pair here is what makes that equivalence explicit.
    function test_a_challenge_body_would_otherwise_have_parsed() {
        var challenge = '<html><head><title>Just a moment...</title>'
            + '<meta property="og:title" content="Definitely A Real Page">'
            + '</head></html>';
        verify(Parse.parseOg(challenge, "https://example.com/a").ready);
        verify(Parse.looksLikeChallenge(challenge));
    }
}
