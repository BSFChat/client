import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import QtQuick.Window
import QtMultimedia
import BSFChat

import "../js/VideoStage.js" as VideoStage

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

    // ── Video state ───────────────────────────────────────────────
    // Bumped whenever any video frame signal fires. Liveness — frames
    // actually arriving, as opposed to a stream merely announced (S-7) —
    // is read THROUGH this tick inside the tiles rather than stored on
    // the feed list: signals aren't dependency-tracked from property
    // bindings, and a feed list whose identity changed on every frame
    // would rebuild every delegate.
    property int _shareTick: 0

    // The ordered feed list: every remote screen share, every remote
    // camera, and our own camera / screen share while they are on. One
    // entry per surface, so a peer who is sharing AND on camera is two.
    //
    // STORED, not a binding, and reassigned only when MEMBERSHIP
    // changes. This is what replaces _peersSharing and it inherits that
    // property's hard-won discipline: a binding that rebuilt the array
    // on every tick handed the Repeater a new model object each time,
    // and a Repeater destroys and recreates every delegate when its
    // model identity changes — so each tile, and the VideoOutput inside
    // it, was thrown away and rebuilt several times a second during a
    // call (S-13). VideoStage.sameFeeds() is the membership comparison
    // that keeps the identity; _refreshFeeds() assigns only on a real
    // change.
    property var _feeds: []
    // Monotonic stamp handed to new feeds so "most recently started"
    // means something. See qml/js/VideoStage.js.
    property int _feedSeq: 0

    // The user's explicit pick from the bottom strip. "" means auto —
    // the most recently started screen share, else the most recently
    // started camera. Escape returns here, and so does a pick whose feed
    // stops.
    property string _selectedKey: ""

    // The capture controllers are context properties that only exist on
    // platforms that have the capture path (see src/main.cpp) — there is
    // no `screenShare` on iOS and no `camera` in a build without voice.
    // Naming one that isn't there throws a ReferenceError and takes the
    // rest of the expression with it, which in _refreshFeeds() would mean
    // abandoning the feed list half-built. Same `typeof` guard VoiceDock
    // uses for its buttons.
    readonly property bool _canShareScreen: typeof screenShare !== "undefined"
                                            && screenShare !== null
    readonly property bool _canUseCamera: typeof camera !== "undefined"
                                          && camera !== null

    readonly property string _stageMode:
        VideoStage.stageMode(_feeds, _selectedKey)
    readonly property int _stageCount:
        VideoStage.stageCount(_feeds, _selectedKey)
    // The strip only earns its space when there is a choice to make —
    // that is, when a screen share has pushed the other feeds off the
    // stage. Cameras on their own are all on the stage already, and a
    // strip under them would be a second copy of the same pictures.
    readonly property bool _stripVisible: _stageMode === "focus"

    function _refreshFeeds() {
        var raw = [];
        var s = serverManager.activeServer;
        var i;
        if (s) {
            var reg = s.videoRegistry;
            // Remote screen shares. S-7: the sender told us this stream
            // stopped. The roster's announced flag lags by a poll, and
            // honouring it here is what kept a dead share on screen as a
            // "Starting share…" placeholder for seconds after it ended.
            var sharers = s.peersCurrentlySharing();
            for (i = 0; i < sharers.length; ++i) {
                if (reg && reg.streamStopped(sharers[i], 0)) continue;
                raw.push({ userId: sharers[i], kind: VideoStage.SCREEN,
                           isSelf: false });
            }
            // Remote cameras, off the voice roster. Self is excluded
            // here deliberately: our own preview comes from the camera
            // controller's sink and never through the remote-peer path,
            // and including ourselves would conjure a frameless "remote"
            // tile for our own face.
            var members = s.voiceMembers || [];
            for (i = 0; i < members.length; ++i) {
                var uid = members[i].user_id || "";
                if (!uid || uid === s.userId) continue;
                if (!s.peerHasCamera(uid)) continue;
                if (reg && reg.streamStopped(uid, 1)) continue;
                raw.push({ userId: uid, kind: VideoStage.CAMERA,
                           isSelf: false });
            }
            // Our own feeds are first-class. The owner asked to see
            // everyone "including self", and a self-view is also the
            // only way to notice the camera is pointing at the ceiling.
            var me = s.userId || "";
            if (me) {
                if (room._canShareScreen && screenShare.active)
                    raw.push({ userId: me, kind: VideoStage.SCREEN,
                               isSelf: true });
                if (room._canUseCamera && camera.active)
                    raw.push({ userId: me, kind: VideoStage.CAMERA,
                               isSelf: true });
            }
        }

        var next = VideoStage.mergeFeeds(room._feeds, raw, room._feedSeq + 1);
        if (VideoStage.sameFeeds(room._feeds, next)) return;
        room._feedSeq++;
        room._feeds = next;
        // A pick whose feed has gone falls back to auto rather than
        // leaving the stage blank.
        if (room._selectedKey && !VideoStage.hasKey(next, room._selectedKey))
            room._selectedKey = "";
    }

    // The roster is the only place a display name lives; the feed
    // objects stay free of anything that can change under them.
    function _displayNameFor(userId) {
        var s = serverManager.activeServer;
        if (!s) return userId;
        if (userId === s.userId) return s.displayName || userId;
        var members = s.voiceMembers || [];
        for (var i = 0; i < members.length; ++i) {
            if ((members[i].user_id || "") === userId)
                return members[i].displayName || userId;
        }
        return userId;
    }

    Connections {
        target: serverManager.activeServer
        ignoreUnknownSignals: true
        function onPeerScreenFrameChanged(userId) {
            room._shareTick++;
            feedRescan.nudge();
        }
        function onPeerCameraFrameChanged(userId) {
            room._shareTick++;
            feedRescan.nudge();
        }
        // A peer switching their camera on shows up as a roster change
        // before a single frame arrives — that is what puts the
        // "Starting camera…" placeholder on the stage on time.
        function onVoiceMembersChanged() { room._refreshFeeds(); }
    }
    Connections {
        target: serverManager
        ignoreUnknownSignals: true
        function onActiveServerChanged() {
            room._shareTick++;
            room._refreshFeeds();
        }
    }
    // The local capture controllers are context properties of their own,
    // not part of the server connection, so their signals are what tells
    // us our own feeds came or went.
    Connections {
        target: room._canUseCamera ? camera : null
        ignoreUnknownSignals: true
        function onActiveChanged() { room._refreshFeeds(); }
    }
    Connections {
        target: room._canShareScreen ? screenShare : null
        ignoreUnknownSignals: true
        function onActiveChanged() { room._refreshFeeds(); }
    }
    // A stream can start arriving without ever having been announced, so
    // frames have to be able to add a feed. But frame signals fire per
    // frame per peer — a roomful of 30 fps cameras is hundreds a second —
    // and rebuilding the list means a roster walk and a sorted set each
    // time. Liveness stays immediate (that is just _shareTick); the
    // membership rescan is coalesced to at most one per interval.
    //
    // start(), NOT restart(): restarting on every frame would push the
    // deadline back forever and the rescan would never run while anything
    // was streaming.
    Timer {
        id: feedRescan
        interval: 250
        repeat: false
        onTriggered: room._refreshFeeds()
        function nudge() { if (!running) start(); }
    }

    Component.onCompleted: room._refreshFeeds()

    // Any video at all — screen or camera, local or remote — takes over
    // the main column. The name predates cameras being part of it; the
    // header toggle and the classic grid both read it.
    readonly property bool isSharing: _feeds.length > 0

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

            // Transport badge. The label is NOT written here and must
            // never be. It used to be a hard-coded literal naming the
            // mesh transport, which had no C++ backing at all — so a
            // call carried by the SFU would have gone on displaying the
            // mesh protocol.
            //
            // It now mirrors ServerConnection.voiceProtectionBadge,
            // which returns voice::protectionBadge() for whichever
            // transport actually started. src/voice/VoiceEncryption.h is
            // the single home for every word this app says about voice
            // security; read it before changing anything here. A test
            // (test_voice_encryption, qmlHoldsNoHardCodedSecurityLabel)
            // fails if such a string reappears as a literal under qml/.
            //
            // Empty until a transport has started, so the badge is
            // absent while connecting rather than asserting protection
            // the call does not have yet.
            Rectangle {
                visible: cryptoText.text.length > 0
                implicitWidth: cryptoText.implicitWidth + Theme.sp.s4
                implicitHeight: 22
                radius: Theme.r1
                color: Theme.accentGlow
                Layout.alignment: Qt.AlignVCenter

                Text {
                    id: cryptoText
                    anchors.centerIn: parent
                    text: serverManager.activeServer
                        ? serverManager.activeServer.voiceProtectionBadge : ""
                    font.family: Theme.fontMono
                    font.pixelSize: 11
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackWide.sm
                    color: Theme.accent
                }

                // The badge on its own is jargon. The tooltip is where
                // the limitation gets stated — also straight from
                // VoiceEncryption, never composed here, never trimmed
                // to fit. `contentWidth` is what makes the Basic
                // style's already-Text.Wrap content actually wrap; the
                // detail strings are two paragraphs and would otherwise
                // lay out as one screen-wide line.
                MouseArea {
                    id: cryptoHover
                    anchors.fill: parent
                    hoverEnabled: true
                }
                ToolTip {
                    visible: cryptoHover.containsMouse && text.length > 0
                    text: serverManager.activeServer
                        ? serverManager.activeServer.voiceProtectionDetail : ""
                    delay: 400
                    contentWidth: 320
                    // Right-aligned to the badge instead of the default
                    // centre: the badge sits at the right edge of the
                    // header, and a 320-wide tooltip centred on it would
                    // hang off the window.
                    x: parent.width - width
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

    // ── Video layout ──────────────────────────────────────────────
    // When anyone has a camera or a screen share up, the main column
    // becomes a stage with the audio member strip along the bottom and,
    // when there is a choice to make, a strip of feed thumbnails above
    // it. Hidden when there is no video — the classic participant grid
    // takes over.
    //
    // THE ONE RULE HERE: there is exactly one tile per feed, and it is
    // the same item whether it is filling the stage or sitting in the
    // strip. The Repeater is driven by _feeds, which changes only on
    // membership; picking a different feed rewrites x/y/width/height and
    // nothing else. Reparenting the tile, or splitting stage and strip
    // into two Repeaters, would destroy and rebuild the VideoOutput and
    // the picture would blink every time somebody clicked a thumbnail.
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

        // Stage + thumbnail strip. One coordinate space for both, so a
        // feed moving between them is an animated resize rather than a
        // change of parent.
        Item {
            id: feedArea
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: memberStrip.top
            anchors.margins: Theme.sp.s5

            readonly property int gap: Theme.sp.s3
            readonly property int stripH: room._stripVisible ? 92 : 0
            readonly property int stageH:
                height - (stripH > 0 ? stripH + gap : 0)
            readonly property int thumbMaxW: 168

            // Keyboard picking. Scoped to focus rather than a Shortcut
            // on purpose: a window-wide Left/Right would fire inside
            // every settings dialog and text field in the app.
            focus: true
            Keys.onLeftPressed: function(event) {
                room._selectedKey =
                    VideoStage.moveSelection(room._feeds, room._selectedKey, -1);
                event.accepted = true;
            }
            Keys.onRightPressed: function(event) {
                room._selectedKey =
                    VideoStage.moveSelection(room._feeds, room._selectedKey, 1);
                event.accepted = true;
            }
            // Back to auto. The fullscreen Shortcut above takes Escape
            // first while fullscreen (shortcuts are matched before key
            // events reach items), which is the order you want: leave
            // fullscreen, then let a second press drop the pick.
            Keys.onEscapePressed: function(event) {
                room._selectedKey = "";
                event.accepted = true;
            }

            // The feed that is on the stage still occupies its slot in
            // the strip, highlighted, so the strip reads as the whole
            // set rather than "the others". A chip, not a second copy of
            // the video: two VideoOutputs on one sink is twice the
            // compositing for a thumbnail nobody is looking at.
            Repeater {
                model: room._feeds
                delegate: Rectangle {
                    id: stageMarker
                    required property var modelData
                    required property int index

                    readonly property var slot: VideoStage.stripSlot(
                        index, room._feeds.length, feedArea.width,
                        feedArea.gap, feedArea.thumbMaxW)

                    visible: room._stripVisible
                             && VideoStage.stageIndexOf(
                                    room._feeds, room._selectedKey,
                                    modelData.key) >= 0
                    x: slot.x
                    y: feedArea.stageH + feedArea.gap
                    width: slot.width
                    height: feedArea.stripH
                    radius: Theme.r2
                    color: Theme.accentGlow
                    border.color: Theme.accent
                    border.width: 2

                    Column {
                        anchors.centerIn: parent
                        spacing: 2
                        width: parent.width - Theme.sp.s4

                        Text {
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            text: "ON STAGE"
                            font.family: Theme.fontSans
                            font.pixelSize: 9
                            font.weight: Theme.fontWeight.semibold
                            font.letterSpacing: Theme.trackWidest.xs
                            color: Theme.accent
                        }
                        Text {
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            text: VideoStage.feedLabel(
                                room._displayNameFor(stageMarker.modelData.userId),
                                stageMarker.modelData.kind,
                                stageMarker.modelData.isSelf === true)
                            font.family: Theme.fontSans
                            font.pixelSize: 9
                            color: Theme.fg2
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            // The feeds themselves.
            Repeater {
                model: room._feeds
                delegate: VideoFeedTile {
                    id: feedTile
                    required property var modelData
                    required property int index

                    // Where this feed belongs right now: a stage cell if
                    // it is on the stage, otherwise its strip slot.
                    readonly property int stageIndex: VideoStage.stageIndexOf(
                        room._feeds, room._selectedKey, modelData.key)
                    readonly property var cell: {
                        if (stageIndex >= 0)
                            return VideoStage.gridCell(
                                stageIndex, room._stageCount, feedArea.width,
                                feedArea.stageH, feedArea.gap);
                        var slot = VideoStage.stripSlot(
                            index, room._feeds.length, feedArea.width,
                            feedArea.gap, feedArea.thumbMaxW);
                        return { x: slot.x,
                                 y: feedArea.stageH + feedArea.gap,
                                 width: slot.width,
                                 height: feedArea.stripH };
                    }

                    feed: modelData
                    displayName: room._displayNameFor(modelData.userId)
                    liveTick: room._shareTick
                    featured: stageIndex >= 0
                    compact: stageIndex < 0
                    x: cell.x
                    y: cell.y
                    width: cell.width
                    height: cell.height

                    onClicked: {
                        feedArea.forceActiveFocus();
                        room._selectedKey = modelData.key;
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
