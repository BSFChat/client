.pragma library
.import "PlaybackMath.js" as PlaybackMath

// Presentation logic for the auto-updater UI (UpdatePanel.qml, which is
// embedded both in the modal UpdateDialog.qml and inline in the
// ClientSettings "Updates" pane).
//
// Why a separate library rather than inline expressions in the QML: every
// function here is a total function of the Updater's published properties
// and nothing else — no Item, no window, no network — so it is the one
// half of "make the update dialog look right" that a test can actually
// hold. See tests/qml/tst_updateformat.qml. The other half (does the
// panel size to its content, is the progress group visually one unit) is
// layout against a live scene graph and is NOT covered there; do not read
// a green test as proof the panel looks right.
//
// The state numbers mirror Updater::State in src/core/Updater.h. QML has
// no access to the Q_ENUM by name (the Updater is a context property, not
// a registered type), so the enum is duplicated here — deliberately, in
// one place, instead of the bare integer literals that were previously
// scattered through three .qml files.

var Idle            = 0;
var Checking        = 1;
var UpToDate        = 2;
var UpdateAvailable = 3;
var Downloading     = 4;
var ReadyToApply    = 5;
var Applying        = 6;
var Failed          = 7;
var AheadOfChannel  = 8;

// ── Predicates ──────────────────────────────────────────────────────
//
// These decide what the panel shows; they do not decide what the updater
// does. Nothing here calls back into the Updater.

// The updater is working and will move on by itself. Nothing for the
// user to press, and "Check now" must be disabled so a second request
// can't be stacked on the first.
function isBusy(state) {
    return state === Checking || state === Downloading || state === Applying;
}

// The updater has stopped and is waiting on a human. These are the
// states worth interrupting someone for.
function awaitsUser(state) {
    return state === UpdateAvailable || state === ReadyToApply
        || state === Failed;
}

// Settled: the updater has finished and there is nothing to do. Distinct
// from awaitsUser — AheadOfChannel and UpToDate are both "nothing to do"
// but only one of them means "you have the newest build".
function isSettled(state) {
    return state === Idle || state === UpToDate || state === AheadOfChannel;
}

// A manual check is allowed. Mirrors the guard the Updater applies
// itself (checkNow() is a no-op mid-flight); this is only about greying
// the button so the click doesn't silently do nothing.
function canCheck(state) {
    return !isBusy(state);
}

// Release notes belong to a specific pending release. Once the download
// starts, the progress group is the thing worth reading, and after apply
// the notes describe a build you are already installing.
function showsNotes(state) {
    return state === UpdateAvailable || state === ReadyToApply;
}

// The bar. Indeterminate during Applying (the installer reports nothing
// back to us) and during a download whose Content-Length was absent.
function showsProgress(state) {
    return state === Downloading || state === Applying;
}

// The byte counter under the bar. Only Downloading has bytes; showing
// "0 B of 0 B" while the installer runs is worse than showing nothing.
function showsByteCount(state) {
    return state === Downloading;
}

// Severity bucket → the QML side maps this to a Theme colour. Kept as a
// string so this file stays free of Theme (and therefore testable
// without the BSFChat module).
function kind(state) {
    switch (state) {
    case UpToDate:        return "ok";
    case ReadyToApply:    return "ok";
    case UpdateAvailable: return "accent";
    case Downloading:     return "accent";
    case Applying:        return "accent";
    case AheadOfChannel:  return "warn";
    case Failed:          return "danger";
    default:              return "neutral";   // Idle, Checking
    }
}

// Bundled SVG name for the status chip (see qml/icons/).
function iconName(state) {
    switch (state) {
    case UpToDate:     return "check";
    case ReadyToApply: return "check";
    case Failed:       return "x";
    case Checking:     return "signal";
    default:           return "bolt";
    }
}

// ── Version strings ─────────────────────────────────────────────────

// One spelling of a version, everywhere. `currentVersion` arrives from
// the build define without a "v" while `availableVersion` is a git tag
// that usually has one, which is how the old dialog produced
// "You're on 0.0.44-rc.2 — latest is v0.0.44-rc.3." in a single sentence.
function vtag(version) {
    if (version === undefined || version === null) return "";
    var v = String(version).trim();
    if (v.length === 0) return "";
    if (v.charAt(0) === "v" || v.charAt(0) === "V") v = v.substring(1);
    if (v.length === 0) return "";
    return "v" + v;
}

function channelLabel(channel) {
    return channel === "beta" ? "beta" : "stable";
}

