import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import QtQuick.Window
import QtMultimedia
import BSFChat

// VoiceRoom (SPEC §3.3) — the "hero" main-content view when the user is in
// a voice channel. Header + participant grid. Each tile carries an avatar,
// name, peer-state status line, and (for self) a speaking-ring glow driven
// by the outgoing mic level.
//
// When not in a voice channel this component isn't shown; main.qml swaps
// between MessageView and VoiceRoom based on activeServer.inVoiceChannel.
Rectangle {
    id: room
    color: Theme.bg0

    // ── Share-mode state ──────────────────────────────────────────
    // True while anyone (local or any remote peer) is broadcasting
    // a screen share. Triggers the alternate layout: big share
    // viewer on top, compact member strip at the bottom.
    // Bumped whenever a remote peer's screen share state changes,
    // since signals aren't dependency-tracked from property bindings.
    // _peersSharing reads this so it re-evaluates as peers come and go.
    property int _shareTick: 0
    Connections {
        target: serverManager.activeServer
        ignoreUnknownSignals: true
        function onPeerScreenFrameChanged(userId) { room._shareTick++; }
    }
    readonly property var _peersSharing: {
        _shareTick;
        var s = serverManager.activeServer;
        return s ? s.peersCurrentlySharing() : [];
    }
    readonly property bool isSharing:
        (screenShare && screenShare.active) || _peersSharing.length > 0

    // User-toggleable: hide the member strip to give the share even
    // more room. Reset to true on every share-mode transition so a
    // fresh session always starts visible.
    property bool showMembers: true
    onIsSharingChanged: if (isSharing) showMembers = true

    property bool fullscreen: false
    function toggleFullscreen() {
        var w = Window.window;
        if (!w) return;
        if (fullscreen) {
            w.visibility = Window.AutomaticVisibility;
            fullscreen = false;
        } else {
            w.visibility = Window.FullScreen;
            fullscreen = true;
        }
    }
    // Esc exits fullscreen. Only fires while the voice room has
    // focus so we don't intercept Esc elsewhere.
    Shortcut {
        sequence: "Escape"
        enabled: room.fullscreen
        onActivated: room.toggleFullscreen()
    }

    // Header (SPEC §3.3, 56h) — channel name, member count, crypto badge,
    // latency chip. Invite/more buttons are placeholders until the feature
    // set around voice grows.
    Rectangle {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 56
        color: Theme.bg0

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.sp.s8
            anchors.rightMargin: Theme.sp.s8
            spacing: Theme.sp.s5

            Icon {
                name: "volume"
                size: 20
                color: Theme.accent
                Layout.alignment: Qt.AlignVCenter
            }

            Text {
                text: {
                    var s = serverManager.activeServer;
                    if (!s || !s.activeVoiceRoomId) return "";
                    return s.roomListModel
                           ? s.roomListModel.roomDisplayName(s.activeVoiceRoomId)
                           : s.activeVoiceRoomId;
                }
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xl
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackTight.xl
                color: Theme.fg0
                Layout.alignment: Qt.AlignVCenter
            }

            Text {
                text: {
                    var s = serverManager.activeServer;
                    if (!s || !s.voiceMembers) return "";
                    return s.voiceMembers.length + " in call";
                }
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
                Layout.alignment: Qt.AlignVCenter
            }

            Item { Layout.fillWidth: true }

            // Hide-member-strip toggle — only relevant in share mode.
            // Compact ghost button that rhymes with MessageView's
            // chat-header action cluster.
            Rectangle {
                visible: room.isSharing
                Layout.preferredWidth: 30
                Layout.preferredHeight: 30
                Layout.alignment: Qt.AlignVCenter
                radius: Theme.r1
                color: hideMemHover.containsMouse ? Theme.bg3 : "transparent"
                border.color: Theme.line
                border.width: 1
                Icon {
                    anchors.centerIn: parent
                    name: "users"
                    size: 14
                    color: room.showMembers ? Theme.accent : Theme.fg2
                }
                MouseArea {
                    id: hideMemHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: room.showMembers = !room.showMembers
                }
                ToolTip.visible: hideMemHover.containsMouse
                ToolTip.text: room.showMembers
                    ? "Hide member strip" : "Show member strip"
                ToolTip.delay: 400
            }

            Rectangle {
                Layout.preferredWidth: 30
                Layout.preferredHeight: 30
                Layout.alignment: Qt.AlignVCenter
                radius: Theme.r1
                color: fullscreenHover.containsMouse ? Theme.bg3 : "transparent"
                border.color: Theme.line
                border.width: 1
                Icon {
                    anchors.centerIn: parent
                    name: "expand"
                    size: 14
                    color: room.fullscreen ? Theme.accent : Theme.fg2
                }
                MouseArea {
                    id: fullscreenHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: room.toggleFullscreen()
                }
                ToolTip.visible: fullscreenHover.containsMouse
                ToolTip.text: room.fullscreen
                    ? "Exit fullscreen  (Esc)" : "Fullscreen"
                ToolTip.delay: 400
            }

            // Transport badge. We're not running SRTP — audio Opus frames
            // ride an SCTP data channel over libdatachannel's DTLS 1.2
            // handshake. So the truthful label is DTLS · SCTP (encryption
            // terminates at the peer, not end-to-end). Upgrade to SRTP /
            // MLS etc. later means changing the text here too.
            Rectangle {
                implicitWidth: cryptoText.implicitWidth + Theme.sp.s4
                implicitHeight: 22
                radius: Theme.r1
                color: Theme.accentGlow
                Layout.alignment: Qt.AlignVCenter

                Text {
                    id: cryptoText
                    anchors.centerIn: parent
                    text: "DTLS \u00B7 SCTP"
                    font.family: Theme.fontMono
                    font.pixelSize: 11
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackWide.sm
                    color: Theme.accent
                }
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.line
        }
    }

    // ── Share-mode layout ─────────────────────────────────────────
    // When any screen share is in progress, the main column turns
    // into a hero viewer for the share(s) with a compact member
    // strip (hide-able) along the bottom. Hidden when nobody's
    // sharing — the classic participant grid takes over.
    Item {
        id: shareLayout
        visible: room.isSharing
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        // Member strip at the bottom — compact avatar chips with
        // speaking rings. Collapses to 0 height when `showMembers`
        // is off, with a smooth animation so the viewer grows in
        // place rather than popping.
        Rectangle {
            id: memberStrip
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: room.showMembers ? 88 : 0
            visible: height > 0
            color: Theme.bg1
            clip: true
            Behavior on height { NumberAnimation {
                duration: Theme.motion.fastMs
                easing.type: Easing.BezierSpline
                easing.bezierCurve: Theme.motion.bezier
            } }

            Rectangle {
                anchors.top: parent.top
                width: parent.width; height: 1; color: Theme.line
            }

            // Horizontal scroll for overflow, ListView keeps tiles
            // virtualised if the call grows large.
            ListView {
                id: memberStripList
                anchors.fill: parent
                anchors.leftMargin: Theme.sp.s5
                anchors.rightMargin: Theme.sp.s5
                anchors.topMargin: Theme.sp.s3
                anchors.bottomMargin: Theme.sp.s3
                orientation: ListView.Horizontal
                spacing: Theme.sp.s3
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: serverManager.activeServer
                    ? serverManager.activeServer.voiceMembers : []

                delegate: Item {
                    required property var modelData
                    width: 56
                    height: memberStripList.height

                    readonly property bool speaking: modelData.speaking === true
                    readonly property bool muted: modelData.muted === true
                    readonly property bool deafened: modelData.deafened === true
                    readonly property string peerId: modelData.user_id || ""
                    readonly property string peerName:
                        modelData.displayName || modelData.user_id || "?"

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 2

                        // Speaking ring + avatar tile.
                        Item {
                            Layout.preferredWidth: 44
                            Layout.preferredHeight: 44
                            Layout.alignment: Qt.AlignHCenter

                            Rectangle {
                                anchors.centerIn: parent
                                width: parent.width + 6
                                height: parent.height + 6
                                radius: width / 2
                                color: "transparent"
                                border.width: 2
                                border.color: Theme.online
                                opacity: speaking ? 0.9 : 0
                                visible: opacity > 0.01
                                Behavior on opacity { NumberAnimation { duration: 120 } }
                            }

                            Rectangle {
                                anchors.fill: parent
                                radius: Theme.r2
                                color: Theme.senderColor(peerId)
                                Text {
                                    anchors.centerIn: parent
                                    text: (peerName.replace(/^[^a-zA-Z0-9]+/, "")
                                          .charAt(0) || "?").toUpperCase()
                                    font.family: Theme.fontSans
                                    font.pixelSize: 16
                                    font.weight: Theme.fontWeight.semibold
                                    color: Theme.onAccent
                                }
                            }

                            // Status glyph in the bottom-right.
                            Item {
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.margins: -2
                                width: 14; height: 14
                                visible: muted || deafened
                                Rectangle {
                                    anchors.fill: parent
                                    radius: width / 2
                                    color: Theme.danger
                                    border.color: Theme.bg1
                                    border.width: 1.5
                                }
                                Icon {
                                    anchors.centerIn: parent
                                    name: deafened ? "headphones-off" : "mic-off"
                                    size: 8
                                    color: "white"
                                }
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignHCenter
                            horizontalAlignment: Text.AlignHCenter
                            text: peerName
                            font.family: Theme.fontSans
                            font.pixelSize: 10
                            color: Theme.fg2
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }

        // Share viewer area — anchored fill between header and
        // member strip. When local and remote both share we split
        // the area vertically via a Column; the usual case (one
        // source) gets the whole pane.
        Item {
            id: viewerArea
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: memberStrip.top
            anchors.margins: Theme.sp.s5

            readonly property int _sources:
                ((screenShare && screenShare.active) ? 1 : 0)
                + room._peersSharing.length

            // Local-own hero tile.
            Rectangle {
                id: localHero
                visible: screenShare && screenShare.active
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: visible
                    ? (viewerArea._sources > 1
                        ? viewerArea.height / viewerArea._sources - Theme.sp.s3
                        : viewerArea.height)
                    : 0
                radius: Theme.r3
                color: Theme.bg2
                border.color: Theme.accent
                border.width: 1
                clip: true

                VideoOutput {
                    id: heroScreenOutput
                    anchors.fill: parent
                    anchors.margins: 1
                    fillMode: VideoOutput.PreserveAspectFit
                    Component.onCompleted: {
                        if (screenShare && videoSink)
                            screenShare.forwardTo(videoSink);
                    }
                }

                Rectangle {
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.margins: Theme.sp.s3
                    width: heroLabel.implicitWidth + Theme.sp.s3 * 2
                    height: 22
                    radius: Theme.r1
                    color: Theme.accent
                    Text {
                        id: heroLabel
                        anchors.centerIn: parent
                        text: "YOU'RE SHARING"
                        font.family: Theme.fontSans
                        font.pixelSize: 10
                        font.weight: Theme.fontWeight.semibold
                        font.letterSpacing: Theme.trackWidest.xs
                        color: Theme.onAccent
                    }
                }
            }

            // Remote shares — stacked beneath the local hero. Since
            // Column doesn't anchor-fill, we compute explicit
            // heights so the total fills the viewer pane evenly.
            Column {
                id: remoteShareColumn
                anchors.top: localHero.visible ? localHero.bottom : parent.top
                anchors.topMargin: localHero.visible ? Theme.sp.s3 : 0
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                spacing: Theme.sp.s3

                Repeater {
                    model: room._peersSharing
                    delegate: Rectangle {
                        required property string modelData
                        width: remoteShareColumn.width
                        height: {
                            var n = room._peersSharing.length;
                            if (n <= 0) return 0;
                            var total = remoteShareColumn.height
                                      - (n - 1) * Theme.sp.s3;
                            return Math.max(120, total / n);
                        }
                        radius: Theme.r3
                        color: Theme.bg2
                        border.color: Theme.accent
                        border.width: 1
                        clip: true

                        Image {
                            anchors.fill: parent
                            anchors.margins: 1
                            fillMode: Image.PreserveAspectFit
                            smooth: true
                            source: {
                                room._shareTick;
                                var s = serverManager.activeServer;
                                return s ? s.peerScreenDataUrl(modelData) : "";
                            }
                            asynchronous: false
                            cache: false
                        }

                        Rectangle {
                            anchors.top: parent.top
                            anchors.left: parent.left
                            anchors.margins: Theme.sp.s3
                            width: rsLabel.implicitWidth + Theme.sp.s3 * 2
                            height: 22
                            radius: Theme.r1
                            color: Theme.accent
                            Text {
                                id: rsLabel
                                anchors.centerIn: parent
                                text: modelData + " — SCREEN SHARE"
                                font.family: Theme.fontSans
                                font.pixelSize: 10
                                font.weight: Theme.fontWeight.semibold
                                font.letterSpacing: Theme.trackWidest.xs
                                color: Theme.onAccent
                            }
                        }
                    }
                }
            }
        }
    }

    // ── Classic participant grid ──────────────────────────────────
    // Shown when no one is screen-sharing. Auto-columns based on
    // tile+gap widths; tiles wrap to the next row when the main
    // column narrows.
    ScrollView {
        visible: !room.isSharing
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        ScrollBar.vertical: ThemedScrollBar {}
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        Item {
            id: contentColumn
            // Width binds to the ScrollView's viewport; height grows with
            // content. Padding via an inner margin item keeps tiles off
            // the scrollbar without needing to reach into the ScrollBar.
            width: parent.width
            implicitHeight: gridWrapper.implicitHeight + Theme.sp.s8 * 2

            Item {
                id: gridWrapper
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: Theme.sp.s8
                anchors.rightMargin: Theme.sp.s8
                anchors.topMargin: Theme.sp.s8
                anchors.top: parent.top
                implicitHeight: grid.implicitHeight

                Grid {
                    id: grid
                    width: parent.width
                    columnSpacing: Theme.layout.participantGap
                    rowSpacing: Theme.layout.participantGap
                    // Pick as many columns as cleanly fit; at least 1 so a
                    // narrow main column still shows a row of one.
                    columns: Math.max(1,
                        Math.floor((width + Theme.layout.participantGap)
                                   / (Theme.layout.participantTileW
                                      + Theme.layout.participantGap)))

                    Repeater {
                        model: serverManager.activeServer
                               ? serverManager.activeServer.voiceMembers
                               : []
                        delegate: ParticipantTile {
                            required property var modelData
                            member: modelData
                        }
                    }
                }
            }
        }
    }

    // Empty state — shown only when the voice channel has no peers yet.
    // Centered icon + headline + subtext, same vocabulary as the ban /
    // member empty-states in ServerSettings so the app reads consistently.
    ColumnLayout {
        anchors.centerIn: parent
        anchors.verticalCenterOffset: 28  // lift slightly above geometric center
        spacing: Theme.sp.s4
        width: 360
        visible: {
            var s = serverManager.activeServer;
            return !s || !s.voiceMembers || s.voiceMembers.length === 0;
        }

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 72
            Layout.preferredHeight: 72
            radius: Theme.r3
            color: Theme.bg1
            border.color: Theme.line
            border.width: 1

            Icon {
                anchors.centerIn: parent
                name: "volume"
                size: 28
                color: Theme.accent
            }

            // Subtle pulse ring so the empty-state doesn't feel static —
            // reads as "the call is live, waiting."
            Rectangle {
                anchors.centerIn: parent
                width: parent.width
                height: parent.height
                radius: parent.radius
                color: "transparent"
                border.color: Theme.accent
                border.width: 2
                opacity: 0
                SequentialAnimation on opacity {
                    loops: Animation.Infinite
                    running: parent.parent.visible
                    NumberAnimation { to: 0.35; duration: 800; easing.type: Easing.OutQuad }
                    NumberAnimation { to: 0;    duration: 900; easing.type: Easing.InQuad  }
                    PauseAnimation { duration: 300 }
                }
                SequentialAnimation on scale {
                    loops: Animation.Infinite
                    running: parent.parent.visible
                    NumberAnimation { to: 1.25; duration: 1700; easing.type: Easing.OutQuad }
                    PropertyAction   { value: 1.0 }
                }
            }
        }

        Text {
            Layout.alignment: Qt.AlignHCenter
            text: "Waiting for others"
            color: Theme.fg0
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.lg
            font.weight: Theme.fontWeight.semibold
            font.letterSpacing: Theme.trackTight.lg
        }

        Text {
            Layout.alignment: Qt.AlignHCenter
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: "You're in the channel. Others who join will show up here, and the controls below stay available."
            color: Theme.fg2
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.sm
            wrapMode: Text.WordWrap
        }
    }
}
