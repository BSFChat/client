import QtQuick
import QtTest
import "../../qml/js/UpdateFormat.js" as UF

// The testable half of the updater UI: the state → (title, body, icon,
// severity) mapping, the predicates that decide which controls exist in
// each state, the byte/percentage formatting, and the markdown → plain
// text reduction applied to the remote release body.
//
// What this file does NOT cover, because a unit test cannot: whether the
// panel sizes to its content, whether the progress bar reads as one unit
// with its label and counter, or whether anything is legible. That is
// layout against a live scene graph. A green run here means the strings
// and the branch logic are right, not that the dialog looks right.
TestCase {
    name: "UpdateFormat"

    // Mirrors Updater::State (src/core/Updater.h). If this list and the
    // C++ enum ever disagree, everything below is testing fiction — the
    // enum is documented as append-only for exactly that reason.
    readonly property var allStates: [
        UF.Idle, UF.Checking, UF.UpToDate, UF.UpdateAvailable,
        UF.Downloading, UF.ReadyToApply, UF.Applying, UF.Failed,
        UF.AheadOfChannel
    ]

    function test_stateEnumIsContiguousFromZero() {
        for (var i = 0; i < allStates.length; ++i)
            compare(allStates[i], i, "state " + i + " renumbered");
    }

    // ── Predicates ──────────────────────────────────────────────────

    function test_busyAwaitsAndSettledPartitionEveryState() {
        // Every state is in exactly one bucket. This is the property
        // worth asserting: a state that falls in none leaves the panel
        // with no controls at all, and one that falls in two gets a
        // "Check now" button that fires mid-download.
        for (var i = 0; i < allStates.length; ++i) {
            var s = allStates[i];
            var n = (UF.isBusy(s) ? 1 : 0) + (UF.awaitsUser(s) ? 1 : 0)
                  + (UF.isSettled(s) ? 1 : 0);
            compare(n, 1, "state " + s + " is in " + n + " buckets");
        }
    }

    function test_canCheckIsTheInverseOfBusy() {
        for (var i = 0; i < allStates.length; ++i)
            compare(UF.canCheck(allStates[i]), !UF.isBusy(allStates[i]));
    }

    function test_aheadOfChannelIsNotUpToDate() {
        // The whole reason state 8 exists. It must not borrow UpToDate's
        // wording, its icon or its severity.
        verify(UF.title(UF.AheadOfChannel, "") !== UF.title(UF.UpToDate, ""));
        verify(UF.kind(UF.AheadOfChannel) !== UF.kind(UF.UpToDate));
        var d = UF.detail(UF.AheadOfChannel,
                          { currentVersion: "0.0.44-rc.2",
                            latestStableVersion: "v0.0.44",
                            channel: "stable" });
        verify(d.indexOf("up to date") === -1);
        verify(d.indexOf("v0.0.44-rc.2") >= 0);
        verify(d.indexOf("(v0.0.44)") >= 0);
    }

    function test_notesOnlyWhereThereIsAPendingRelease() {
        verify(UF.showsNotes(UF.UpdateAvailable));
        verify(UF.showsNotes(UF.ReadyToApply));
        verify(!UF.showsNotes(UF.Downloading));
        verify(!UF.showsNotes(UF.UpToDate));
        verify(!UF.showsNotes(UF.Failed));
    }

    function test_progressOnlyWhileSomethingIsMoving() {
        verify(UF.showsProgress(UF.Downloading));
        verify(UF.showsProgress(UF.Applying));
        verify(!UF.showsProgress(UF.UpdateAvailable));
        verify(!UF.showsProgress(UF.ReadyToApply));
    }

    function test_byteCountOnlyWhereThereAreBytes() {
        // Applying gets the bar (indeterminate — the installer reports
        // nothing back) but not the counter, which would sit at whatever
        // the finished download left behind.
        verify(UF.showsByteCount(UF.Downloading));
        verify(!UF.showsByteCount(UF.Applying));
        verify(!UF.showsByteCount(UF.ReadyToApply));
        for (var i = 0; i < allStates.length; ++i) {
            if (UF.showsByteCount(allStates[i]))
                verify(UF.showsProgress(allStates[i]),
                       "state " + allStates[i] + " counts bytes with no bar");
        }
    }

    function test_idleNamesTheRunningBuild() {
        // Idle is where the Updates pane sits most of the time, and it
        // is also what a check that learned nothing falls back to — so
        // it answers "what am I on" without claiming to be current.
        var d = UF.detail(UF.Idle, { currentVersion: "0.0.44" });
        verify(d.indexOf("v0.0.44") >= 0);
        verify(d.indexOf("up to date") === -1);
        // …and degrades cleanly with no version to name.
        verify(UF.detail(UF.Idle, {}).length > 0);
        verify(UF.detail(UF.Idle, {}).indexOf("You're on") === -1);
    }

    // ── Version spelling ────────────────────────────────────────────

    function test_vtag_data() {
        return [
            { tag: "bare",       v: "0.0.44",       expect: "v0.0.44" },
            { tag: "prefixed",   v: "v0.0.44",      expect: "v0.0.44" },
            { tag: "capital V",  v: "V0.0.44",      expect: "v0.0.44" },
            { tag: "prerelease", v: "0.0.44-rc.2",  expect: "v0.0.44-rc.2" },
            { tag: "padded",     v: "  v1.2.3  ",   expect: "v1.2.3" },
            { tag: "empty",      v: "",             expect: "" },
            { tag: "just v",     v: "v",            expect: "" },
            { tag: "undefined",  v: undefined,      expect: "" },
            { tag: "null",       v: null,           expect: "" },
        ];
    }
    function test_vtag(d) { compare(UF.vtag(d.v), d.expect); }

    function test_oneSentenceNeverMixesSpellings() {
        // The screenshot bug: "You're on 0.0.44-rc.2 — latest is
        // v0.0.44-rc.3." Same sentence, two conventions.
        var t = UF.title(UF.UpdateAvailable, "v0.0.44-rc.3");
        var d = UF.detail(UF.UpdateAvailable,
                          { currentVersion: "0.0.44-rc.2", channel: "beta" });
        verify(t.indexOf("v0.0.44-rc.3") >= 0);
        verify(d.indexOf("v0.0.44-rc.2") >= 0);
        // No bare "on 0.0" — every version carries the v.
        verify(d.indexOf(" 0.0.44") === -1);
    }

    // ── Titles and bodies ───────────────────────────────────────────

    function test_everyStateHasATitleAndNoUndefinedLeaksThrough() {
        for (var i = 0; i < allStates.length; ++i) {
            var s = allStates[i];
            var t = UF.title(s, "v1.0.0");
            var d = UF.detail(s, { currentVersion: "1.0.0", channel: "stable",
                                   latestStableVersion: "v0.9.0",
                                   osName: "osx" });
            verify(t.length > 0, "state " + s + " has no title");
            verify(t.indexOf("undefined") === -1, "state " + s + " title: " + t);
            verify(d.indexOf("undefined") === -1, "state " + s + " detail: " + d);
            verify(d.indexOf("null") === -1, "state " + s + " detail: " + d);
        }
    }

    function test_missingVersionsDegradeToASentenceThatStillReads() {
        // Every property the Updater exposes can legitimately be empty:
        // availableVersion is cleared in UpToDate/AheadOfChannel, and a
        // build without the version define reports "".
        for (var i = 0; i < allStates.length; ++i) {
            var s = allStates[i];
            var t = UF.title(s, "");
            var d = UF.detail(s, {});
            verify(t.length > 0);
            verify(t.indexOf("undefined") === -1);
            verify(d.indexOf("undefined") === -1);
            // No dangling "()" or " ·" from an interpolated empty value.
            verify(t.indexOf("()") === -1, "state " + s + " title: " + t);
            verify(d.indexOf("()") === -1, "state " + s + " detail: " + d);
            verify(d.indexOf(" ·") === -1, "state " + s + " detail: " + d);
        }
    }

    function test_detailDoesNotRepeatTheTitle() {
        for (var i = 0; i < allStates.length; ++i) {
            var s = allStates[i];
            var ctx = { currentVersion: "1.0.0", channel: "stable",
                        latestStableVersion: "0.9.0", osName: "osx" };
            var t = UF.title(s, "1.1.0");
            var d = UF.detail(s, ctx);
            verify(d.indexOf(t) === -1, "state " + s + " repeats its title");
        }
    }

    function test_failedDefersTheVerbatimMessageToItsOwnBanner() {
        // lastError is not interpolated into detail(); the panel shows it
        // in a bordered banner so a 200-character network error can't
        // shove the buttons off the bottom of the dialog.
        var d = UF.detail(UF.Failed, { currentVersion: "1.0.0" });
        verify(d.length > 0);
        verify(d.indexOf("1.0.0") === -1);
    }

    function test_channelLabelFallsBackToStable() {
        compare(UF.channelLabel("beta"), "beta");
        compare(UF.channelLabel("stable"), "stable");
        compare(UF.channelLabel(""), "stable");
        compare(UF.channelLabel(undefined), "stable");
    }

    function test_upToDateNamesTheChannelItCheckedOn() {
        var beta = UF.detail(UF.UpToDate, { currentVersion: "1.0.0-rc.1",
                                            channel: "beta" });
        verify(beta.indexOf("beta") >= 0);
        var stable = UF.detail(UF.UpToDate, { currentVersion: "1.0.0",
                                              channel: "stable" });
        verify(stable.indexOf("stable") >= 0);
    }

    // ── The apply action is platform-specific ───────────────────────

    function test_linuxIsNotToldItWillRestart() {
        // Updater::applyLinux opens the release page and quits; it does
        // not replace the running app. Saying "Restart to install" there
        // is a false statement about what the button does.
        compare(UF.applyLabel("linux"), "Open the release page");
        compare(UF.applyLabel("osx"), "Restart to install");
        compare(UF.applyLabel("windows"), "Restart to install");
        verify(UF.applyHint("linux").indexOf("restart") === -1);
        verify(UF.applyHint("linux").indexOf("package manager") >= 0);
        verify(UF.applyHint("osx").indexOf("restart") >= 0);
    }

    function test_readyToApplyDetailTracksThePlatform() {
        compare(UF.detail(UF.ReadyToApply, { osName: "linux" }),
                UF.applyHint("linux"));
        compare(UF.detail(UF.ReadyToApply, { osName: "osx" }),
                UF.applyHint("osx"));
    }

    // ── Progress ────────────────────────────────────────────────────

    function test_progressKnown_data() {
        return [
            { tag: "positive",  n: 1024,      expect: true },
            { tag: "zero",      n: 0,         expect: false },
            { tag: "unknown",   n: -1,        expect: false },
            { tag: "undefined", n: undefined, expect: false },
        ];
    }
    function test_progressKnown(d) {
        compare(UF.progressKnown(d.n), d.expect);
    }

    function test_progressFraction_data() {
        return [
            { tag: "start",      d: 0,    t: 100,  expect: 0 },
            { tag: "half",       d: 50,   t: 100,  expect: 0.5 },
            { tag: "done",       d: 100,  t: 100,  expect: 1 },
            { tag: "overshoot",  d: 140,  t: 100,  expect: 1 },
            { tag: "negative",   d: -5,   t: 100,  expect: 0 },
            { tag: "no total",   d: 50,   t: 0,    expect: 0 },
        ];
    }
    function test_progressFraction(d) {
        compare(UF.progressFraction(d.d, d.t), d.expect);
    }

    function test_percentTextNeverClaimsAProgressItDoesNotHave() {
        compare(UF.percentText(0, 0), "");
        compare(UF.percentText(5, -1), "");
        compare(UF.percentText(0, 100), "0%");
        // Floors rather than rounds: "100%" must mean finished.
        compare(UF.percentText(999, 1000), "99%");
        compare(UF.percentText(1000, 1000), "100%");
    }

    function test_byteProgressUsesTheOneSizeFormatter() {
        // Same helper as VideoPlayerCard (qml/js/PlaybackMath.js), so a
        // megabyte is spelled the same way in both places.
        compare(UF.byteProgress(5 * 1024 * 1024, 10 * 1024 * 1024),
                "5.0 MB of 10.0 MB");
        compare(UF.byteProgress(2048, 0), "2.0 KB");
        compare(UF.byteProgress(0, 1024 * 1024), "0 B of 1.0 MB");
    }

    // ── Release notes: markdown in, plain text out ──────────────────

    function test_theBugFromTheScreenshot() {
        // GitHub's own footer, rendered literally by the old TextArea.
        var out = UF.plainNotes(
            "**Full Changelog**: https://github.com/o/r/compare/v1...v2");
        compare(out,
            "Full Changelog: https://github.com/o/r/compare/v1...v2");
    }

    function test_plainNotes_data() {
        return [
            { tag: "atx heading",  md: "## What's Changed", expect: "What's Changed" },
            { tag: "bullet dash",  md: "- fixed a thing",   expect: "• fixed a thing" },
            { tag: "bullet star",  md: "* fixed a thing",   expect: "• fixed a thing" },
            { tag: "bold",         md: "**loud**",          expect: "loud" },
            { tag: "italic",       md: "*quiet*",           expect: "quiet" },
            { tag: "strike",       md: "~~gone~~",          expect: "gone" },
            { tag: "code",         md: "use `--flag` now",  expect: "use --flag now" },
            { tag: "quote",        md: "> quoted",          expect: "quoted" },
            // A rule leaves the paragraph break it was drawing, not a
            // seam — "a\nb" would glue two sections together.
            { tag: "rule",         md: "a\n---\nb",         expect: "a\n\nb" },
            { tag: "setext",       md: "Heading\n===\nbody", expect: "Heading\n\nbody" },
            { tag: "link",         md: "see [the docs](https://x/y)", expect: "see the docs" },
            { tag: "image",        md: "![a chart](https://x/y.png)", expect: "a chart" },
            { tag: "autolink",     md: "<https://x/y>",     expect: "https://x/y" },
            { tag: "snake case",   md: "renamed max_bitrate_kbps", expect: "renamed max_bitrate_kbps" },
            { tag: "empty",        md: "",                  expect: "" },
            { tag: "undefined",    md: undefined,           expect: "" },
            { tag: "null",         md: null,                expect: "" },
            { tag: "blank runs",   md: "a\n\n\n\n\nb",      expect: "a\n\nb" },
            { tag: "crlf",         md: "a\r\nb",            expect: "a\nb" },
            { tag: "trailing ws",  md: "  a  \n",           expect: "  a" },
        ];
    }
    function test_plainNotes(d) { compare(UF.plainNotes(d.md), d.expect); }

    // ── Release notes: the untrusted-input properties ───────────────
    //
    // The release body is remote text. It is rendered with PlainText, so
    // these assertions are about what reaches the layout, not about an
    // escaping scheme — there is no rich-text document to inject into.

    function test_noMarkupSurvivesTheReduction() {
        var hostile = [
            "<img src='https://tracker.example/pixel.png'>",
            "<a href=\"file:///etc/passwd\">click</a>",
            "<script>doSomething()</script>",
            "<iframe src='https://evil.example'></iframe>",
            "<!-- a comment -->",
            "<b onmouseover='x()'>hover</b>",
            "![](https://tracker.example/p.png)",
            "<style>* { color: red }</style>",
        ];
        for (var i = 0; i < hostile.length; ++i) {
            var out = UF.plainNotes(hostile[i]);
            verify(out.indexOf("<") === -1, "angle bracket survived: " + out);
            verify(out.indexOf(">") === -1, "angle bracket survived: " + out);
            verify(out.indexOf("tracker.example") === -1,
                   "remote asset URL survived: " + out);
        }
    }

    function test_linkTargetsAreDroppedNotRendered() {
        // The label survives; the destination does not. Nothing in the
        // panel is clickable, so a URL printed here would be dead text
        // that merely looks trustworthy.
        var out = UF.plainNotes("[bsfchat.com](https://phish.example/login)");
        compare(out, "bsfchat.com");
    }

    function test_notesAreLengthCapped() {
        var huge = "";
        for (var i = 0; i < 400; ++i) huge += "0123456789abcdef\n";
        var out = UF.plainNotes(huge);
        verify(out.length <= UF.NOTES_LIMIT + 1, "length " + out.length);
        verify(out.charAt(out.length - 1) === "…");
    }

    function test_hasNotesIsFalseForBodiesThatAreOnlySyntax() {
        verify(!UF.hasNotes(""));
        verify(!UF.hasNotes(undefined));
        verify(!UF.hasNotes("\n\n   \n"));
        verify(!UF.hasNotes("---"));
        verify(!UF.hasNotes("<!-- nothing to see -->"));
        verify(UF.hasNotes("## Fixes\n- one thing"));
    }
}