// ── Headline + supporting line ──────────────────────────────────────
//
// title() is the one bold line; detail() is the sentence directly under
// it. They are a pair: detail() never repeats the title, and the panel
// renders them as a single block so the status line stops floating in
// the middle of the dialog with nothing to attach to.

function title(state, availableVersion) {
    var v = vtag(availableVersion);
    switch (state) {
    case Checking:        return "Checking for updates…";
    case UpToDate:        return "You're up to date";
    case UpdateAvailable: return v ? "BSFChat " + v + " is available"
                                   : "An update is available";
    case Downloading:     return v ? "Downloading " + v : "Downloading update";
    case ReadyToApply:    return v ? v + " is ready to install"
                                   : "Update ready to install";
    case Applying:        return "Installing update…";
    case Failed:          return "Update failed";
    case AheadOfChannel:  return "Ahead of your update channel";
    default:              return "Updates";
    }
}

// `ctx` is a plain object: { currentVersion, availableVersion,
// latestStableVersion, channel, osName }. Every field is optional; a
// missing field degrades the sentence rather than printing "undefined".
function detail(state, ctx) {
    ctx = ctx || {};
    var cur = vtag(ctx.currentVersion);
    var chan = channelLabel(ctx.channel);
    var on = cur ? "You're on " + cur : "";

    switch (state) {
    case Checking:
        return "Looking at the release feed for a newer build.";
    case UpToDate:
        return cur
            ? cur + " is the newest build on the " + chan + " channel."
            : "No newer build on the " + chan + " channel.";
    case UpdateAvailable:
        return on ? on + " · " + chan + " channel." : "";
    case Downloading:
        return "This runs in the background — you can keep using BSFChat.";
    case ReadyToApply:
        return applyHint(ctx.osName);
    case Applying:
        return "Handing the download to the installer.";
    case Failed:
        // The verbatim message gets its own banner underneath; repeating
        // it here would print it twice.
        return "The last attempt didn't finish.";
    case AheadOfChannel:
        var stable = vtag(ctx.latestStableVersion);
        var head = cur ? cur + " is newer than " : "This build is newer than ";
        head += stable ? "the latest " + chan + " release (" + stable + ")."
                       : "anything on the " + chan + " channel.";
        return head + " Nothing to install.";
    default:
        // Idle. Also reached after a check that learned nothing — an
        // empty release list, or a release with no asset for this
        // platform — so it must not claim anything either way. Names the
        // running build because Idle is the state the Updates pane sits
        // in most of the time and "what am I on" is the question it is
        // there to answer.
        return on
            ? on + ". Check to see whether a newer build has been published."
            : "Check to see whether a newer build has been published.";
    }
}

// ── The apply action, which is not the same thing on every platform ──
//
// Updater::applyUpdate() runs a different path per OS and only two of
// the three replace the running app:
//   macOS   — mounts the .dmg, dittos the bundle, relaunches.
//   Windows — starts the installer, which relaunches.
//   Linux   — opens the release page in a browser and quits; the
//             package manager (or the user) does the rest.
// Labelling the Linux case "Restart to install" would be a straight
// falsehood, so the label and its hint are computed, not hard-coded.

function applyLabel(osName) {
    return osName === "linux" ? "Open the release page" : "Restart to install";
}

function applyHint(osName) {
    return osName === "linux"
        ? "BSFChat will close and open the release page — Linux builds are "
          + "installed through your package manager."
        : "BSFChat restarts to finish installing. Nothing else to do.";
}

// ── Download progress ───────────────────────────────────────────────
//
// The byte counter uses PlaybackMath.formatFileSize so there is exactly
// one size formatter in the QML tree, not a second one that rounds
// differently three dialogs away.

// A Content-Length of 0 or -1 means the server didn't say — the bar is
// indeterminate and there is no percentage to print.
function progressKnown(total) {
    return typeof total === "number" && total > 0;
}

function progressFraction(downloaded, total) {
    if (!progressKnown(total)) return 0;
    var d = (typeof downloaded === "number" && downloaded > 0) ? downloaded : 0;
    if (d > total) d = total;
    return d / total;
}

function percentText(downloaded, total) {
    if (!progressKnown(total)) return "";
    return Math.floor(progressFraction(downloaded, total) * 100) + "%";
}

