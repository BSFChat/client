import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import QtQuick.Window
import QtMultimedia
import BSFChat

import "../js/VideoStage.js" as VideoStage
import "../js/VideoWindows.js" as VideoWindows

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

    // The feed the user EXPANDED by clicking it. "" is the default and
    // means the grid — every feed on screen at once. Nothing is ever
    // written here on the app's own initiative: a screen share starting
    // used to take the stage by itself and sweep every face into the
    // strip, and that is exactly what this layout exists to undo.
    // Escape returns to "", so does clicking the expanded feed again,
    // and so does that feed ending (_refreshFeeds below).
    property string _selectedKey: ""

    // The tile the pointer is over, "" for none. Only `F` and the header
    // full-screen button read it, and only in the grid, where "this
    // video" can only mean the one being looked at. It is deliberately
    // NOT selection: hovering changes nothing about the layout.
    property string _hoveredKey: ""

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
    // The strip only earns its space when something has been expanded
    // and the other feeds have nowhere else to be. In the grid every
    // feed is already on the stage, and a strip under it would be a
    // second, smaller copy of the same pictures.
    readonly property bool _stripVisible: _stageMode === "expanded"

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
        // A feed that ends while it is expanded takes the stage back to
        // the grid. It does NOT hand the stage to some other feed: the
        // user expanded that one, and the next-best guess is a view
        // they never asked for.
        if (room._selectedKey && !VideoStage.hasKey(next, room._selectedKey))
            room._selectedKey = "";
        if (room._hoveredKey && !VideoStage.hasKey(next, room._hoveredKey))
            room._hoveredKey = "";
        // And a feed that has gone takes its windows with it: the share
        // stopped, the peer left, or we left the channel. pruneToFeeds
        // returns the SAME array when nothing is stale, which matters —
        // this runs four times a second while anyone is sharing, and a
        // fresh array would destroy and rebuild every pop-out window
        // that often.
        room._popouts = VideoWindows.pruneToFeeds(room._popouts, next);
        if (room._fullscreenKey
            && !VideoStage.hasKey(next, room._fullscreenKey))
            room._fullscreenKey = "";
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

    // ── Fullscreen and pop-out ────────────────────────────────────
    //
    // Both of these put a feed in a window of its own, and NEITHER of
    // them touches the main window. The header button used to write
    // `Window.window.visibility = Window.FullScreen`, which made the
    // app — sidebar, channel list, member strip, dock and all — cover
    // the screen with the video still in its panel in the middle. What
    // the owner asked for is the VIDEO filling the screen, so the
    // fullscreen button now opens VideoFullscreenWindow on the feed
    // that is on the stage and leaves this window exactly as it was.
    // tests/test_qml_hygiene.cpp fails if a write to another window's
    // visibility comes back to any of the voice-room video files.
    //
    // The feed key currently shown fullscreen BY THIS ROOM, or "". A
    // feed that is popped out gets its fullscreen from the pop-out
    // window instead (VideoWindows.fullscreenOwner), so the app can
    // never stack two fullscreen windows of one feed on one screen.
    property string _fullscreenKey: ""
    // Open pop-outs. See qml/js/VideoWindows.js — an array of records,
    // oldest first, whose identity changes only when a window actually
    // opens or closes.
    property var _popouts: VideoWindows.emptyState()

    readonly property bool fullscreen: _fullscreenKey !== ""

    // "This video" for a keyboard or header action: the expanded feed
    // when one is expanded, else whatever the pointer is over, else the
    // first feed — see VideoStage.focusKeyFor().
    function _stageFeedKey() {
        return VideoStage.focusKeyFor(room._feeds, room._selectedKey,
                                      room._hoveredKey);
    }

    function feedForKey(key) {
        var i = VideoStage.indexOfKey(room._feeds, key);
        return i >= 0 ? room._feeds[i] : null;
    }

    function isPoppedOut(key) {
        return VideoWindows.isOpen(room._popouts, key);
    }

    // Fullscreen for one feed, routed to whichever window owns it.
    function toggleFullscreenFor(key) {
        if (!key || !VideoStage.hasKey(room._feeds, key)) return;
        if (VideoWindows.fullscreenOwner(room._popouts, key) === "popout") {
            var w = popoutHost.windowFor(key);
            if (w) {
                w.raise();
                w.requestActivate();
                w.toggleFullscreen();
            }
            return;
        }
        room._fullscreenKey = (room._fullscreenKey === key) ? "" : key;
    }

    function toggleFullscreen() {
        room.toggleFullscreenFor(room.fullscreen ? room._fullscreenKey
                                                 : room._stageFeedKey());
    }

    // Pop a feed out into its own window, or raise the one already up.
    function popOutFeed(key) {
        var feed = room.feedForKey(key);
        if (!feed) return;
        var r = VideoWindows.requestPopout(room._popouts, feed);
        if (r.action === "open") {
            room._popouts = r.state;
            // A feed cannot be fullscreen from here AND popped out:
            // ownership just moved to the new window.
            if (room._fullscreenKey === key) room._fullscreenKey = "";
        } else if (r.action === "raise") {
            var w = popoutHost.windowFor(r.key);
            if (w) { w.raise(); w.requestActivate(); }
        }
    }

    function closePopout(key) {
        room._popouts = VideoWindows.closePopout(room._popouts, key);
    }

    // The room's own fullscreen window. A Loader, so the window exists
    // only while something is fullscreen and is destroyed — not merely
    // hidden — on the way out. `active` also falls to false on its own
    // when the feed disappears from _feeds, which is how "the stream
    // ended, the peer left, we left the channel" closes it.
    Loader {
        id: roomFullscreen
        active: room._fullscreenKey !== ""
                && VideoStage.hasKey(room._feeds, room._fullscreenKey)
        sourceComponent: VideoFullscreenWindow {
            // Both of these have to survive the feed vanishing between
            // one binding evaluation and the next — `active` above and
            // these are not evaluated in any guaranteed order, and
            // `feedForKey(...).userId` on a feed that has just ended is
            // a TypeError that takes the whole binding with it.
            feed: room.feedForKey(room._fullscreenKey)
            displayName: {
                var f = room.feedForKey(room._fullscreenKey);
                return f ? room._displayNameFor(f.userId) : "";
            }
            liveTick: room._shareTick
            onExitRequested: room._fullscreenKey = ""
        }
    }

    // Pop-out windows, one per record. An Instantiator rather than a
    // Repeater because these are Windows, not Items — there is no
    // visual parent for them to sit in.
    Instantiator {
        id: popoutHost
        model: room._popouts
        delegate: VideoPopoutWindow {
            required property var modelData
            required property int index
            feed: modelData
            displayName: room._displayNameFor(modelData.userId)
            liveTick: room._shareTick
            cascadeIndex: index
            onCloseRequested: room.closePopout(modelData.key)
        }

        // Raising an existing window needs the object behind a key.
        function windowFor(key) {
            for (var i = 0; i < count; ++i) {
                var o = objectAt(i);
                if (o && o.feed && o.feed.key === key) return o;
            }
            return null;
        }
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
                    ? "Exit full screen  (Esc)"
                    : "Full screen this video  (F)"
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
                id: cryptoBadge
                visible: cryptoText.text.length > 0
                implicitWidth: cryptoText.implicitWidth + Theme.sp.s4
                implicitHeight: 22
                // Tapped-open state for touch, where there is no hover.
                // See the MouseArea below.
                property bool detailPinned: false
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
                //
                // On touch there is no hover, so this badge said
                // "E2EE" — or didn't — and the sentence explaining
                // what that does and does not cover was unreachable.
                // For a product whose entire pitch is that you host it
                // yourself and nobody else is listening, the caveats on
                // that claim are not an optional hover detail. Tap
                // opens the same text, tap again or wait closes it.
                MouseArea {
                    id: cryptoHover
                    anchors.fill: parent
                    // The badge is 22 pt tall — half the touch minimum.
                    // Grown vertically only: horizontally it is already
                    // as wide as its text and it has neighbours.
                    anchors.topMargin: Theme.isMobile ? -11 : 0
                    anchors.bottomMargin: Theme.isMobile ? -11 : 0
                    hoverEnabled: true
                    onClicked: {
                        if (!Theme.isMobile) return;
                        cryptoBadge.detailPinned = !cryptoBadge.detailPinned;
                        if (cryptoBadge.detailPinned) cryptoPinTimer.restart();
                        else cryptoPinTimer.stop();
                    }
                }
                // Times out rather than waiting for a second tap the user
                // may not think to make; a panel pinned open over the
                // participant list is its own small bug.
                Timer {
                    id: cryptoPinTimer
                    interval: 8000
                    onTriggered: cryptoBadge.detailPinned = false
                }
                ToolTip {
                    visible: (cryptoHover.containsMouse
                              || cryptoBadge.detailPinned) && text.length > 0
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
            //
            // The arrows walk WHICH FEED IS EXPANDED. In the grid they
            // do nothing at all — every feed is already visible, so
            // there is nothing to walk between, and an arrow key that
            // suddenly expanded something would be a view change nobody
            // asked for. moveSelection() enforces that; these handlers
            // stay symmetric with it.
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
            // Back to the grid. The fullscreen window is a window of
            // its own and takes its own Escape while it has focus, so
            // this one only ever means "collapse".
            Keys.onEscapePressed: function(event) {
                room._selectedKey = "";
                event.accepted = true;
            }
            // F full-screens the expanded feed, or in the grid the one
            // under the pointer. A Keys handler and not a Shortcut, for
            // the same reason as the arrows above: a window-wide "F"
            // would fire in every text field in the app. Bare F only —
            // Cmd-F is search.
            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_F
                    && event.modifiers === Qt.NoModifier) {
                    room.toggleFullscreen();
                    event.accepted = true;
                }
            }

            // The expanded feed still occupies its slot in the strip,
            // highlighted, so the strip reads as the whole set rather
            // than "the others". A chip, not a second copy of the
            // video: two VideoOutputs on one sink is twice the
            // compositing for a thumbnail nobody is looking at.
            //
            // It is clickable, and clicking it collapses back to the
            // grid — the strip is where the eye goes to change what is
            // expanded, so the one slot that means "show me everything
            // again" has to live there too.
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

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            feedArea.forceActiveFocus();
                            room._selectedKey = VideoStage.toggleExpanded(
                                room._feeds, room._selectedKey,
                                stageMarker.modelData.key);
                        }
                        ToolTip.visible: containsMouse
                        ToolTip.text: "Back to the grid  (Esc)"
                        ToolTip.delay: 400
                        hoverEnabled: true
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
                    // it is on the stage, otherwise its strip slot. In
                    // the grid every feed is on the stage and this is
                    // its cell in the grid; when one feed is expanded
                    // that feed's cell IS the whole stage (a 1-tile
                    // grid) and everybody else takes a strip slot.
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
                    // The accent highlight means "this is the one you
                    // expanded". In the grid nothing is expanded, and
                    // ringing every tile in accent would say nothing at
                    // all — so `featured` is deliberately not just
                    // "on the stage" any more.
                    featured: room._stageMode === "expanded"
                              && stageIndex >= 0
                    compact: stageIndex < 0
                    // The tile stays LIVE while its feed is popped out —
                    // that is the whole reason the registry fans one
                    // stream out to several sinks — so all this does is
                    // say so.
                    poppedOut: room.isPoppedOut(modelData.key)
                    x: cell.x
                    y: cell.y
                    width: cell.width
                    height: cell.height

                    // One click, both directions: a grid tile expands,
                    // the expanded tile collapses, a strip thumbnail
                    // takes over the stage.
                    onClicked: {
                        feedArea.forceActiveFocus();
                        room._selectedKey = VideoStage.toggleExpanded(
                            room._feeds, room._selectedKey, modelData.key);
                    }
                    onHoveredChanged: {
                        if (hovered)
                            room._hoveredKey = modelData.key;
                        else if (room._hoveredKey === modelData.key)
                            room._hoveredKey = "";
                    }
                    onFullscreenRequested:
                        room.toggleFullscreenFor(modelData.key)
                    onPopOutRequested: room.popOutFeed(modelData.key)
                    onClosePopOutRequested: room.closePopout(modelData.key)
                }
            }
        }
    }

    // ── Classic participant grid ──────────────────────────────────
    // Shown when there is no video at all. Auto-columns based on
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
