import QtQuick
import QtMultimedia
import BSFChat

import "../js/VideoStage.js" as VideoStage

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
    // True while this feed is the one on the main stage. Drives the
    // highlight and the fill mode; changing it must never cost a frame.
    //
    // NOT named `onStage`: QML parses any `on` + Capital identifier as a
    // signal handler, so a property with that name is a load error that
    // takes the whole component — and everything importing it — with it.
    // That is the rc.6 Theme.onScrim bug; tests/test_qml_hygiene.cpp
    // exists because of it.
    property bool featured: false
    // Strip presentation: smaller type, no diagnostics, click target.
    property bool compact: false
    // Bumped by VoiceRoom on every video frame signal. Liveness is read
    // through this rather than stored on the feed, so a stream going
    // live does not change the feed list and does not rebuild delegates.
    property int liveTick: 0

    signal clicked()

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
            if (!tile.feed || !videoSink) return;
            if (tile.feed.isSelf) {
                // typeof, not truthiness: neither controller exists on
                // every platform (src/main.cpp), and naming one that is
                // not there is a ReferenceError, not undefined.
                if (tile.isScreen) {
                    if (typeof screenShare !== "undefined" && screenShare)
                        screenShare.forwardTo(videoSink);
                } else if (typeof camera !== "undefined" && camera) {
                    camera.forwardTo(videoSink);
                }
                return;
            }
            var s = serverManager.activeServer;
            if (s && s.videoRegistry)
                s.videoRegistry.attachOutput(tile.feed.userId,
                                             tile.feed.streamId, videoSink);
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
            radius: width / 2
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
                    + " · " + st.codec;
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

    // ── Picking ──────────────────────────────────────────────────────
    //
    // Clicking a thumbnail promotes it to the stage. Clicking the stage
    // tile only takes keyboard focus, so the arrow keys work without
    // first hunting for something to click.
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: tile.compact ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: tile.clicked()
        // A quiet lift on hover — enough to read as clickable without
        // competing with the selection highlight.
        onContainsMouseChanged: hoverWash.opacity =
            (containsMouse && tile.compact && !tile.featured) ? 0.10 : 0
    }

    Rectangle {
        id: hoverWash
        anchors.fill: parent
        radius: parent.radius
        color: Theme.accent
        opacity: 0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }
    }
}