function byteProgress(downloaded, total) {
    var d = (typeof downloaded === "number" && downloaded > 0) ? downloaded : 0;
    if (!progressKnown(total)) return PlaybackMath.formatFileSize(d);
    if (d > total) d = total;
    return PlaybackMath.formatFileSize(d) + " of "
         + PlaybackMath.formatFileSize(total);
}

// ── Release notes ───────────────────────────────────────────────────

// Hard cap on what we will lay out. A release body is whatever someone
// typed into GitHub; it has no length limit and the panel does.
var NOTES_LIMIT = 6000;

// Markdown → PLAIN TEXT. Note the direction: this never produces markup.
//
// The release body is remote text fetched from a GitHub release. It is
// rendered with textFormat: PlainText, which is why this function strips
// syntax instead of translating it: there is no HTML, no rich-text
// document, no <img>/<a> resolution and therefore no way for a release
// body to make the client load a remote resource or open a link. The
// alternative (TextEdit.MarkdownText, or MarkdownParser::toHtml) hands
// the string to QTextDocument, which does resolve embedded resources —
// fine for our own message pipeline, not for a third-party string that
// arrives over the wire ahead of any user action.
//
// Tags are stripped rather than escaped so a body containing raw HTML
// reads as its text instead of as angle-bracket noise.
function plainNotes(markdown) {
    if (markdown === undefined || markdown === null) return "";
    var s = String(markdown).replace(/\r\n/g, "\n").replace(/\r/g, "\n");

    // Fence markers go; the code inside stays, unindented as it was.
    s = s.replace(/^[ \t]*(```|~~~)[^\n]*$/gm, "");

    // Autolinks keep their URL — unwrap them before the tag strip below
    // would eat the whole thing brackets and all.
    s = s.replace(/<((?:https?|mailto):[^>\s]{1,400})>/g, "$1");

    // Any HTML the author pasted in. Done before link/emphasis handling
    // so an attribute value can't be mistaken for markdown syntax.
    s = s.replace(/<!--[\s\S]*?-->/g, "");
    s = s.replace(/<\/?[a-zA-Z][^<>\n]{0,400}>/g, "");

    // Images first (they are links with a leading '!'), then links.
    // Both collapse to their label; the URL is dropped rather than
    // printed, because the panel offers a "Release notes" button that
    // opens the real page.
    s = s.replace(/!\[([^\]\n]*)\]\([^)\n]*\)/g, "$1");
    s = s.replace(/\[([^\]\n]*)\]\([^)\n]*\)/g, "$1");
    // Autolinks: <https://…> already lost their brackets above, but the
    // reference form [text][ref] has not.
    s = s.replace(/\[([^\]\n]*)\]\[[^\]\n]*\]/g, "$1");

    var lines = s.split("\n");
    for (var i = 0; i < lines.length; ++i) {
        var line = lines[i];
        // Thematic break / setext underline — pure noise once the
        // heading above it is already plain.
        if (/^[ \t]*([-*_=])\1{2,}[ \t]*$/.test(line)) { lines[i] = ""; continue; }
        line = line.replace(/^[ \t]*#{1,6}[ \t]+/, "");   // ATX heading
        line = line.replace(/^[ \t]*>[ \t]?/, "");        // block quote
        line = line.replace(/^[ \t]*[-*+][ \t]+/, "• ");  // bullet
        lines[i] = line;
    }
    s = lines.join("\n");

    // Emphasis. `**` before `*` so the pair isn't half-eaten. Single `_`
    // is left alone on purpose: release notes are full of snake_case
    // identifiers and stripping it mangles them.
    s = s.replace(/\*\*([^\n]+?)\*\*/g, "$1");
    s = s.replace(/(^|[^\w])__([^\n]+?)__(?!\w)/g, "$1$2");
    s = s.replace(/(^|[^\w*])\*([^*\n]+)\*(?!\*)/g, "$1$2");
    s = s.replace(/~~([^\n]+?)~~/g, "$1");
    s = s.replace(/`([^`\n]+)`/g, "$1");

    // Collapse the blank runs the stripping just created.
    s = s.replace(/[ \t]+$/gm, "");
    s = s.replace(/\n{3,}/g, "\n\n");
    s = s.replace(/^\n+/, "").replace(/\n+$/, "");

    if (s.length > NOTES_LIMIT) s = s.substring(0, NOTES_LIMIT) + "…";
    return s;
}

// Whether there is anything worth giving a box to. An empty release
// body is common (tag-only releases) and an empty bordered rectangle
// looks like a rendering failure.
function hasNotes(markdown) {
    return plainNotes(markdown).length > 0;
}
