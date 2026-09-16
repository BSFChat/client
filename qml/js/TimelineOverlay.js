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

// The empty state distinguishes three situations that need different
// wording, and is shown for all three. Returns "", "no-server",
// "no-channel" or "no-history".
function emptyStateKind(hasServer, roomId, rowCount) {
    if (!hasServer) return "no-server";
    if (!roomId || roomId.length === 0) return "no-channel";
    if ((rowCount || 0) === 0) return "no-history";
    return "";
}

function emptyStateVisible(hasServer, roomId, rowCount) {
    return emptyStateKind(hasServer, roomId, rowCount) !== "";
}

function emptyStateIcon(kind) {
    switch (kind) {
        case "no-server":  return "at";
        case "no-channel": return "hash";
        default:           return "send";
    }
}

function emptyStateTitle(kind) {
    switch (kind) {
        case "no-server":  return "No server selected";
        case "no-channel": return "Pick a channel";
        default:           return "It's quiet in here";
    }
}

function emptyStateBody(kind) {
    switch (kind) {
        case "no-server":
            return "Sign in to a BSFChat server to start chatting.";
        case "no-channel":
            return "Choose one from the sidebar to join the conversation.";
        default:
            return "Be the first to say something.";
    }
}
