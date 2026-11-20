.pragma library

// Emphasis for search hits (SearchPopup.qml), as StyledText.
//
// The old renderer escaped the body first and then ran the term regex over
// the ESCAPED text, so a term could match inside an entity: searching "amp"
// in "a & b" turned "&amp;" into "&<b>amp</b>;", and "lt" broke every "<".
// Here the matching runs on the raw text, the hit ranges are merged, and only
// then is each piece escaped — so a <b> can never land inside an entity or a
// tag, and the body's own markup is always text.

function escapeHtml(s) {
    return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;")
                    .replace(/>/g, "&gt;").replace(/"/g, "&quot;");
}

// [start, end) ranges of every case-insensitive occurrence of any term in
// `text`, merged where they overlap or touch. Empty terms are ignored.
function matchRanges(text, terms) {
    var lower = String(text).toLowerCase();
    var ranges = [];
    for (var i = 0; i < (terms ? terms.length : 0); ++i) {
        var t = String(terms[i]).toLowerCase();
        if (t.length === 0) continue;
        var at = lower.indexOf(t);
        while (at >= 0) {
            ranges.push([at, at + t.length]);
            at = lower.indexOf(t, at + t.length);
        }
    }
    ranges.sort(function(a, b) { return a[0] - b[0] || a[1] - b[1]; });
    var merged = [];
    for (var j = 0; j < ranges.length; ++j) {
        var r = ranges[j];
        if (merged.length > 0 && r[0] <= merged[merged.length - 1][1]) {
            merged[merged.length - 1][1] = Math.max(merged[merged.length - 1][1], r[1]);
        } else {
            merged.push([r[0], r[1]]);
        }
    }
    return merged;
}

// The body as StyledText with every term in <b>, everything escaped.
function highlight(body, terms) {
    var text = String(body === undefined || body === null ? "" : body);
    var ranges = matchRanges(text, terms);
    var out = "";
    var pos = 0;
    for (var i = 0; i < ranges.length; ++i) {
        out += escapeHtml(text.substring(pos, ranges[i][0]));
        out += "<b>" + escapeHtml(text.substring(ranges[i][0], ranges[i][1])) + "</b>";
        pos = ranges[i][1];
    }
    return out + escapeHtml(text.substring(pos));
}
