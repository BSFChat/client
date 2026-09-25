.pragma library

// What the connection banner says, and whether it says anything at all.
//
// ── The bug this file exists for ─────────────────────────────────────────
//
// The banner was `syncBanner`, the first row of the ColumnLayout inside
// qml/components/MessageView.qml. A real child of the timeline — which is a
// PAGE of the main column's StackLayout on both shells:
//
//     StackLayout { currentIndex: ...             // main.qml, MobileMain.qml
//                   MessageView { }               // page 0 — banner lives here
//                   VoiceRoom { }                 // page 1
//                   Item { } }                    // page 2 (mobile only)
//
// A StackLayout writes `visible` on every child each time currentIndex
// changes, so flipping to the voice room took the banner off the screen with
// the timeline. A user sitting in a call whose session expired, or whose
// connection dropped, was told nothing at all — on the one surface where a
// silent disconnect is least explicable, because the voice tiles simply stop
// moving and there is no message list to notice has gone quiet. The mobile
// shell had no banner of its own; neither did the desktop shell.
//
// (It mattered more than it looks. Until fix/reauth-recovery there was no
// re-auth path in the client at all, and the documented workaround for an
// expired session was to remove the server and add it back. The "Sign in
// again" button in this banner is still the only way out of that state from
// the main column.)
//
// The fix is to hoist the banner out of the page and into the shell, as a
// LAYOUT ROW of the main ColumnLayout — a sibling of the StackLayout and the
// VoiceDock, above both. A row and not an overlay: an item anchored over the
// column composites over live video, which is the bug that was just fixed in
// MobileMain.qml (see qml/js/MainSurface.js). A row takes 28px off the top of
// whichever surface is showing and cannot paint over any of them.
//
// This file is the banner's rules, out here rather than inline, for the same
// reason MainSurface.js is: qml/components/ConnectionBanner.qml imports the
// BSFChat module, which is compiled into the app binary, so no test target
// can instantiate it — and a rule that cannot be exercised is a rule that
// drifts. tests/qml/tst_connectionbanner.qml exercises these; the guard that
// both shells still mount the component as a row is
// theConnectionBannerIsAShellRow() in tests/test_qml_hygiene.cpp.
//
// `view` is a plain snapshot of four ServerConnection properties,
//     { connectionStatus, syncErrorMessage, needsReauth, reauthInProgress }
// built in QML so the binding's dependencies are captured there — the same
// shape and the same reason as MainSurface.js's `view`. A null/undefined view
// means "no server connected", which shows nothing.

// ServerConnection::connectionStatus. Documented at src/net/ServerConnection.h
// ("0 = disconnected, 1 = connected/syncing, 2 = reconnecting, 3 = session
// expired"), named here so the rules below read as states rather than digits.
var Disconnected = 0;
var Connected    = 1;
var Reconnecting = 2;
var Expired      = 3;

// The banner's three tones. Not colours: ConnectionBanner.qml maps these onto
// Theme.warn / Theme.danger, so a theme change does not come through here.
var None   = "none";
var Warn   = "warn";
var Danger = "danger";

// The property is `int` on the C++ side, so always a number from a real
// connection — but a plain JS object in a test, or a connection read before
// it has one, can hand us anything. Anything that is not a number is treated
// as "nothing to report", which is what the old `!== 1` test did for a null
// server.
function _status(view) {
    if (!view) return Connected;
    return (typeof view.connectionStatus === "number")
        ? view.connectionStatus : Connected;
}

function _text(v) {
    return (typeof v === "string") ? v : "";
}

// Which tone, or None for "do not show the banner at all".
//
// Note what is NOT here: the old rule was `connectionStatus !== 1`, so any
// value the C++ side might grow — a 4 for "backing off", say — produced a
// 28px strip in `color: "transparent"` with no text in it: a silent gap above
// the timeline that read as a rendering glitch. Only the three states that
// have something to say show anything.
function tone(view) {
    switch (_status(view)) {
    case Reconnecting: return Warn;
    // Expired is red rather than amber deliberately. It is a permanent state
    // until the user signs in again, not a transient one the client can
    // recover from by waiting, so it must not look like the amber one that
    // clears itself.
    case Disconnected: return Danger;
    case Expired:      return Danger;
    default:           return None;
    }
}

function isShowing(view) {
    return tone(view) !== None;
}

// The line in the strip.
function message(view) {
    switch (_status(view)) {
    case Reconnecting:
        return "Reconnecting to server…";
    case Expired:
        // The server's own words when it gave any. It does not always: the
        // 401 paths in ServerConnection set status 3 before every one of them
        // has a message to hand over, and an empty string here used to leave
        // a red strip with nothing written in it next to a "Sign in again"
        // button — which reads as a bug in the app rather than as an expired
        // session.
        return _text(view && view.syncErrorMessage)
            || "Session expired — sign in again to reconnect";
    case Disconnected:
        return "Disconnected — messages won't send until the server is reachable";
    default:
        return "";
    }
}

// The way out, and the reason the banner is worth hoisting at all. Driven by
// needsReauth rather than by `status === Expired`: the two are set together
// (every site in ServerConnection.cpp that raises needsReauth also writes
// status 3) but it is the auth state, not the connection state, that says
// whether a login round trip is the thing to offer.
function showsReauth(view) {
    return isShowing(view) && !!(view && view.needsReauth);
}

function reauthBusy(view) {
    return !!(view && view.reauthInProgress);
}

// Disabled, not hidden, while the round trip is in flight — the button is the
// only affordance in the strip and taking it away mid-sign-in would read as
// the click having destroyed it.
function reauthEnabled(view) {
    return showsReauth(view) && !reauthBusy(view);
}

function reauthLabel(view) {
    return reauthBusy(view) ? "Signing in…" : "Sign in again";
}
