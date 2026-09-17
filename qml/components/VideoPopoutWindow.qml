import QtQuick
import QtQuick.Controls
import QtQuick.Window
import QtMultimedia
import BSFChat

import "../js/VideoStage.js" as VideoStage
import "../js/VideoWindows.js" as VideoWindows

// One feed in a window of its own: an ordinary, resizable, movable
// top-level window showing a single video aspect-fit on black, with a
// keep-on-top pin and its own fullscreen toggle.
//
// The point is a second monitor. The in-room tile STAYS LIVE while a
// feed is popped out — VoiceRoom does not know or care that this window
// exists, the tile is never destroyed or reparented (SPEC §3.4), and it
// grows a small "popped out" badge and nothing else. That works because
// VideoStreamRegistry fans one decoded stream out to every attached
// sink, so this window is a second SURFACE, not a second decode and not
// a theft. tests/test_video_registry.cpp pins it.
//
// Lifetime is the owner's, not this window's: VoiceRoom runs an
// Instantiator over VideoWindows.requestPopout() state, and a feed that
// ends (share stopped, peer left, we left the channel) is pruned out of
// that state, which destroys this object. Hence there is no "is my feed
// still alive" polling here — closing on feed end is one rule, in the
// .js, tested in tests/qml/tst_videowindows.qml.
//
// transientParent is explicitly null: a QML Window nested in an item
// tree otherwise adopts the app window as its transient parent, which on
// macOS makes it an NSWindow child that is ALWAYS ordered above its
// parent and follows it between Spaces. That is the opposite of what a
// pop-out is for, and it would make the pin toggle below meaningless.
Window {
    id: pop

    // The feed object from VideoStage.mergeFeeds().
    property var feed: null
    property string displayName: ""
    property int liveTick: 0
    // How many pop-outs were already open when this one was created —
    // drives the cascade so two windows restoring the same remembered
    // position do not land exactly on top of each other.
    property int cascadeIndex: 0

    // The user closed it from inside (the window's own close button is
    // handled here too). The owner drops it from the pop-out state.
    signal closeRequested()

    readonly property bool _isScreen:
        feed && feed.kind === VideoStage.SCREEN

    readonly property bool _live: {
        pop.liveTick;
        if (!pop.feed) return false;
        if (pop.feed.isSelf) return true;
        var s = serverManager.activeServer;
        return s && s.videoRegistry
            ? s.videoRegistry.hasLiveVideo(pop.feed.userId, pop.feed.streamId)
            : false;
    }

    // ── Window identity and shape ────────────────────────────────────

    title: VideoWindows.windowTitle(
        pop.displayName || (pop.feed ? pop.feed.userId : ""),
        pop.feed ? pop.feed.kind : "",
        pop.feed ? pop.feed.isSelf === true : false)
    color: "black"
    transientParent: null
    // Pinning is a window LEVEL change, not a recreate: Qt maps
    // WindowStaysOnTopHint to the NSWindow level on macOS and applies it
    // in place, so the VideoOutput and its sink survive a toggle.
    flags: Qt.Window | (pop.pinned ? Qt.WindowStaysOnTopHint : 0)
    property bool pinned: false

    minimumWidth: VideoWindows.minimumSize().width
    minimumHeight: VideoWindows.minimumSize().height

    // ── Remembered geometry ──────────────────────────────────────────
    //
    // Per kind, in Settings. Restored once here; saved debounced below,
    // because a drag emits xChanged for every pixel of the drag and
    // QSettings would be written a few hundred times per move.
    readonly property string _geometryKey:
        VideoWindows.geometryKey(pop.feed ? pop.feed.kind : "")

    function _screenRects() {
        var out = [];
        var list = Qt.application.screens;
        for (var i = 0; i < list.length; ++i) {
            out.push({ x: list[i].virtualX, y: list[i].virtualY,
                       width: list[i].width, height: list[i].height });
        }
        return out;
    }

    Component.onCompleted: {
        // The feed has usually not delivered a frame yet at this point,
        // so the aspect is unknown and restoreGeometry falls back to the
        // 16:9 default. _shapeToAspect() below fixes that on the first
        // frame — but only when there was nothing remembered, because a
        // size the user chose outranks one we derived.
        var saved = appSettings.popoutGeometry(pop._geometryKey);
        var g = VideoWindows.restoreGeometry(saved, pop._aspect());
        pop.width = g.width;
        pop.height = g.height;
        pop._hadSavedSize = (saved.width > 0 && saved.height > 0);

        if (g.hasPosition) {
            var offset = VideoWindows.cascadeOffset(pop.cascadeIndex);
            var x = g.x + offset;
            var y = g.y + offset;
            // The monitor it was last on may not be plugged in any more.
            if (VideoWindows.positionIsOnScreen(x, y, pop._screenRects())) {
                pop.x = x;
                pop.y = y;
            }
        }
        pop.visible = true;
        pop.requestActivate();
    }

    property bool _hadSavedSize: false
    property bool _shapedToAspect: false

    function _aspect() {
        var size = output.videoSink ? output.videoSink.videoSize : null;
        if (!size || !(size.width > 0) || !(size.height > 0)) return 0;
        return size.width / size.height;
    }

    // First real frame: if nothing was remembered, take the window to
    // ~960x540's worth of area in the feed's own aspect ratio, so a
    // portrait phone camera is not a 16:9 box with pillars either side.
    // Once only, and never against a size the user picked.
    function _shapeToAspect() {
        if (pop._shapedToAspect || pop._hadSavedSize) return;
        var a = pop._aspect();
        if (!(a > 0)) return;
        pop._shapedToAspect = true;
        var s = VideoWindows.defaultSize(a);
        pop.width = s.width;
        pop.height = s.height;
    }

    Timer {
        id: geometrySave
        interval: 600
        repeat: false
        onTriggered: {
            // Never persist a fullscreen or minimised shape as "the
            // window size" — the same trap main.qml documents for the
            // app window. Only a plain windowed state is a real size.
            if (pop.visibility !== Window.Windowed) return;
            appSettings.setPopoutGeometry(pop._geometryKey,
                                          pop.x, pop.y,
                                          pop.width, pop.height);
        }
    }
    onXChanged: geometrySave.restart()
    onYChanged: geometrySave.restart()
    onWidthChanged: geometrySave.restart()
    onHeightChanged: geometrySave.restart()

    // Closing the window from the title bar is a close request like any
    // other: the owner has to drop it from the pop-out state, or the
    // tile would keep its "popped out" badge over a window that is gone.
    onClosing: function(close) {
        close.accepted = true;
        pop.closeRequested();
    }

    // ── The picture ──────────────────────────────────────────────────
    //
    // Attached once, on creation. Same three-way routing as the in-room
    // tile — our own preview mirrors its controller, a remote stream
    // comes from the registry — kept in one tested place rather than
    // copied a third time (qml/js/VideoWindows.js attachFeed).
    VideoOutput {
        id: output
        anchors.fill: parent
        fillMode: VideoOutput.PreserveAspectFit
        visible: pop._live
        Component.onCompleted: {
            var s = serverManager.activeServer;
            VideoWindows.attachFeed(
                pop.feed, videoSink,
                s ? s.videoRegistry : null,
                (typeof screenShare !== "undefined") ? screenShare : null,
                (typeof camera !== "undefined") ? camera : null);
        }
    }

    // videoSizeChanged is the only honest signal for "we now know the
    // shape of this stream"; it fires once when the first frame is
    // decoded and again if the sender changes resolution mid-share.
    Connections {
        target: output.videoSink
        ignoreUnknownSignals: true
        function onVideoSizeChanged() { pop._shapeToAspect(); }
    }

    Text {
        anchors.centerIn: parent
        visible: !pop._live
        text: VideoStage.placeholderText(pop.feed ? pop.feed.kind : "")
        font.family: Theme.fontSans
        font.pixelSize: Theme.fontSize.md
        color: Theme.fg2
    }

    // ── Toolbar on hover ─────────────────────────────────────────────

    MouseArea {
        id: hover
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        // Double-click is the universal "make this fill the screen" and
        // costs nothing to support here too.
        onDoubleClicked: pop.toggleFullscreen()
    }

    Rectangle {
        id: toolbar
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: Theme.sp.s3
        width: toolbarRow.implicitWidth + Theme.sp.s3 * 2
        height: 34
        radius: Theme.r2
        color: Qt.rgba(0, 0, 0, 0.65)
        opacity: hover.containsMouse || pinHover.containsMouse
                 || fsHover.containsMouse ? 1 : 0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }

        Row {
            id: toolbarRow
            anchors.centerIn: parent
            spacing: Theme.sp.s2

            // Keep on top.
            Rectangle {
                width: 26
                height: 26
                anchors.verticalCenter: parent.verticalCenter
                radius: Theme.r1
                color: pop.pinned ? Theme.accentGlow
                     : (pinHover.containsMouse ? Theme.bg3 : "transparent")
                Icon {
                    anchors.centerIn: parent
                    name: "pin"
                    size: 13
                    color: pop.pinned ? Theme.accent : "white"
                }
                MouseArea {
                    id: pinHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: pop.pinned = !pop.pinned
                }
                ToolTip.visible: pinHover.containsMouse
                ToolTip.text: pop.pinned ? "Don't keep on top" : "Keep on top"
                ToolTip.delay: 400
            }

            // Fullscreen — Feature 1, reused verbatim.
            Rectangle {
                width: 26
                height: 26
                anchors.verticalCenter: parent.verticalCenter
                radius: Theme.r1
                color: fsHover.containsMouse ? Theme.bg3 : "transparent"
                Icon {
                    anchors.centerIn: parent
                    name: "expand"
                    size: 13
                    color: "white"
                }
                MouseArea {
                    id: fsHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: pop.toggleFullscreen()
                }
                ToolTip.visible: fsHover.containsMouse
                ToolTip.text: "Full screen  (F)"
                ToolTip.delay: 400
            }
        }
    }

    // ── Fullscreen, owned by this window ─────────────────────────────
    //
    // VideoWindows.fullscreenOwner() says a feed that is popped out gets
    // its fullscreen HERE rather than from the voice room, so the app can
    // never stack two fullscreen windows of one feed on one screen. This
    // window stays open behind it; the fullscreen window is a third sink
    // on the same stream, which the registry fan-out makes free.
    function toggleFullscreen() {
        fullscreenLoader.active = !fullscreenLoader.active;
    }

    Loader {
        id: fullscreenLoader
        active: false
        sourceComponent: VideoFullscreenWindow {
            feed: pop.feed
            displayName: pop.displayName
            liveTick: pop.liveTick
            onExitRequested: fullscreenLoader.active = false
        }
    }

    Shortcut {
        sequence: "F"
        enabled: !fullscreenLoader.active
        onActivated: pop.toggleFullscreen()
    }
    // Escape closes the pop-out, but only when it is not standing in for
    // the fullscreen window's own Escape (which that window takes first,
    // being the focused one).
    Shortcut {
        sequence: "Escape"
        enabled: !fullscreenLoader.active
        onActivated: pop.closeRequested()
    }
}
