import QtQuick
import QtTest
import "../../qml/js/TimelineOverlay.js" as Overlay

// U-H1: the message timeline's overlays — the pagination spinner, the
// scroll-to-latest chevron and the empty state.
//
// READ THIS BEFORE TRUSTING IT. Two halves, proved two different ways:
//
//   1. WHEN each overlay shows. That is qml/js/TimelineOverlay.js, which
//      MessageView.qml imports and calls, so the rules exercised below are
//      literally the shipped ones.
//
//   2. WHERE each overlay is, which is the half the audit got wrong and
//      the reason these cases exist at all.
//
//      The report said the three were scrolling away with the
//      conversation because they were declared inside the ListView, and a
//      Flickable reparents every visual child into `contentItem` — the
//      item that moves. The first assertion below is that claim, measured:
//      it is true of Flickable and FALSE of ListView. QQuickItemView keeps
//      an inline child as a direct child of the VIEW, so `anchors.bottom:
//      parent.bottom` anchored the chevron to the viewport all along and
//      the reported symptom does not reproduce on Qt 6.
//
//      MessageView moved them out to siblings regardless, so the placement
//      no longer depends on a subclass quietly overriding a default
//      property. These cases are what keeps that decision honest: if a
//      future Qt makes ListView behave like its base class, the first case
//      fails and says so, rather than the chevron silently going for a
//      ride again.
//
//      The scene is a REPLICA of the fixed structure, not MessageView
//      itself: that component imports the BSFChat module, which is
//      compiled into the application binary and cannot be loaded from a
//      test executable (same constraint as tst_transportbarinput.qml). So
//      these cases prove the Qt behaviour the structure relies on and the
//      visibility rules at each scroll position. They cannot prove
//      MessageView.qml is still wired this way; the guard for that is the
//      comment above `Item { id: timelineArea }` in that file.
TestCase {
    id: tc
    name: "TimelineOverlay"
    when: windowShown
    width: 300
    height: 200
    visible: true

    // ── 1. The visibility rules ──────────────────────────────────────

    function test_spinnerTracksLoadingHistoryOnly() {
        verify(Overlay.spinnerVisible(true));
        verify(!Overlay.spinnerVisible(false));
        // Not merely "there is more history" — only a live request.
        verify(!Overlay.spinnerVisible(undefined));
    }

    function test_chevronVisible_data() {
        return [
            { tag: "away from end, rows",   atBottom: false, rows: 12, expect: true },
            { tag: "at end, rows",          atBottom: true,  rows: 12, expect: false },
            { tag: "away from end, empty",  atBottom: false, rows: 0,  expect: false },
            { tag: "at end, empty",         atBottom: true,  rows: 0,  expect: false },
            // A view that has not reported yet must not flash the chevron.
            { tag: "unreported, empty",     atBottom: undefined, rows: 0, expect: false },
        ];
    }
    function test_chevronVisible(d) {
        compare(Overlay.chevronVisible(d.atBottom, d.rows), d.expect);
    }

    function test_emptyStateKind_data() {
        return [
            { tag: "no server",   server: false, room: "!r:s", rows: 5, expect: "no-server" },
            { tag: "no channel",  server: true,  room: "",     rows: 5, expect: "no-channel" },
            // "Pick a channel / choose one from the sidebar" is the wrong
            // advice on a server that has none — it sends the reader to look
            // for something that is not there, which is what a brand-new
            // server did until it started creating its own first channels.
            { tag: "no channels on the server", server: true, room: "", rows: 0,
              channels: 0, expect: "no-channels" },
            { tag: "channels exist, none picked", server: true, room: "", rows: 0,
              channels: 3, expect: "no-channel" },
            // A caller that cannot count must not be told the server is
            // empty: undefined keeps the original answer.
            { tag: "channel count unknown", server: true, room: "", rows: 0,
              expect: "no-channel" },
            // "No channels" is about the SERVER, so it outranks anything the
            // message list is doing — with no room selected there is no
            // history to be loading in the first place.
            { tag: "no channels beats a loading list", server: true, room: "", rows: 0,
              loading: true, more: true, channels: 0, expect: "no-channels" },
            { tag: "no history",  server: true,  room: "!r:s", rows: 0, expect: "no-history" },
            { tag: "has content", server: true,  room: "!r:s", rows: 1, expect: "" },
            // An empty LIST is not always an empty CHANNEL (#notifications,
            // 2026-09-22: the newest history was all edits, which are never
            // rows, and the view said "be the first to say something" over
            // 46 real messages).
            { tag: "empty, loading", server: true, room: "!r:s", rows: 0,
              loading: true, more: true, expect: "" },
            { tag: "empty, loading, first page", server: true, room: "!r:s", rows: 0,
              loading: true, more: false, expect: "" },
            { tag: "empty, older history exists", server: true, room: "!r:s", rows: 0,
              loading: false, more: true, expect: "more-history" },
            { tag: "empty, start of room", server: true, room: "!r:s", rows: 0,
              loading: false, more: false, expect: "no-history" },
            { tag: "content, more history", server: true, room: "!r:s", rows: 3,
              loading: false, more: true, expect: "" },
        ];
    }
    function test_emptyStateKind(d) {
        var kind = Overlay.emptyStateKind(d.server, d.room, d.rows, d.loading, d.more,
                                          d.channels);
        compare(kind, d.expect);
        compare(Overlay.emptyStateVisible(d.server, d.room, d.rows, d.loading, d.more,
                                          d.channels),
                d.expect !== "");
        // Every non-empty kind has wording and an icon of its own — the
        // three cases need different copy, which is why the state is a
        // kind and not a bool.
        if (d.expect !== "") {
            verify(Overlay.emptyStateTitle(kind).length > 0);
            verify(Overlay.emptyStateBody(kind).length > 0);
            verify(Overlay.emptyStateIcon(kind).length > 0);
        }
    }

    function test_emptyStateWordingIsDistinctPerKind() {
        var kinds = ["no-server", "no-channels", "no-channel", "no-history", "more-history"];
        var seenTitle = {}, seenIcon = {};
        for (var i = 0; i < kinds.length; ++i) {
            var t = Overlay.emptyStateTitle(kinds[i]);
            var ic = Overlay.emptyStateIcon(kinds[i]);
            verify(seenTitle[t] === undefined, "duplicate title for " + kinds[i]);
            verify(seenIcon[ic] === undefined, "duplicate icon for " + kinds[i]);
            seenTitle[t] = true;
            seenIcon[ic] = true;
        }
    }

    // The "Load older messages" button appears only where the user has no
    // other way to ask for history: the list cannot scroll to the top and
    // the client has stopped asking on its own.
    function test_loadOlderVisible_data() {
        return [
            { tag: "stuck: short list, budget spent", more: true, loading: false,
              scrollable: false, spent: true, expect: true },
            { tag: "client still filling on its own", more: true, loading: false,
              scrollable: false, spent: false, expect: false },
            { tag: "request in flight", more: true, loading: true,
              scrollable: false, spent: true, expect: false },
            { tag: "scrollable: scroll-to-top works", more: true, loading: false,
              scrollable: true, spent: true, expect: false },
            // The guard that matters most: at the true start of the room
            // there is nothing to load, whatever else is true.
            { tag: "start of room", more: false, loading: false,
              scrollable: false, spent: true, expect: false },
        ];
    }
    function test_loadOlderVisible(d) {
        compare(Overlay.loadOlderVisible(d.more, d.loading, d.scrollable, d.spent),
                d.expect);
    }

    // ── 2. The structure ─────────────────────────────────────────────

    Component {
        id: sceneComponent
        Item {
            id: timelineArea
            width: 300
            height: 200

            property alias list: list
            property alias overlay: timelineOverlay
            property alias sibling: siblingChevron
            property alias inner: innerChevron
            property alias flick: plainFlickable
            property alias flickChild: flickableChevron

            ListView {
                id: list
                anchors.fill: parent
                clip: true
                model: 60
                delegate: Rectangle { width: list.width; height: 20; color: "#333" }

                // The OLD structure: a visual child of the ListView.
                Rectangle {
                    id: innerChevron
                    anchors.bottom: parent.bottom
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 40; height: 32
                    color: "#0f0"
                }
            }

            // The control: the same declaration under a plain Flickable,
            // which DOES reparent it into contentItem. Off to one side and
            // never interacted with except by the parent assertions.
            Flickable {
                id: plainFlickable
                width: 1; height: 100
                contentHeight: 1000
                Rectangle {
                    id: flickableChevron
                    anchors.bottom: parent.bottom
                    width: 1; height: 8
                }
            }

            // The FIXED structure: a sibling, anchored to the list itself.
            Item {
                id: timelineOverlay
                anchors.fill: list

                Rectangle {
                    id: siblingChevron
                    anchors.bottom: parent.bottom
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 40; height: 32
                    color: "#00f"
                    visible: Overlay.chevronVisible(list.atYEnd, list.count)
                }
            }
        }
    }

    property var scene: null

    function init() {
        scene = sceneComponent.createObject(tc);
        verify(scene !== null);
        scene.list.forceLayout();
        wait(0);
    }

    function cleanup() {
        if (scene) { scene.destroy(); scene = null; }
    }

    // THE MEASUREMENT THE AUDIT NEEDED.
    //
    // A plain Flickable reparents an inline visual child into its
    // contentItem. A ListView does not — QQuickItemView keeps it as a
    // direct child of the view. That single difference is the whole of
    // U-H1: it is why the chevron was never actually riding the content,
    // and why nobody reading the file could tell either way.
    function test_listViewDoesNotReparentInlineChildrenButFlickableDoes() {
        compare(scene.inner.parent, scene.list,
                "a ListView's inline child should stay a child of the VIEW");
        verify(scene.inner.parent !== scene.list.contentItem);

        // The same declaration under a plain Flickable, for contrast.
        compare(scene.flickChild.parent, scene.flick.contentItem,
                "a Flickable's inline child SHOULD land in contentItem");

        // The overlay we ship is a sibling, so it is correct either way.
        compare(scene.sibling.parent, scene.overlay);
        verify(scene.sibling.parent !== scene.list.contentItem);
    }

    // Scroll to the middle. Neither the sibling nor (on this Qt) the
    // in-list child moves — but the child under a real Flickable does,
    // which is the behaviour the sibling placement makes irrelevant.
    function test_theOverlayDoesNotRideTheContent() {
        var list = scene.list;

        var siblingTop0 = scene.sibling.mapToItem(scene, 0, 0).y;
        var innerTop0 = scene.inner.mapToItem(scene, 0, 0).y;
        var flickTop0 = scene.flickChild.mapToItem(scene, 0, 0).y;

        list.contentY = list.contentHeight / 2;
        scene.flick.contentY = 400;
        wait(0);

        fuzzyCompare(scene.sibling.mapToItem(scene, 0, 0).y, siblingTop0, 0.5);
        fuzzyCompare(scene.inner.mapToItem(scene, 0, 0).y, innerTop0, 0.5);
        // ... whereas this one has gone for a ride.
        verify(Math.abs(scene.flickChild.mapToItem(scene, 0, 0).y - flickTop0) > 10,
               "a Flickable's inline child should have scrolled with content");

        // And the shipped overlay is where a user can click it.
        var top = scene.sibling.mapToItem(scene, 0, 0).y;
        verify(top >= 0 && top < scene.height);
    }

    // Walk the list from top to bottom and check the chevron's visibility
    // at each position: shown everywhere except parked at the end.
    function test_chevronVisibilityAcrossScrollPositions() {
        var list = scene.list;
        var maxY = list.contentHeight - list.height;
        verify(maxY > 0, "the fixture must actually be scrollable");

        var positions = [0, maxY * 0.25, maxY * 0.5, maxY * 0.9, maxY];
        for (var i = 0; i < positions.length; ++i) {
            list.contentY = positions[i];
            wait(0);
            var atEnd = list.atYEnd;
            compare(scene.sibling.visible, !atEnd,
                    "contentY=" + positions[i] + " atYEnd=" + atEnd);
            if (!atEnd) {
                // Visible AND on screen — the part the old structure got
                // wrong even when `visible` said true.
                var top = scene.sibling.mapToItem(scene, 0, 0).y;
                verify(top >= 0 && top < scene.height);
            }
        }
    }

    // An empty room has nothing to jump to, so no chevron at any position.
    function test_noChevronWithoutRows() {
        var list = scene.list;
        list.model = 0;
        wait(0);
        compare(scene.sibling.visible, false);
    }
}
