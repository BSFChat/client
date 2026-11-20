.pragma library

// URLs in a message body, for link-preview cards (MessageBubble.qml).
//
// The old pattern refused "(" and ")" anywhere in a URL, so
// https://en.wikipedia.org/wiki/Foo_(bar) was cut at "Foo_" and previewed the
// wrong page. Now a URL runs to whitespace or a quote/angle bracket, and the
// tail is trimmed the way prose punctuation needs: trailing . , ! ? ; : are
// dropped, and a closing ")" or "]" is dropped only when it has no opener
// inside the URL — so "(see https://x/a_(b))" keeps "a_(b)" and loses the
// sentence's own ")".

function _count(s, ch) {
    var n = 0;
    for (var i = 0; i < s.length; ++i) if (s.charAt(i) === ch) ++n;
    return n;
}

function trimUrl(u) {
    var changed = true;
    while (changed && u.length > 0) {
        changed = false;
        var last = u.charAt(u.length - 1);
        if (".,!?;:".indexOf(last) >= 0) {
            u = u.substring(0, u.length - 1);
            changed = true;
        } else if (last === ")" && _count(u, ")") > _count(u, "(")) {
            u = u.substring(0, u.length - 1);
            changed = true;
        } else if (last === "]" && _count(u, "]") > _count(u, "[")) {
            u = u.substring(0, u.length - 1);
            changed = true;
        }
    }
    return u;
}

// Up to `max` distinct http(s) URLs in `body`, in order.
function previewUrls(body, max) {
    if (!body) return [];
    var re = /\bhttps?:\/\/[^\s<>"']+/gi;
    var matches = String(body).match(re);
    if (!matches) return [];
    var seen = {};
    var out = [];
    for (var i = 0; i < matches.length && out.length < max; ++i) {
        var u = trimUrl(matches[i]);
        if (!/^https?:\/\/[^\/]+/i.test(u) || seen[u]) continue;
        seen[u] = true;
        out.push(u);
    }
    return out;
}
