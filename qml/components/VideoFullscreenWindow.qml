import QtQuick
import QtQuick.Window
import QtMultimedia
import BSFChat

import "../js/VideoStage.js" as VideoStage
import "../js/VideoWindows.js" as VideoWindows

// One video feed filling one whole screen. Black, aspect-fit, no app
// chrome — the thing the owner asked for when they said "make the full
// screen button put the VIDEO as fullscreen, not the window".
//
// WHAT THIS IS NOT. It is not the main window changed state. The old
// header button wrote `Window.window.visibility = Window.FullScreen`,
// which made the whole app — sidebar, channel list, member strip, dock —
// cover the screen with the video still in its little stage panel in the
// middle of it. The main window must not move, resize or change state
// while this is up; tests/test_qml_hygiene.cpp fails if a write to
// another window's `visibility` comes back to the voice-room files.
//
// It is also not the in-room tile reparented. The tile keeps running
// exactly where it is (VoiceRoom moves tiles by geometry and never
// destroys them — SPEC §3.4), and this window attaches a SECOND sink to
// the same stream. VideoStreamRegistry fans one decode out to every
// attached sink and replays the last frame on attach, so opening this is
// not a second decode and closing it cannot blank the tile behind it.
// tests/test_video_registry.cpp pins that.
//
// ── macOS: why these flags, and why no dead Space ────────────────────
//
// Qt has two fullscreens on macOS and they behave completely differently.
//
//   NATIVE fullscreen (the green button / -[NSWindow toggleFullScreen:])
//   moves the window into a Space of its own, with the animation. Qt
//   takes this path in QCocoaWindow only when the window carries
//   Qt::WindowFullscreenButtonHint. It is wrong for us twice over: the
//   Space swipes away from the call the user is still in, and a window
//   destroyed while native-fullscreen is what leaves the empty grey
//   Space behind in Mission Control that has to be cleaned up by hand.
//
//   NON-NATIVE fullscreen — a borderless window resized to the screen
//   with the menu bar and Dock hidden via NSApplicationPresentationOptions
//   — is what Qt does for a window WITHOUT that hint. It stays in the
//   current Space, it cannot strand one, and closing it is just a window
//   closing.
//
// So the flags below are deliberate and load-bearing: Qt.FramelessWindowHint
// (a borderless NSWindow cannot have a fullscreen button at all, which is
// what keeps Qt off the native path) and NO Qt.WindowFullscreenButtonHint.
// `transientParent: null` for the same reason — a transient child is an
// NSWindow attached to its parent with addChildWindow:, which is ordered
// with the parent and cannot own the screen.
//
// Verify on the machine, not from this comment: open fullscreen, close it,
// and check Mission Control has no leftover Space.
Window {
    id: fsWin

    // The feed object from VideoStage.mergeFeeds():
    // { key, userId, kind, streamId, isSelf, started }.
    property var feed: null
    // Resolved by the owner — this component does not know the roster.
    property string displayName: ""
    // Bumped on every video frame signal, same as the tiles. Read
    // through, not stored, so liveness never changes the feed object.
    property int liveTick: 0

    // The user asked to leave. The OWNER closes this window (by dropping
    // the Loader that made it), because the owner is what knows whether
    // anything else should happen — the voice room clears its fullscreen
    // key, a pop-out just returns to its windowed self.
    signal exitRequested()

    readonly property bool _isScreen:
        feed && feed.kind === VideoStage.SCREEN

    readonly property bool _live: {
        fsWin.liveTick;
        if (!fsWin.feed) return false;
        if (fsWin.feed.isSelf) return true;
        var s = serverManager.activeServer;
        return s && s.videoRegistry
            ? s.videoRegistry.hasLiveVideo(fsWin.feed.userId,
                                           fsWin.feed.streamId)
            : false;
    }

    flags: Qt.Window | Qt.FramelessWindowHint
    transientParent: null
    visibility: Window.FullScreen
    visible: true
    color: "black"
    // Frameless, so nothing draws this — but the window still appears in
    // the Window menu and in screen readers, and "BSFChat" alone there
    // while three feeds are open is no help.
    title: VideoWindows.windowTitle(fsWin.displayName,
                                    fsWin.feed ? fsWin.feed.kind : "",
                                    fsWin.feed ? fsWin.feed.isSelf === true
                                               : false)

    // A frameless window is not given focus by the compositor on its own,
    // and without focus the Escape shortcut below never fires — leaving a
    // fullscreen video with no way out but the mouse.
    Component.onCompleted: fsWin.requestActivate()

    // ── The picture ──────────────────────────────────────────────────
    //
    // Attached once, on creation, exactly like VideoFeedTile: `feed` is
    // set by the Loader before this completes and is never reassigned on
    // a live window (a different feed gets a different window), so
    // re-attaching could only ever stack duplicate sinks on one stream.
    VideoOutput {
        id: output
        anchors.fill: parent
        // The whole picture, letterboxed. Cropping somebody's shared
        // screen to fill a 16:10 display would cut off the thing they
        // are pointing at.
        fillMode: VideoOutput.PreserveAspectFit
        visible: fsWin._live
        Component.onCompleted: {
            var s = serverManager.activeServer;
            // typeof, not truthiness: neither capture controller exists
            // on every platform (src/main.cpp), and naming one that is
            // not there is a ReferenceError that would abandon the rest
            // of this handler.
            VideoWindows.attachFeed(
                fsWin.feed, videoSink,
                s ? s.videoRegistry : null,
                (typeof screenShare !== "undefined") ? screenShare : null,
                (typeof camera !== "undefined") ? camera : null);
        }
    }

    // Announced but not yet delivering frames (S-7), or a stream that
    // went quiet. Black with a word on it beats a frozen last frame.
    Text {
        anchors.centerIn: parent
        visible: !fsWin._live
        text: VideoStage.placeholderText(fsWin.feed ? fsWin.feed.kind : "")
        font.family: Theme.fontSans
        font.pixelSize: Theme.fontSize.lg
        color: Theme.fg2
    }

    // ── Chrome that gets out of the way ──────────────────────────────
    //
    // Name, kind and an exit button, fading out after
    // VideoWindows.OVERLAY_IDLE_MS of no mouse movement, along with the
    // cursor. The policy lives in the .js so it is testable; this is a
    // poll rather than a one-shot timer so that the binding below is a
    // real binding on the tested function rather than a second copy of
    // the rule written in QML.
    property real _lastMoveMs: 0
    property bool _chromeVisible:
        VideoWindows.overlayVisible(fsWin._lastMoveMs, idleClock.nowMs)

    Timer {
        id: idleClock
        property real nowMs: 0
        interval: 200
        repeat: true
        running: true
        onTriggered: nowMs = Date.now()
    }

    // Full-window mouse handling: movement wakes the chrome, a
    // double-click leaves. Declared BEFORE the chrome so the exit button
    // is on top of it and gets its own clicks.
    MouseArea {
        id: wake
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        cursorShape: fsWin._chromeVisible ? Qt.ArrowCursor : Qt.BlankCursor
        onPositionChanged: fsWin._lastMoveMs = Date.now()
        onDoubleClicked: fsWin.exitRequested()
        // A single click must do nothing. Clicking to pause/exit is the
        // behaviour of a video player, and this is somebody's live share.
    }

    // Identity, top-left.
    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: Theme.sp.s5
        width: fsLabel.implicitWidth + Theme.sp.s5 * 2
        height: 34
        radius: Theme.r2
        color: Qt.rgba(0, 0, 0, 0.6)
        opacity: fsWin._chromeVisible ? 1 : 0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }

        Text {
            id: fsLabel
            anchors.centerIn: parent
            text: VideoWindows.fullscreenLabel(
                fsWin.displayName
                    || (fsWin.feed ? fsWin.feed.userId : ""),
                fsWin.feed ? fsWin.feed.kind : "",
                fsWin.feed ? fsWin.feed.isSelf === true : false)
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.sm
            font.weight: Theme.fontWeight.semibold
            color: "white"
        }
    }

    // Exit, top-right.
    Rectangle {
        id: exitButton
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.sp.s5
        width: exitRow.implicitWidth + Theme.sp.s5 * 2
        height: 34
        radius: Theme.r2
        color: exitHover.containsMouse ? Theme.accent : Qt.rgba(0, 0, 0, 0.6)
        opacity: fsWin._chromeVisible ? 1 : 0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }
        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

        Row {
            id: exitRow
            anchors.centerIn: parent
            spacing: Theme.sp.s2
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                name: "x"
                size: 13
                color: exitHover.containsMouse ? Theme.onAccent : "white"
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Exit full screen  (Esc)"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                font.weight: Theme.fontWeight.semibold
                color: exitHover.containsMouse ? Theme.onAccent : "white"
            }
        }

        MouseArea {
            id: exitHover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: fsWin.exitRequested()
            // Moving onto the button counts as movement, or the chrome
            // would fade out from under the pointer resting on it.
            onPositionChanged: fsWin._lastMoveMs = Date.now()
            onContainsMouseChanged: fsWin._lastMoveMs = Date.now()
        }
    }

    // Escape and F both leave. Qt.WindowShortcut (the default) scopes
    // these to THIS window, so they cannot fire in the app window behind.
    Shortcut {
        sequences: ["Escape", "F"]
        onActivated: fsWin.exitRequested()
    }
}
