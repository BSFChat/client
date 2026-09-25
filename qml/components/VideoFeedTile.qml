import QtQuick
import QtQuick.Controls
import QtMultimedia
import BSFChat

import "../js/VideoStage.js" as VideoStage
import "../js/VideoWindows.js" as VideoWindows

// One video surface in the voice room: a remote screen share, a remote
// camera, our own camera, or our own screen share.
//
// READ THIS BEFORE MOVING ANYTHING. The whole point of this component is
// that it is the ONLY place a voice-room VideoOutput is declared, and
// that one instance serves both positions a feed can be in — filling the
// main stage, or shrunk into the bottom strip. VoiceRoom drives that with
// geometry alone (x/y/width/height and `compact`), never by reparenting
// and never by swapping delegates, because a VideoOutput that is
// destroyed and rebuilt comes back empty and the picture blinks. The
// sink is attached exactly once, in Component.onCompleted.
//
// The three sources are wired differently and there is no way around it:
// the local camera and the local screen capture mirror their controller's
// preview sink (`forwardTo`), while every remote stream comes out of
// ServerConnection.videoRegistry, which fans one per-peer sink out to any
// number of attached outputs and replays the last frame on attach — so a
// tile that appears mid-stream is never blank waiting for the next
// keyframe.
Rectangle {
    id: tile

    // The feed object from VideoStage.mergeFeeds():
    // { key, userId, kind, streamId, isSelf, started }.
    required property var feed
    // Resolved elsewhere — this component does not know the roster.
    property string displayName: ""
    // True while this feed is the one the user EXPANDED to fill the
    // stage. Drives the accent highlight; changing it must never cost a
    // frame. False for every tile in the grid, on purpose — see the
    // binding in VoiceRoom.qml.
    //
    // NOT named `onStage`: QML parses any `on` + Capital identifier as a
    // signal handler, so a property with that name is a load error that
    // takes the whole component — and everything importing it — with it.
    // That is the rc.6 Theme.onScrim bug; tests/test_qml_hygiene.cpp
    // exists because of it.
    property bool featured: false
    // Strip presentation: smaller type, no diagnostics, cropped fill.
    // Grid tiles are NOT compact however small the grid gets — a grid
    // tile is a real view of the feed and is aspect-fit like the stage.
    property bool compact: false
    // Bumped by VoiceRoom on every video frame signal. Liveness is read
    // through this rather than stored on the feed, so a stream going
    // live does not change the feed list and does not rebuild delegates.
    property int liveTick: 0
    // This feed also has a pop-out window open. The tile STAYS LIVE —
    // VideoStreamRegistry fans one decoded stream out to every attached
    // sink, so the window is a second surface and not a theft — and all
    // this does is say so, because a feed that is on screen twice is
    // otherwise baffling.
    property bool poppedOut: false

    // The pointer is somewhere on this tile — including on one of the
    // hover buttons, which take hover away from the tile's own
    // MouseArea. VoiceRoom reads it so that `F` in the grid means "the
    // one I am looking at". Purely advisory: it changes no layout.
    readonly property bool hovered: tileMouse.containsMouse
                                    || popHover.containsMouse
                                    || fullHover.containsMouse

    // Room enough for the hover buttons and the diagnostics line. A
    // nine-up grid on a small window makes tiles a couple of hundred
    // pixels wide, and two 26px buttons plus a mono stats line over one
    // of those is more chrome than picture. The context menu still
    // reaches every action, so nothing becomes unreachable.
    readonly property bool _roomyEnoughForChrome: tile.width >= 220
                                                  && tile.height >= 120

    signal clicked()
    // The hover actions and the context menu. The tile does not act on
    // any of these itself: which window owns a feed's fullscreen, and
    // whether a pop-out opens or merely raises, is policy that lives in
    // qml/js/VideoWindows.js and is applied by VoiceRoom.
    signal fullscreenRequested()
    signal popOutRequested()
    signal closePopOutRequested()

    readonly property bool isScreen: feed && feed.kind === VideoStage.SCREEN

    // Our own preview is live as soon as the controller is active — the
    // local sink is already producing frames by then. For a remote peer
    // this is the S-7 distinction: the roster can announce a stream
    // seconds before (or after) frames actually arrive, and the
    // placeholder belongs to exactly that gap.
    readonly property bool live: {
        tile.liveTick;
        if (!tile.feed) return false;
        if (tile.feed.isSelf) return true;
        var s = serverManager.activeServer;
        return s && s.videoRegistry
            ? s.videoRegistry.hasLiveVideo(tile.feed.userId, tile.feed.streamId)
            : false;
    }

    radius: tile.compact ? Theme.r2 : Theme.r3
    color: Theme.bg2
    clip: true
    // The highlight is the only thing that says which tile is on the
    // stage. Width stays constant so the content box doesn't resize with
    // selection — the same reason ParticipantTile pins its border width.
    border.width: 2
    border.color: tile.featured ? Theme.accent : Theme.line
    Behavior on border.color { ColorAnimation { duration: Theme.motion.fastMs } }

    // Geometry is animated so a feed starting or stopping slides the
    // others into place instead of teleporting them. VoiceRoom writes
    // x/y/width/height directly; these smooth the write.
    Behavior on x      { NumberAnimation { duration: Theme.motion.fastMs } }
    Behavior on y      { NumberAnimation { duration: Theme.motion.fastMs } }
    Behavior on width  { NumberAnimation { duration: Theme.motion.fastMs } }
    Behavior on height { NumberAnimation { duration: Theme.motion.fastMs } }

    // ── The picture ──────────────────────────────────────────────────
    //
    // Attached once. `feed` is `required` and never reassigned on a live
    // tile (VoiceRoom's Repeater rebuilds delegates only when the feed
    // LIST changes), so Component.onCompleted is the right and only
    // hook — re-running it would stack duplicate outputs on the sink.
    VideoOutput {
        id: output
        anchors.fill: parent
        anchors.margins: 2
        // On the stage the whole picture matters, letterboxed. In the
        // strip a legible crop beats a postage stamp in a black box.
        fillMode: tile.compact ? VideoOutput.PreserveAspectCrop
                               : VideoOutput.PreserveAspectFit
        visible: tile.live
        Component.onCompleted: {
            var s = serverManager.activeServer;
            // The three-way routing (our own preview mirrors its
            // controller, a remote stream comes from the registry) is in
            // qml/js/VideoWindows.js because the pop-out and fullscreen
            // windows need exactly the same rule, and a self-feed sent
            // down the remote path is a permanently black surface.
            //
            // typeof, not truthiness: neither controller exists on every
            // platform (src/main.cpp), and naming one that is not there
            // is a ReferenceError, not undefined.
            VideoWindows.attachFeed(
                tile.feed, videoSink,
                s ? s.videoRegistry : null,
                (typeof screenShare !== "undefined") ? screenShare : null,
                (typeof camera !== "undefined") ? camera : null);
        }
    }

    // ── Announced, not yet live (S-7) ────────────────────────────────
    //
    // Avatar initial plus a status line, same vocabulary as the
    // participant tiles. Only a remote feed can be in this state.
    Column {
        anchors.centerIn: parent
        spacing: Theme.sp.s3
        visible: !tile.live

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: tile.compact ? Theme.avatar.md : Theme.avatar.xl
            height: width
            // Rounded square like every other avatar; the radius tracks the
            // size band the tile is in, r2 for avatar.md and r3 for avatar.xl,
            // which is the pairing the rest of the tree already uses.
            radius: tile.compact ? Theme.r2 : Theme.r3
            color: Theme.senderColor(tile.feed ? tile.feed.userId : "")
            Text {
                anchors.centerIn: parent
                text: {
                    var n = tile.displayName
                        || (tile.feed ? tile.feed.userId : "");
                    var stripped = n.replace(/^[^a-zA-Z0-9]+/, "");
                    return (stripped.length > 0
                            ? stripped.charAt(0) : "?").toUpperCase();
                }
                font.family: Theme.fontSans
                font.pixelSize: tile.compact ? 14 : 28
                font.weight: Theme.fontWeight.semibold
                color: Theme.onAccent
            }
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: !tile.compact
            text: VideoStage.placeholderText(tile.feed ? tile.feed.kind : "")
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.sm
            color: Theme.fg2
        }
    }

    // ── Identity pill ────────────────────────────────────────────────
    Rectangle {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: tile.compact ? Theme.sp.s2 : Theme.sp.s3
        width: Math.min(parent.width - Theme.sp.s3 * 2,
                        nameLabel.implicitWidth + Theme.sp.s3 * 2)
        height: tile.compact ? 18 : 22
        radius: Theme.r1
        color: tile.featured ? Theme.accent : Qt.rgba(0, 0, 0, 0.6)
        Text {
            id: nameLabel
            anchors.centerIn: parent
            width: Math.min(implicitWidth, parent.width - Theme.sp.s3 * 2)
            elide: Text.ElideRight
            text: VideoStage.feedLabel(
                tile.displayName || (tile.feed ? tile.feed.userId : ""),
                tile.feed ? tile.feed.kind : "",
                tile.feed ? tile.feed.isSelf === true : false)
            font.family: Theme.fontSans
            font.pixelSize: tile.compact ? 9 : 10
            font.weight: Theme.fontWeight.semibold
            font.letterSpacing: Theme.trackWidest.xs
            color: tile.featured ? Theme.onAccent : "white"
        }
    }

    // ── "Nobody is receiving this" ───────────────────────────────────
    //
    // We are capturing but no peer channel is carrying the frames.
    // `=== false` keeps this hidden on builds whose controller does not
    // expose `transmitting`.
    Rectangle {
        id: notVisibleBadge
        visible: !tile.compact && tile.feed && tile.feed.isSelf === true
                 && (tile.isScreen
                     ? (typeof screenShare !== "undefined" && screenShare
                        && screenShare.active
                        && screenShare.transmitting === false)
                     : (typeof camera !== "undefined" && camera
                        && camera.active
                        && camera.transmitting === false))
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.margins: Theme.sp.s3
        width: notVisibleRow.implicitWidth + Theme.sp.s3 * 2
        height: 22
        radius: Theme.r1
        color: Theme.bg1
        border.color: Theme.warn
        border.width: 1
        Row {
            id: notVisibleRow
            anchors.centerIn: parent
            spacing: Theme.sp.s2
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                name: "bolt"
                size: 10
                color: Theme.warn
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                // Alone in the channel is not a fault, it is an empty
                // room. Only claim a transmission problem when there is
                // somebody who should be seeing this.
                text: {
                    var s = serverManager.activeServer;
                    var members = s && s.voiceMembers ? s.voiceMembers.length : 0;
                    return members <= 1
                        ? "No one else is in the channel"
                        : "Not visible to others";
                }
                font.family: Theme.fontSans
                font.pixelSize: 10
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWide.sm
                color: Theme.warn
            }
        }
    }

    // ── Diagnostics overlay (Settings → Advanced) ────────────────────
    //
    // Cumulative receive counters polled once a second and diffed into
    // rates. fps is DECODED fps — what the viewer actually gets. Remote
    // only: there is no receive path for our own preview. Stage only:
    // the text does not fit a thumbnail and the stage feed is the one
    // being diagnosed.
    Rectangle {
        id: diagOverlay
        visible: appSettings.showVideoDiagnostics && !tile.compact
                 && tile._roomyEnoughForChrome
                 && tile.live && tile.feed && tile.feed.isSelf !== true
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: Theme.sp.s3
        color: "#c0000000"
        radius: Theme.r1
        width: diagText.implicitWidth + Theme.sp.s3 * 2
        height: diagText.implicitHeight + Theme.sp.s2 * 2

        property var _prev: null
        property string statsLine: "measuring…"

        Timer {
            running: diagOverlay.visible
            interval: 1000
            repeat: true
            triggeredOnStart: true
            onTriggered: {
                var s = serverManager.activeServer;
                if (!s || !tile.feed) return;
                var st = s.videoReceiveStats(tile.feed.userId,
                                             tile.feed.streamId);
                if (!st || st.rxFrames === undefined) {
                    diagOverlay.statsLine = "no stream data";
                    diagOverlay._prev = null;
                    return;
                }
                var now = Date.now();
                var p = diagOverlay._prev;
                diagOverlay._prev = {
                    t: now,
                    decoded: st.decodedFrames,
                    bytes: st.rxBytes,
                };
                if (!p) return;
                var dt = (now - p.t) / 1000;
                if (dt <= 0) return;
                var fps = Math.max(0, (st.decodedFrames - p.decoded) / dt);
                var kbps = Math.max(0, (st.rxBytes - p.bytes) * 8 / dt / 1000);
                // Uncompressed I420 at this res/fps vs received bits =
                // compression ratio.
                var rawKbps = fps * st.width * st.height * 1.5 * 8 / 1000;
                var ratio = kbps > 0 ? rawKbps / kbps : 0;
                diagOverlay.statsLine =
                    st.width + "x" + st.height
                    + " @ " + fps.toFixed(0) + " fps"
                    + " · " + (kbps >= 1000
                        ? (kbps / 1000).toFixed(1) + " Mbps"
                        : kbps.toFixed(0) + " kbps")
                    + " · " + (ratio > 0 ? ratio.toFixed(0) + ":1" : "–")
                    + " · drops " + st.droppedAus
                    + " · " + st.codec
                    // Playout smoothing: how long the last frame was
                    // held against the measured arrival jitter.
                    + (st.smoothingMaxMs > 0
                        ? " · held " + st.smoothingHeldMs + "/"
                          + st.smoothingMaxMs + " ms (jitter "
                          + st.jitterMs + ")"
                        : " · smoothing off");
            }
        }

        Text {
            id: diagText
            anchors.centerIn: parent
            text: diagOverlay.statsLine
            color: "#e0ffffff"
            font.family: Theme.fontMono
            font.pixelSize: Theme.fontSize.xs
        }
    }

    // ── "Popped out" badge ───────────────────────────────────────────
    //
    // Top-left, under the "not visible to others" warning when that is
    // also up (our own feed can be popped out too). Never on a strip
    // thumbnail: there is no room, and the pop-out window itself is the
    // more obvious evidence.
    Rectangle {
        visible: tile.poppedOut && !tile.compact
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.leftMargin: Theme.sp.s3
        anchors.topMargin: Theme.sp.s3
            + (notVisibleBadge.visible ? 22 + Theme.sp.s2 : 0)
        width: poppedRow.implicitWidth + Theme.sp.s3 * 2
        height: 22
        radius: Theme.r1
        color: Theme.accentGlow
        border.color: Theme.accent
        border.width: 1
        Row {
            id: poppedRow
            anchors.centerIn: parent
            spacing: Theme.sp.s2
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                name: "app-window"
                size: 10
                color: Theme.accent
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "POPPED OUT"
                font.family: Theme.fontSans
                font.pixelSize: 10
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWide.sm
                color: Theme.accent
            }
        }
    }

    // ── Picking ──────────────────────────────────────────────────────
    //
    // EVERY tile is clickable now, in both directions: a tile in the
    // grid expands to fill the stage, the expanded tile collapses back
    // to the grid, and a strip thumbnail takes the stage from whatever
    // is on it. VoiceRoom decides which of those a click means
    // (VideoStage.toggleExpanded); the tile only reports it. That is
    // why the cursor is a hand everywhere and no longer only over the
    // strip.
    //
    // Right-click anywhere on a tile opens the same actions the hover
    // buttons offer, because the buttons are too small to be the only
    // route to them — and on a small grid tile they are not drawn at
    // all.
    MouseArea {
        id: tileMouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: Qt.PointingHandCursor
        onClicked: function(mouse) {
            if (mouse.button === Qt.RightButton) {
                feedMenu.popup();
                return;
            }
            tile.clicked();
        }
        // The same menu the right-click opens. Without it a phone had no
        // route to "Full screen" at all: the corner buttons below are
        // revealed by hover, and there is no hover.
        onPressAndHold: feedMenu.popup()
    }

    // ── Hover actions ────────────────────────────────────────────────
    //
    // Bottom-right, the one free corner: the identity pill is
    // bottom-left, the diagnostics overlay top-right, the transmit
    // warning top-left. Declared AFTER the picking MouseArea so these
    // get their own clicks rather than promoting the tile to the stage.
    Row {
        id: actions
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: tile.compact ? Theme.sp.s2 : Theme.sp.s3
        spacing: Theme.sp.s2
        // Strip thumbnails are a few dozen pixels tall, and a tile in a
        // nine-up grid on a small window is not much better; two 26px
        // buttons would cover the picture. The context menu still
        // reaches them.
        visible: !tile.compact && tile._roomyEnoughForChrome
        // Permanently out on touch. Reveal-on-hover is not a dimmer there,
        // it is an off switch, and what it was switching off is the only
        // control that makes somebody else's camera readable on a phone.
        opacity: Theme.isMobile || tileMouse.containsMouse
                 || popHover.containsMouse
                 || fullHover.containsMouse ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }

        Rectangle {
            // Pop-out opens a second OS window. There is no such thing on a
            // phone, so the button is not there either — which also gives
            // the fullscreen button beside it room to be finger-sized.
            visible: !Theme.isMobile
            width: 26
            height: 26
            radius: Theme.r1
            color: popHover.containsMouse ? Theme.accent : Qt.rgba(0, 0, 0, 0.6)
            Icon {
                anchors.centerIn: parent
                name: tile.poppedOut ? "x" : "app-window"
                size: 13
                color: popHover.containsMouse ? Theme.onAccent : "white"
            }
            MouseArea {
                id: popHover
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: tile.poppedOut ? tile.closePopOutRequested()
                                          : tile.popOutRequested()
            }
            ToolTip.visible: popHover.containsMouse
            ToolTip.text: tile.poppedOut ? "Close pop-out window" : "Pop out"
            ToolTip.delay: 400
        }

        Rectangle {
            // 44 pt on touch. The tile only draws this chrome at all above
            // 220 px wide (tile._roomyEnoughForChrome), so a finger-sized
            // button here cannot swallow a thumbnail.
            width: Theme.isMobile ? 44 : 26
            height: Theme.isMobile ? 44 : 26
            radius: Theme.r1
            color: fullHover.containsMouse ? Theme.accent : Qt.rgba(0, 0, 0, 0.6)
            Accessible.role: Accessible.Button
            Accessible.name: "Full screen"
            Accessible.onPressAction: tile.fullscreenRequested()
            Icon {
                anchors.centerIn: parent
                name: "expand"
                size: Theme.isMobile ? 18 : 13
                color: fullHover.containsMouse ? Theme.onAccent : "white"
            }
            MouseArea {
                id: fullHover
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: tile.fullscreenRequested()
            }
            ToolTip.visible: fullHover.containsMouse
            ToolTip.text: "Full screen this video"
            ToolTip.delay: 400
        }
    }

    Menu {
        id: feedMenu
        MenuItem {
            text: "Full screen"
            onTriggered: tile.fullscreenRequested()
        }
        MenuItem {
            // No second window to pop out into on a phone. A menu item that
            // cannot do anything is worse than a shorter menu.
            visible: !Theme.isMobile
            height: visible ? implicitHeight : 0
            text: tile.poppedOut ? "Close pop-out window" : "Pop out"
            onTriggered: tile.poppedOut ? tile.closePopOutRequested()
                                        : tile.popOutRequested()
        }
    }

    // A quiet lift on hover — enough to read as clickable without
    // competing with the expanded feed's accent highlight. A BINDING,
    // not a write from onContainsMouseChanged: clicking the expanded
    // tile collapses it under a pointer that never moved, and an
    // imperative wash would sit at the wrong value until the mouse left
    // and came back.
    Rectangle {
        id: hoverWash
        anchors.fill: parent
        radius: parent.radius
        color: Theme.accent
        opacity: (tileMouse.containsMouse && !tile.featured) ? 0.10 : 0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }
    }
}
