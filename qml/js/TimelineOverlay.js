.pragma library

// Visibility policy for the three overlays that float over the message
// timeline: the back-pagination spinner, the scroll-to-latest chevron and
// the empty state (MessageView.qml).
//
// They live out here for the same reason PlaybackMath.js does — they are
// the part of the overlay whose behaviour can be checked without a window.
// The other half of U-H1 is structural: the overlays have to be SIBLINGS of
// the ListView, not children of it, because a ListView is a Flickable and
// reparents every visual child into `contentItem`, which is the thing that
// scrolls. No pure function can prove that; tests/qml/tst_timelineoverlay.qml
// builds a real ListView and asserts it directly.
//
// See tests/qml/tst_timelineoverlay.qml.

// The back-pagination spinner. Shown only while a /messages request is
// genuinely in flight — never merely because more history exists.
function spinnerVisible(loadingHistory) {
    return loadingHistory === true;
}

// The scroll-to-latest chevron.
//
// The rule is unchanged from the original — "not at the bottom, and there
// is something to scroll to" — but it was unreachable while the button was
// inside the Flickable: anchored to the bottom of the CONTENT rather than
// the viewport, it only came into view within about 40px of the end of the
// content, which is exactly the band where `atBottom` turns true and hides
// it. So the visible symptom of the structural bug was a chevron that
// appeared to exist but could never be seen.
function chevronVisible(atBottom, rowCount) {
    return atBottom !== true && (rowCount || 0) > 0;
}

// The empty state distinguishes the situations that need different
// wording. Returns "", "no-server", "no-channels", "no-channel",
// "no-history" or "more-history".
//
// "no-channels" vs "no-channel" is one letter and two completely different
// situations. "Pick a channel / Choose one from the sidebar" is good advice
// when there is a sidebar full of them and bad advice when the server has
// none: it sends the reader to look for something that is not there, which is
// exactly what a brand-new server used to do. `channelCount` separates them.
// It may be omitted (undefined), which means "not known" and preserves the
// original answer rather than guessing zero — a caller that cannot count
// channels must not be told the server has none.
//
// An empty list is NOT always an empty channel. #notifications (2026-09-22)
// showed "It's quiet in here" over 46 real messages, because the newest
// stretch of its history was nothing but edits of one bot board and edits
// are never rows. So with no rows:
//   * while history is loading, say nothing — the spinner is the answer;
//   * with older history still on the server, say THAT, not "be the first";
//   * only when the server has nothing older is the channel really empty.
// `loadingHistory` / `hasMoreHistory` may be omitted (undefined), which
// reads as false: the original three-way answer.
function emptyStateKind(hasServer, roomId, rowCount, loadingHistory, hasMoreHistory,
                        channelCount) {
    if (!hasServer) return "no-server";
    if (!roomId || roomId.length === 0) {
        return channelCount === 0 ? "no-channels" : "no-channel";
    }
    if ((rowCount || 0) === 0) {
        if (loadingHistory === true) return "";
        if (hasMoreHistory === true) return "more-history";
        return "no-history";
    }
    return "";
}

function emptyStateVisible(hasServer, roomId, rowCount, loadingHistory, hasMoreHistory,
                           channelCount) {
    return emptyStateKind(hasServer, roomId, rowCount, loadingHistory, hasMoreHistory,
                          channelCount) !== "";
}

// The "Load older messages" button: only where the user has no other way to
// ask. A list that scrolls reaches the scroll-to-top trigger; while the
// client's own fills still have budget they will ask by themselves (showing
// the button then would flash it between two automatic fills).
function loadOlderVisible(hasMoreHistory, loadingHistory, scrollable, autoFillSpent) {
    return hasMoreHistory === true && loadingHistory !== true
        && scrollable !== true && autoFillSpent === true;
}

function emptyStateIcon(kind) {
    switch (kind) {
        case "no-server":  return "at";
        case "no-channels": return "plus";
        case "no-channel": return "hash";
        case "more-history": return "inbox";
        default:           return "send";
    }
}

function emptyStateTitle(kind) {
    switch (kind) {
        case "no-server":  return "No server selected";
        case "no-channels": return "No channels yet";
        case "no-channel": return "Pick a channel";
        case "more-history": return "Nothing recent to show";
        default:           return "It's quiet in here";
    }
}

