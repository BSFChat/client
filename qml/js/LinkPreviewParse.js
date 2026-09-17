.pragma library

// Pure text helpers for the OpenGraph unfurl in components/LinkPreview.qml:
// turning a fetched HTML body into preview metadata, and recognising a body
// that is a bot-challenge shim rather than a page.
//
// They live out here for two reasons.
//
// The first is scope. LinkPreview drives XMLHttpRequest, and a QML XHR
// callback is a plain JS closure: it keeps firing after the delegate that
// created it has been destroyed, and message delegates are destroyed and
// recreated constantly. Once the delegate is gone, every unqualified name in
// that closure resolves against a dead QML scope — which is what put
//     ReferenceError: _looksLikeChallenge is not defined
//     Invalid write to global property "_failed"
// in the v0.0.44-rc.7 field logs. LinkPreview now cancels its request on
// destruction, and functions reached through a `.pragma library` import
// cannot depend on a component scope at all, so this half of the work is
// immune to that failure by construction.
//
// The second is that the parsing is the part worth testing and the only part
// that can be: the component itself needs the BSFChat module, which only the
// app binary has, while this file is importable from tests/qml
// (tst_linkpreviewparse.qml).
//
// Nothing here touches the network, a component or the cache.

// Regex-extract the OpenGraph set plus sensible fallbacks. This is
// deliberately not a real HTML parser — it just picks out the
// handful of <meta> tags we care about. Most modern sites emit
// them in the same `<meta property="og:*" content="...">` shape
// with quote variation that the regex tolerates.
function parseOg(html, urlStr) {
    function pick(prop) {
        // Property-first or content-first ordering both seen in
        // the wild. Match either.
        var re = new RegExp(
            '<meta[^>]+(?:property|name)=["\']' + prop
            + '["\'][^>]*content=["\']([^"\']+)["\']', 'i');
        var m = re.exec(html);
        if (m) return decodeEntities(m[1]);
        re = new RegExp(
            '<meta[^>]+content=["\']([^"\']+)["\'][^>]*(?:property|name)=["\']'
            + prop + '["\']', 'i');
        m = re.exec(html);
        return m ? decodeEntities(m[1]) : "";
    }
    var out = {};
    out.title = pick("og:title");
    out.description = pick("og:description");
    out.image = pick("og:image");
    out.siteName = pick("og:site_name");

    // Fallbacks: <title> tag + <meta name="description">.
    if (!out.title) {
        var tm = /<title[^>]*>([^<]+)<\/title>/i.exec(html);
        if (tm) out.title = decodeEntities(tm[1].trim());
    }
    if (!out.description) out.description = pick("description");

    // Relative or protocol-relative image URLs — turn into
    // absolute against the requested url.
    if (out.image) out.image = absolutize(out.image, urlStr);

    out.ready = !!(out.title || out.description || out.image);
    return out;
}

// Minimal HTML entity decode — covers the common ampersand /
// quote / apostrophe / less / greater entities that show up in
// og:title values.
function decodeEntities(s) {
    return s
        .replace(/&amp;/g, "&")
        .replace(/&quot;/g, '"')
        .replace(/&#39;/g, "'")
        .replace(/&apos;/g, "'")
        .replace(/&lt;/g, "<")
        .replace(/&gt;/g, ">");
}

// Returns true when the fetched HTML is obviously a bot-challenge
// shim (Cloudflare, PerimeterX, DataDome, generic "please wait")
// rather than real page content. Matching is deliberately broad —
// a false positive at worst means no preview, which is fine since
// the alternative is showing "Please wait for verification" as a
// title. A properly-signed server-side fetcher won't hit these.
function looksLikeChallenge(html) {
    var t = /<title[^>]*>([^<]+)<\/title>/i.exec(html);
    if (t) {
        var title = t[1].toLowerCase();
        if (title.indexOf("just a moment") >= 0) return true;
        if (title.indexOf("please wait") >= 0) return true;
        if (title.indexOf("verify you are human") >= 0) return true;
        if (title.indexOf("attention required") >= 0) return true;
        if (title.indexOf("access denied") >= 0) return true;
        if (title.indexOf("verification") >= 0) return true;
        if (title.indexOf("checking your browser") >= 0) return true;
    }
    // Body-level giveaways that survive even when the title is
    // generic or missing.
    if (/cf-chl-page|cf_chl_opt|_cf_chl_|challenge-platform/i.test(html)) return true;
    if (/perimeterx\.net|pxhd-captcha|_pxAppId/i.test(html)) return true;
    if (/datadome\.co|ddkey|dd_protected/i.test(html)) return true;
    return false;
}

function absolutize(maybeRel, baseUrl) {
    if (/^https?:\/\//i.test(maybeRel)) return maybeRel;
    if (maybeRel.indexOf("//") === 0) {
        var scheme = baseUrl.match(/^(https?:)/i);
        return (scheme ? scheme[1] : "https:") + maybeRel;
    }
    var base = baseUrl.match(/^(https?:\/\/[^\/]+)/i);
    if (!base) return maybeRel;
    if (maybeRel.indexOf("/") === 0) return base[1] + maybeRel;
    // Strip trailing filename from the base path before appending.
    var path = baseUrl.replace(base[1], "").replace(/[^\/]*$/, "");
    return base[1] + path + maybeRel;
}
