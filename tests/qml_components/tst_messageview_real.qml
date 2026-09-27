import QtQuick
import QtQuick.Layouts
import QtTest
import BSFChat
import "qrc:/qt/qml/BSFChat/qml/js/TimelineOverlay.js" as TimelineOverlay

// MessageView.qml — the shipped timeline, instantiated, filled with real rows
// out of a real MessageModel, and MEASURED.
//
// WHY THIS FILE EXISTS
//
// `theTimelineBottomAnchorsAShortHistory()` in tests/test_qml_hygiene.cpp is
// the guard this replaces, and it is the one that already let a real bug
// ship. It greps MessageView.qml for four strings:
//
//     topMargin: TimelineOverlay.bottomAnchorSlack(
//     TimelineOverlay.restingContentY(
//     !/contentY\s*=\s*originY\s*;/
//     verticalLayoutDirection: ListView.TopToBottom
//
// Every one of those can be present while the timeline renders five messages
// tucked under the header with 900pt of dead space down to the composer,
// because none of them says where a row ends up. The store screenshots did.
//
// tests/qml/tst_timelineoverlay.qml is the other half and it STAYS: it checks
// that bottomAnchorSlack() and restingContentY() compute the right numbers.
// It cannot check that MessageView's ListView is the thing that consumes
// them, because it builds a ListView of its own. That gap — "the rules are
// right, and is the shipped view wired to them?" — is exactly the gap the
// text-scrape was standing in, and it is what is measured here.
//
// So every case below asserts on a POSITION, not on a property being set:
// where the first row's top edge is, where the last row's bottom edge is,
// and whether the surface stays there when another message lands.
//
// The environment (AppSettings singleton, serverManager / appSettings /
// haptics context properties) is stubbed in tests/qml_components_test_main.cpp;
// the MessageModel behind serverManager.activeServer.messageModel is the real
// src/model/MessageModel.h. MessageView.qml, its delegates and Theme.qml are
// all the shipped files.
TestCase {
    id: tc
    name: "MessageViewReal"
    when: windowShown
    width: 600
    height: 700
    visible: true

    // A fixed frame, so every measurement below is against a viewport whose
    // size the test chose rather than whatever the runner's window happened
    // to be. MessageView fills it the way both shells mount it.
    Item {
        id: frame
        anchors.fill: parent

        MessageView {
            id: view
            anchors.fill: parent
        }
    }

    // ── helpers ──────────────────────────────────────────────────────

    function conn() {
        return serverManager.activeServer;
    }

    function descendants(item) {
        var out = [];
        if (!item || item.children === undefined)
            return out;
        for (var i = 0; i < item.children.length; ++i) {
            var c = item.children[i];
            out.push(c);
            out = out.concat(descendants(c));
        }
        return out;
    }

    // The timeline ListView, found by the one thing that cannot be true of
    // any other view in this tree: its model IS the room's MessageModel.
    //
    // Not by child index and not by type alone — MessageView also carries
    // ThreadPanel's list and ForwardDialog's, and an index would silently
    // retarget the moment anybody adds a sibling. Matching on model identity
    // means that if the timeline ever stops being driven by messageModel,
    // this returns null and every case below fails loudly rather than
    // measuring the wrong list.
    function timeline() {
        var mm = conn().messageModel;
        var d = descendants(view);
        for (var i = 0; i < d.length; ++i) {
            var it = d[i];
            if (it.contentY === undefined || it.orientation === undefined)
                continue;   // not a ListView
            if (it.model === mm)
                return it;
        }
        return null;
    }

    // Bottom edge of row `i`, expressed in VIEWPORT coordinates — which is
    // the coordinate system the user's eye is in and the only one in which
    // "the newest message sits on the composer" is a statement.
    //
    // A ListView lays rows out in CONTENT coordinates, so item.y alone says
    // nothing about where the row appears; `- contentY` is what turns it
    // into a position on screen. The shipped bug was entirely a contentY /
    // topMargin question, so measuring in content coordinates would have
    // reproduced it rather than caught it.
    function rowBottomInViewport(lv, i) {
        var it = lv.itemAtIndex(i);
        if (!it) return NaN;
        return it.y + it.height - lv.contentY;
    }

    function rowTopInViewport(lv, i) {
        var it = lv.itemAtIndex(i);
        if (!it) return NaN;
        return it.y - lv.contentY;
    }

    // Rows are asynchronous. itemAtIndex() returns null until the delegate
    // for that index is created, and delegate heights commit over one or
    // more layout passes, so every measurement below waits for the geometry
    // to settle rather than reading it on the frame the model changed.
    // #34 shipped a geometry assertion that passed under ctest and failed
    // run directly for exactly this reason.
    function waitForRows(lv, n) {
        tryVerify(function() { return lv.count === n; });
        tryVerify(function() { return lv.itemAtIndex(n - 1) !== null; });
        tryVerify(function() { return lv.contentHeight > 0; });
        waitForRendering(lv);
    }

    function init() {
        serverManager.resetForTest();
        appSettings.resetForTest();
        var lv = timeline();
        verify(lv !== null, "timeline ListView not found in MessageView");
        tryCompare(lv, "count", 0);
    }

    // ── 1. the shipped component came up ─────────────────────────────

    // Stated as a case of its own so that a failure to load surfaces as one
    // clear error rather than as every measurement below reading NaN.
    function test_a_the_real_messageview_loaded() {
        verify(view !== null);
        verify(view.toString().indexOf("MessageView") >= 0,
               "expected a MessageView instance, got " + view);
        var lv = timeline();
        verify(lv !== null);
        // It is the shipped list, in the shipped direction. The hygiene
        // guard asserts this string is in the file; this asserts the live
        // view is in that mode, which is what the top-margin arithmetic
        // below is only valid for.
        compare(lv.verticalLayoutDirection, ListView.TopToBottom);
    }

    // ── 2. the bug that shipped ──────────────────────────────────────

    // A history shorter than the viewport must sit on the BOTTOM of it.
    //
    // This is the measurement the grep could not make. With three messages
    // in a 700pt frame the rows are a couple of hundred points tall at most,
    // and the question is where those points are. Top-anchored (the bug) puts
    // row 0 at viewport y == 0 and leaves everything below it empty.
    // Bottom-anchored puts the LAST row's bottom edge on the composer.
    function test_b_a_short_history_sits_at_the_bottom() {
        var lv = timeline();
        conn().pushMessages(3);
        waitForRows(lv, 3);

        // Precondition: this case is only about the short-history state, so
        // say out loud that the content really is shorter than the viewport.
        // If a delegate ever grows enough to break that, this fails here
        // with a clear reason instead of failing an anchor assertion for a
        // reason that has nothing to do with anchoring.
        verify(lv.contentHeight < lv.height,
               "three messages no longer fit in a " + lv.height
               + "pt viewport (contentHeight " + lv.contentHeight
               + "); this case needs a shorter history or a taller frame");

        // The slack is real and it is on the view.
        tryVerify(function() {
            return Math.abs(lv.topMargin - (lv.height - lv.contentHeight)) < 1.0;
        }, 5000, "topMargin is not the bottom-anchor slack: topMargin "
                 + lv.topMargin + ", height " + lv.height
                 + ", contentHeight " + lv.contentHeight);

        // THE ASSERTION. The newest message's bottom edge is at the bottom of
        // the viewport, not a screen away from it.
        tryVerify(function() {
            return Math.abs(rowBottomInViewport(lv, lv.count - 1) - lv.height) < 1.5;
        }, 5000, "the newest row does not end at the bottom of the timeline: "
                 + "bottom " + rowBottomInViewport(lv, lv.count - 1)
                 + ", viewport height " + lv.height);

        // And the other side of the same fact, stated so a half-fix cannot
        // pass: the OLDEST row starts well down the viewport rather than
        // against the header. This is the assertion that fails on a
        // top-anchored list, and it fails by the whole height of the gap.
        var top = rowTopInViewport(lv, 0);
        verify(top > lv.height * 0.5,
               "the oldest of three messages starts at y=" + top + " in a "
               + lv.height + "pt viewport — the timeline is top-anchored and "
               + "the dead space is back");
    }

    // The regression `restingContentY()` exists for.
    //
    // `_jumpToEnd()` runs on every count change. Its old un-scrollable branch
    // assigned `contentY = originY`, which on a list carrying a top margin is
    // one whole margin ABOVE where the list rests — it pulled the rows back
    // under the header and put the gap straight back the moment anybody said
    // anything. The hygiene guard looks for the absence of the string
    // `contentY = originY;`. That is satisfied by writing the same wrong
    // number any other way; this is satisfied only by the rows staying put.
    function test_c_a_new_message_does_not_undo_the_anchor() {
        var lv = timeline();
        conn().pushMessages(3);
        waitForRows(lv, 3);
        tryVerify(function() {
            return Math.abs(rowBottomInViewport(lv, lv.count - 1) - lv.height) < 1.5;
        });

        // Somebody says something. This is the event that used to break it.
        conn().pushMessages(1);
        waitForRows(lv, 4);

        tryVerify(function() {
            return Math.abs(rowBottomInViewport(lv, lv.count - 1) - lv.height) < 1.5;
        }, 5000, "after a message arrived the newest row ends at "
                 + rowBottomInViewport(lv, lv.count - 1) + " in a "
                 + lv.height + "pt viewport — _jumpToEnd undid the anchor");

        // contentY rests where restingContentY() says it should, which for a
        // margined, un-scrollable list is originY - topMargin and NOT originY.
        tryVerify(function() {
            return Math.abs(lv.contentY
                            - TimelineOverlay.restingContentY(
                                  lv.originY, lv.topMargin,
                                  lv.contentHeight, lv.height)) < 1.0;
        }, 5000, "contentY " + lv.contentY + " is not the resting position "
                 + TimelineOverlay.restingContentY(lv.originY, lv.topMargin,
                                                   lv.contentHeight, lv.height));

        // Stated separately because it is the exact old bug: resting AT
        // originY with a non-zero margin is the broken position.
        verify(lv.topMargin > 0,
               "precondition: four messages should still be a short history");
        verify(Math.abs(lv.contentY - lv.originY) > 1.0,
               "contentY is parked at originY with a " + lv.topMargin
               + "pt top margin — that is the `contentY = originY` bug");
    }

    // The other side of the contract: the fix has to be INERT once the
    // content is taller than the viewport, because every piece of scroll
    // bookkeeping in MessageView.qml is written for that case. A margin that
    // survives into a scrollable list would put a permanent gap above the
    // history and shift every scroll position in the file.
    function test_d_a_full_history_carries_no_slack() {
        var lv = timeline();
        conn().pushMessages(60);
        waitForRows(lv, 60);

        tryVerify(function() { return lv.contentHeight > lv.height; },
                  5000, "60 messages did not overflow a " + lv.height
                        + "pt viewport (contentHeight " + lv.contentHeight + ")");
        tryVerify(function() { return lv.topMargin === 0; },
                  5000, "a scrollable timeline still carries "
                        + lv.topMargin + "pt of bottom-anchor slack");

        // And it is parked at the END of the history, which for a scrollable
        // list is the same "newest message on the composer" statement as
        // test_b makes for a short one.
        tryVerify(function() {
            return Math.abs(lv.contentY - (lv.originY + lv.contentHeight - lv.height))
                   < 2.0;
        }, 5000, "a full timeline did not open at the newest message: contentY "
                 + lv.contentY + ", end " + (lv.originY + lv.contentHeight - lv.height));
    }

    // Growing past the viewport is the transition between the two states
    // above, and it is where a wrong sign shows up: the margin has to fall to
    // zero as the content crosses the viewport height, without the rows
    // jumping.
    function test_e_the_slack_falls_away_as_the_history_grows() {
        var lv = timeline();
        conn().pushMessages(2);
        waitForRows(lv, 2);
        verify(lv.topMargin > 0, "two messages should be a short history");
        var smallSlack = lv.topMargin;

        conn().pushMessages(6);
        waitForRows(lv, 8);
        // Still short, but less slack: the margin tracks the content.
        if (lv.contentHeight < lv.height) {
            verify(lv.topMargin < smallSlack,
                   "the slack did not shrink as rows were added: "
                   + smallSlack + " → " + lv.topMargin);
            verify(lv.topMargin >= 0, "negative slack");
        }

        conn().pushMessages(80);
        waitForRows(lv, 88);
        tryVerify(function() { return lv.topMargin === 0; },
                  5000, "slack survived into a scrollable list");
        // Bottom-anchored throughout: the newest row is still on the
        // composer after the transition.
        tryVerify(function() {
            return Math.abs(rowBottomInViewport(lv, lv.count - 1) - lv.height) < 2.0;
        }, 5000, "after growing past the viewport the newest row ends at "
                 + rowBottomInViewport(lv, lv.count - 1));
    }
}