function emptyStateBody(kind) {
    switch (kind) {
        case "no-server":
            return "Sign in to a BSFChat server to start chatting.";
        case "no-channels":
            // Says what is true and what to do, without naming a control: the
            // create affordance sits in a sidebar on the desktop and behind a
            // drawer on a phone, and a body that named either would be wrong
            // on the other shell.
            return "This server doesn't have any channels. Add the first one from the channel list.";
        case "no-channel":
            return "Choose one from the sidebar to join the conversation.";
        case "more-history":
            return "The latest activity here is edits and other changes, not new messages. Older messages are further back.";
        default:
            return "Be the first to say something.";
    }
}

// ── Bottom-anchoring a history shorter than the viewport ────────────────
//
// A ListView lays its rows out from the TOP of its viewport. A channel with
// five messages in it therefore renders five rows tucked under the header
// with the rest of the viewport — about 900pt on an iPhone 16 Pro Max — left
// as dead space between the last message and the composer. That is what the
// store screenshots caught.
//
// `positionViewAtEnd()` and MessageView's own `_jumpToEnd()` cannot fix it.
// Both move `contentY`, and when the content is shorter than the viewport
// there is no scroll range to move it through: "the end" and "the start" are
// the same position, and that position is the top. The list was not scrolled
// wrongly, it was never scrollable.
//
// So the slack is taken up by padding the flickable's top instead. Discord
// and Slack both bottom-anchor a short history this way and it is what a
// reader expects — new messages should appear next to where you type them,
// not a screen away from it.
//
// Returns the number of pixels of top padding the list needs. Zero once the
// content is taller than the viewport, which is the overwhelmingly common
// case and the one every piece of scroll bookkeeping in MessageView.qml was
// written against: the fix is inert in every state except the broken one.
//
// NOT done this way, and the reason it matters: `verticalLayoutDirection:
// ListView.BottomToTop`. That is the usual advice and it is a rewrite of
// MessageView.qml, not a property change. It renumbers the view so the
// NEWEST row is at index 0 and content grows upward, which inverts the sign
// of every scroll-position statement in that file — `contentY <
// paginationTriggerPx` as the back-pagination trigger, the `originY` term in
// `_isAtEnd()` that exists because a prepend moves originY by the height of
// everything inserted, the `originY + contentHeight - height` jump target,
// and the pagination anchor that restores position after older rows land.
// Those are the exact places whose past breakages are documented at length
// in that file. Inverting all of them to remove a gap that only appears when
// the content does not fill the screen is not a trade worth making.
//
// Guarded against the numbers being absent or nonsense rather than trusting
// them: both arrive from a live QQuickItemView, and during a room switch the
// view is mid-relayout — `contentHeight` is an ESTIMATE until the delegates
// commit their heights, and both can be read as 0 or NaN before the first
// layout runs. A NaN reaching `topMargin` poisons the flickable's extents and
// the list stops responding to scrolls entirely, so it never leaves here.
function bottomAnchorSlack(viewportHeight, contentHeight) {
    var h = Number(viewportHeight);
    var c = Number(contentHeight);
    if (!isFinite(h) || !isFinite(c)) return 0;
    if (h <= 0 || c <= 0) return 0;   // nothing laid out yet: pad nothing
    return c >= h ? 0 : h - c;
}

// Where `contentY` rests for a list carrying `bottomAnchorSlack()` as its
// top margin.
//
// This exists because `_jumpToEnd()` had a special case reading
// `contentY = originY` for un-scrollable content, and with a top margin that
// is no longer the resting position — it is one whole margin BELOW it. A
// flickable padded at the top rests at `originY - topMargin`; assigning
// `originY` scrolls the rows back up under the header and reinstates the
// exact gap the margin exists to remove. The jump is called on every count
// change, so the gap came back the moment anybody said anything.
//
// When the content is taller than the viewport the margin is 0 and this is
// the same `originY + contentHeight - height` it always was.
function restingContentY(originY, topMargin, contentHeight, viewportHeight) {
    var o = Number(originY) || 0;
    var m = Number(topMargin) || 0;
    var c = Number(contentHeight);
    var h = Number(viewportHeight);
    if (!isFinite(c) || !isFinite(h)) return o - m;
    return c <= h ? o - m : o + c - h;
}
