import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// Message view (playing the role of SPEC §3.6 ChatPanel while we're
// still text-first). bg0 backdrop — matches the VoiceRoom and the
// window, so the main column reads as a single continuous surface
// across the view swap, with bg1 sidebars lifting either side. When the
// real SPEC §3.6 ChatPanel lands as a 320w right-rail variant, it'll
// switch to bg1 to read as the "panel" surface instead.
Rectangle {
    color: Theme.bg0

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Sync error banner — 28h strip above the channel header when the
        // connection isn't healthy. Reconnecting = amber warn; disconnected
        // = danger red. Small animated dot on the left signals live state.
        Rectangle {
            id: syncBanner
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 28 : 0
            color: {
                if (!serverManager.activeServer) return "transparent";
                if (serverManager.activeServer.connectionStatus === 2) return Theme.warn;
                if (serverManager.activeServer.connectionStatus === 0) return Theme.danger;
                return "transparent";
            }
            visible: serverManager.activeServer !== null && serverManager.activeServer.connectionStatus !== 1

            RowLayout {
                anchors.centerIn: parent
                spacing: Theme.sp.s3

                // Pulse dot — loops opacity so the banner reads as "live
                // state, not static warning."
                Rectangle {
                    Layout.alignment: Qt.AlignVCenter
                    width: 6; height: 6; radius: 3
                    color: Theme.onAccent
                    SequentialAnimation on opacity {
                        loops: Animation.Infinite
                        running: syncBanner.visible
                        NumberAnimation { to: 0.3; duration: 600; easing.type: Easing.InOutQuad }
                        NumberAnimation { to: 1.0; duration: 600; easing.type: Easing.InOutQuad }
                    }
                }

                Text {
                    Layout.alignment: Qt.AlignVCenter
                    text: {
                        if (!serverManager.activeServer) return "";
                        if (serverManager.activeServer.connectionStatus === 2)
                            return "Reconnecting to server…";
                        if (serverManager.activeServer.connectionStatus === 0)
                            return "Disconnected — messages won't send until the server is reachable";
                        return "";
                    }
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackTight.sm
                    // onAccent works here because warn/danger are both
                    // high-saturation colours that contrast with both the
                    // dark-mode near-black and the light-mode white.
                    color: Theme.onAccent
                }
            }

            Behavior on Layout.preferredHeight {
                NumberAnimation { duration: Theme.motion.normalMs
                                  easing.type: Easing.BezierSpline
                                  easing.bezierCurve: Theme.motion.bezier }
            }
        }

        // Header bar (SPEC §3.6 top, 40-48h) — hash + channel name, inline
        // topic, bottom divider.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            color: Theme.bg0

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.sp.s7
                anchors.rightMargin: Theme.sp.s7
                spacing: Theme.sp.s5

                Icon {
                    name: "hash"
                    size: 20
                    color: Theme.fg3
                    visible: serverManager.activeServer !== null && serverManager.activeServer.activeRoomId !== ""
                }

                Text {
                    text: serverManager.activeServer ? serverManager.activeServer.activeRoomName : ""
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.lg
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackTight.lg
                    color: Theme.fg0
                }

                // Topic separator
                Rectangle {
                    width: 1
                    height: 18
                    color: Theme.line
                    visible: topicText.text !== ""
                }

                Text {
                    id: topicText
                    text: serverManager.activeServer ? serverManager.activeServer.activeRoomTopic : ""
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    color: Theme.fg2
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    visible: text !== ""
                }

                Item { Layout.fillWidth: true; visible: topicText.text === "" }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.line
            }
        }

        // Messages list
        ListView {
            id: messageListView
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: Theme.sp.s7
            Layout.rightMargin: Theme.sp.s7
            clip: true
            verticalLayoutDirection: ListView.TopToBottom
            spacing: 2
            // Prevent rubber-band overshoot. Without this, async delegate
            // height changes (image loads, reaction chip layout) combined
            // with positionViewAtEnd() during the initial settle could
            // park `contentY` thousands of pixels past `contentHeight`,
            // from which the normal "am I at bottom?" heuristic couldn't
            // recover.
            boundsBehavior: Flickable.StopAtBounds

            ScrollBar.vertical: ThemedScrollBar {}

            model: serverManager.activeServer ? serverManager.activeServer.messageModel : null

            // Whether the user is pinned to the bottom. Updated ONLY by
            // user-initiated scroll completion (onMovementEnded) so
            // content growth doesn't transiently flip it. Content growth
            // uses the latched value to decide whether to auto-scroll.
            property bool atBottom: true
            property bool initialLoad: true
            readonly property int bottomTolerance: 80

            // Back-pagination bookkeeping. Record contentHeight AND contentY
            // just before we request older messages; after they prepend we
            // use both to figure out whether Qt's ListView auto-shifted
            // contentY (it does, for inserts at index 0) or we need to
            // nudge it ourselves. Double-adjusting caused the view to
            // jump back toward the bottom after pagination.
            property real paginationAnchorContentHeight: -1
            property real paginationAnchorContentY: 0
            readonly property int paginationTriggerPx: 200

            function _maybeLoadOlder() {
                if (!serverManager.activeServer) return;
                var mm = serverManager.activeServer.messageModel;
                if (!mm || !mm.hasMoreHistory || mm.loadingHistory) return;
                if (count === 0) return;
                paginationAnchorContentHeight = contentHeight;
                paginationAnchorContentY = contentY;
                serverManager.activeServer.loadOlderMessages(50);
            }

            // Fires whenever contentY moves, from any source: user drag,
            // wheel, kinetic flick, ScrollBar drag, positionViewAtEnd().
            // This is the one chokepoint that catches every way of
            // scrolling, including ScrollBar drags (which bypass
            // movementStarted because they poke contentY programmatically).
            //
            // Two jobs:
            //   1. If the new contentY is not near the bottom, drop the
            //      "pin to end" state so subsequent content growth doesn't
            //      snap us back. This also ends the 2-second initial-load
            //      grace window.
            //   2. Near the top, kick off back-pagination.
            // Helper: is `contentY` currently within the tight band at
            // the end of the list? Unlike a one-sided check, this rejects
            // contentY values PAST the valid end (dist < 0) as "not at
            // bottom" — those come from transient layout churn, not from
            // the user intentionally being at the end.
            function _isAtEnd() {
                if (contentHeight <= height) return true;
                var dist = contentHeight - contentY - height;
                return dist >= -bottomTolerance && dist <= bottomTolerance;
            }

            onContentYChanged: {
                if (contentHeight <= height) return;
                if (!_isAtEnd()) {
                    if (initialLoad) {
                        initialLoad = false;
                        scrollTimer.stop();
                    }
                    atBottom = false;
                }
                if (!initialLoad && contentY < paginationTriggerPx) {
                    _maybeLoadOlder();
                }
            }

            // Event id to pulse-highlight after a reply/link jump. Set by
            // onJumpToEvent / scrollToEventRequested, cleared by
            // highlightTimer. Each delegate binds its overlay visibility to
            // (highlightedEventId === model.eventId).
            property string highlightedEventId: ""
            Timer {
                id: highlightTimer
                interval: 1500
                onTriggered: messageListView.highlightedEventId = ""
            }

            function jumpToLoadedEvent(eventId) {
                if (!serverManager.activeServer) return false;
                var mm = serverManager.activeServer.messageModel;
                if (!mm) return false;
                var idx = mm.indexForEventId(eventId);
                if (idx < 0) {
                    // Not in the loaded timeline — try back-paginating up
                    // to a cap and re-check on each response. The cap
                    // prevents an unbounded hammer if the target event
                    // doesn't exist or was redacted.
                    if (mm.hasMoreHistory && _jumpAttemptsLeft > 0) {
                        _jumpPendingEventId = eventId;
                        _jumpAttemptsLeft -= 1;
                        paginationAnchorContentHeight = contentHeight;
                        serverManager.activeServer.loadOlderMessages(100);
                        return false;
                    }
                    console.warn("MessageView: target event not in loaded history:",
                                 eventId);
                    return false;
                }
                // Disable auto-scroll-to-bottom so the jump sticks even if
                // content height updates arrive right after.
                messageListView.atBottom = false;
                messageListView.positionViewAtIndex(idx, ListView.Center);
                messageListView.highlightedEventId = eventId;
                highlightTimer.restart();
                _jumpPendingEventId = "";
                _jumpAttemptsLeft = 10;
                return true;
            }

            // Paginate-until-found state for jumpToLoadedEvent. When a
            // reply-target isn't loaded yet, we kick off back-pagination
            // and re-try after each batch lands, up to _jumpAttemptsLeft
            // pages. Fresh jumps reset the counter.
            property string _jumpPendingEventId: ""
            property int _jumpAttemptsLeft: 10

            onModelChanged: {
                initialLoad = true;
                atBottom = true;
            }

            // Any user-driven scroll (drag, flick, wheel) cancels the
            // initial-load grace period and its pending scroll-to-end
            // timer. Without this, scrolling up in the first ~2s after a
            // channel loaded got yanked back to the bottom when the timer
            // fired — or when an inline image loaded and
            // onContentHeightChanged triggered a forced end-scroll while
            // initialLoad was still true.
            onMovementStarted: {
                initialLoad = false;
                scrollTimer.stop();
                atBottom = false;
            }

            onMovementEnded: {
                atBottom = _isAtEnd();
            }

            // When content grows (new message, image loaded), auto-scroll
            // if we were at the bottom before the growth. Exception:
            // if a back-pagination just landed, shift contentY by the
            // growth delta so the user's viewport stays on the same
            // message instead of being shoved downward by the prepended
            // batch.
            onContentHeightChanged: {
                if (paginationAnchorContentHeight >= 0
                    && contentHeight > paginationAnchorContentHeight) {
                    var delta = contentHeight - paginationAnchorContentHeight;
                    // Has Qt already shifted contentY to compensate for the
                    // prepend? It does this for inserts at index 0 when
                    // they're outside the visible area. If the current cY
                    // is already at/near (anchorCY + delta), we'd be
                    // double-adjusting by adding delta again — which
                    // pushed the view back toward the bottom. Only nudge
                    // contentY if Qt hasn't.
                    var expectedAuto = paginationAnchorContentY + delta;
                    var alreadyShifted = Math.abs(contentY - expectedAuto) < 2;
                    if (!alreadyShifted) {
                        contentY = paginationAnchorContentY + delta;
                    }
                    paginationAnchorContentHeight = -1;
                    return;
                }
                if (initialLoad || atBottom) _scrollToEndSoon();
            }

            onCountChanged: {
                if (initialLoad || atBottom) {
                    _scrollToEndSoon();
                    if (initialLoad) scrollTimer.restart();
                }
            }

            // Guarded callLater — re-checks the pin state at fire time so
            // a mid-frame user scroll can abort a pending auto-scroll.
            function _scrollToEndSoon() {
                Qt.callLater(function() {
                    if (initialLoad || atBottom) positionViewAtEnd();
                });
            }

            Timer {
                id: scrollTimer
                interval: 2000
                onTriggered: {
                    if (messageListView.initialLoad || messageListView.atBottom) {
                        messageListView.positionViewAtEnd();
                    }
                    messageListView.initialLoad = false;
                }
            }

            // Date separator helper
            function formatDateSeparator(timestamp) {
                var date = new Date(timestamp);
                var today = new Date();
                var yesterday = new Date();
                yesterday.setDate(yesterday.getDate() - 1);
                if (date.toDateString() === today.toDateString()) return "Today";
                if (date.toDateString() === yesterday.toDateString()) return "Yesterday";
                return date.toLocaleDateString(Qt.locale(), "MMMM d, yyyy");
            }

            delegate: Column {
                width: messageListView.width
                spacing: 0

                // Date separator
                Item {
                    width: parent.width
                    height: visible ? 40 : 0
                    visible: model.showDateSeparator

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: 1
                        color: Theme.bg3
                    }

                    Rectangle {
                        anchors.centerIn: parent
                        width: dateSepText.implicitWidth + 24
                        height: 22
                        color: Theme.bg2
                        radius: Theme.r4

                        Text {
                            id: dateSepText
                            anchors.centerIn: parent
                            text: messageListView.formatDateSeparator(model.timestamp)
                            font.pixelSize: Theme.fontSize.sm
                            font.bold: true
                            color: Theme.fg2
                        }
                    }
                }

                MessageBubble {
                    width: parent.width
                    eventId: model.eventId
                    highlighted: messageListView.highlightedEventId === model.eventId
                    sender: model.sender
                    senderDisplayName: model.senderDisplayName
                    body: model.body
                    formattedBody: model.formattedBody
                    timestamp: model.timestamp
                    msgtype: model.msgtype
                    mediaUrl: model.mediaUrl || ""
                    mediaFileName: model.mediaFileName || ""
                    mediaFileSize: model.mediaFileSize || 0
                    isOwnMessage: model.isOwnMessage
                    showSender: model.showSender
                    edited: model.edited || false
                    replyToEventId: model.replyToEventId || ""
                    replyToSender: model.replyToSender || ""
                    replyPreview: model.replyPreview || ""
                    reactions: model.reactions || []

                    onSenderClicked: (userId, displayName) => {
                        messageProfileCard.userId = userId;
                        messageProfileCard.profileDisplayName = displayName;
                        messageProfileCard.open();
                    }
                    onEditRequested: (targetId, currentBody) => {
                        messageInput.beginEditing(targetId, currentBody);
                    }
                    onReplyRequested: (targetId, body, senderName) => {
                        var preview = body.length > 80 ? body.substring(0, 80) + "\u2026" : body;
                        messageInput.beginReplying(targetId, senderName, preview);
                    }
                    onForwardRequested: (targetId, body, senderName) => {
                        forwardDialog.openFor(targetId, body, senderName);
                    }
                    onJumpToEvent: (targetId) => {
                        messageListView._jumpAttemptsLeft = 10;
                        messageListView.jumpToLoadedEvent(targetId);
                    }
                    // #channel tag clicked inside a message body — switch
                    // the active server to that channel if one matches.
                    onChannelLinkClicked: (name) => {
                        if (serverManager.activeServer)
                            serverManager.activeServer.activateRoomByName(name);
                    }
                    // bsfchat://message/... clicked — ServerManager resolves
                    // the server URL, switches, opens the room, scrolls.
                    onMessageLinkClicked: (link) => {
                        serverManager.openMessageLink(link);
                    }
                    // @mention anchor clicked — show the profile card for
                    // the target user id.
                    onUserLinkClicked: (userId, displayName) => {
                        messageProfileCard.userId = userId;
                        messageProfileCard.profileDisplayName = displayName;
                        messageProfileCard.open();
                    }
                    // Copy-link button — ask the active connection for the
                    // canonical URL and stuff it on the clipboard.
                    onCopyLinkRequested: (targetId) => {
                        if (!serverManager.activeServer) return;
                        var link = serverManager.activeServer.messageLink(targetId);
                        if (link) serverManager.copyToClipboard(link);
                    }
                    // Reaction chip clicked or emoji picker selection —
                    // toggle the current user's reaction for that emoji.
                    onReactionToggled: (targetId, emoji) => {
                        if (!serverManager.activeServer) return;
                        serverManager.activeServer.toggleReaction(targetId, emoji);
                    }
                }
            }

            // Back-pagination loading indicator at the top of the list.
            // Visible while a /messages request is in flight; stays tiny
            // so it doesn't crowd the conversation.
            Rectangle {
                id: paginationSpinner
                anchors.top: parent.top
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.topMargin: 6
                width: 20; height: 20; radius: 10
                color: Theme.bg3
                opacity: 0.9
                visible: serverManager.activeServer
                         && serverManager.activeServer.messageModel
                         && serverManager.activeServer.messageModel.loadingHistory
                Text {
                    anchors.centerIn: parent
                    text: "\u21BB"
                    font.pixelSize: 12
                    color: Theme.fg0
                    RotationAnimation on rotation {
                        from: 0; to: 360; duration: 900
                        loops: Animation.Infinite
                        running: paginationSpinner.visible
                    }
                }
            }

            // Scroll-to-bottom button — pill on bg1 with line border, hover
            // tints into accent so the action reads as "live." SVG chevron
            // replaces the bare unicode down-arrow.
            Rectangle {
                id: scrollToBottomBtn
                anchors.bottom: parent.bottom
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottomMargin: Theme.sp.s3
                width: 40; height: 32
                radius: 16
                color: scrollBottomMouse.containsMouse ? Theme.accent : Theme.bg1
                border.color: scrollBottomMouse.containsMouse ? Theme.accent : Theme.line
                border.width: 1
                visible: !messageListView.atBottom && messageListView.count > 0

                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                Behavior on border.color { ColorAnimation { duration: Theme.motion.fastMs } }

                Icon {
                    anchors.centerIn: parent
                    name: "chevron-down"
                    size: 16
                    color: scrollBottomMouse.containsMouse ? Theme.onAccent : Theme.fg1
                }

                MouseArea {
                    id: scrollBottomMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        messageListView.positionViewAtEnd();
                        messageListView.atBottom = true;
                    }
                }
            }

            // Empty state — iconic hash mark above a quiet prompt line.
            // Separates "no server" (meta problem), "no channel selected"
            // (user action needed), and "no history yet" (channel is new).
            ColumnLayout {
                anchors.centerIn: parent
                visible: !serverManager.activeServer
                         || serverManager.activeServer.activeRoomId === ""
                         || messageListView.count === 0
                spacing: Theme.sp.s5

                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    width: 64; height: 64
                    radius: 32
                    color: Theme.bg3
                    Icon {
                        anchors.centerIn: parent
                        name: !serverManager.activeServer
                              ? "at"
                              : (serverManager.activeServer.activeRoomId === ""
                                 ? "hash" : "send")
                        size: 28
                        color: Theme.fg3
                    }
                }

                Text {
                    Layout.alignment: Qt.AlignHCenter
                    horizontalAlignment: Text.AlignHCenter
                    text: {
                        if (!serverManager.activeServer) return "No server selected";
                        if (serverManager.activeServer.activeRoomId === "")
                            return "Pick a channel";
                        return "It's quiet in here";
                    }
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xl
                    font.weight: Theme.fontWeight.semibold
                    color: Theme.fg1
                }

                Text {
                    Layout.alignment: Qt.AlignHCenter
                    horizontalAlignment: Text.AlignHCenter
                    text: {
                        if (!serverManager.activeServer)
                            return "Sign in to a BSFChat server to start chatting.";
                        if (serverManager.activeServer.activeRoomId === "")
                            return "Choose one from the sidebar to join the conversation.";
                        return "Be the first to say something.";
                    }
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    color: Theme.fg3
                    wrapMode: Text.WordWrap
                    Layout.maximumWidth: 320
                }
            }
        }

        // Typing indicator — animated three-dot bounce + status text.
        // Fades in/out with a height transition so the composer doesn't
        // jump when someone starts typing.
        Item {
            id: typingBar
            Layout.fillWidth: true
            Layout.leftMargin: Theme.sp.s7
            Layout.preferredHeight: visible ? 20 : 0
            visible: serverManager.activeServer
                     && serverManager.activeServer.typingDisplay !== ""

            RowLayout {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.sp.s2

                // Three-dot bounce — staggered opacity animation keeps the
                // indicator unambiguous even at low text size.
                Row {
                    spacing: 3
                    Repeater {
                        model: 3
                        delegate: Rectangle {
                            required property int index
                            width: 4; height: 4; radius: 2
                            color: Theme.fg2
                            opacity: 0.3
                            SequentialAnimation on opacity {
                                loops: Animation.Infinite
                                running: typingBar.visible
                                PauseAnimation { duration: index * 140 }
                                NumberAnimation { to: 1.0; duration: 280 }
                                NumberAnimation { to: 0.3; duration: 280 }
                                PauseAnimation { duration: (2 - index) * 140 }
                            }
                        }
                    }
                }

                Text {
                    text: serverManager.activeServer
                          ? serverManager.activeServer.typingDisplay : ""
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    font.italic: true
                    color: Theme.fg2
                }
            }

            Behavior on Layout.preferredHeight {
                NumberAnimation { duration: Theme.motion.fastMs
                                  easing.type: Easing.BezierSpline
                                  easing.bezierCurve: Theme.motion.bezier }
            }
        }

        // Message input
        MessageInput {
            id: messageInput
            Layout.fillWidth: true
            Layout.leftMargin: Theme.sp.s7
            Layout.rightMargin: Theme.sp.s7
            Layout.topMargin: Theme.sp.s3
            Layout.bottomMargin: Theme.sp.s7
            Layout.minimumHeight: 48
            visible: serverManager.activeServer !== null && serverManager.activeServer.activeRoomId !== ""
            roomName: serverManager.activeServer ? serverManager.activeServer.activeRoomName : ""
        }
    }

    // Cross-room/server jump. ServerManager emits scrollToEventRequested on
    // the target connection after switching the room; we listen on whatever
    // the currently-active server is (target is automatically reassigned
    // when activeServer changes).
    Connections {
        target: serverManager.activeServer
        ignoreUnknownSignals: true
        function onScrollToEventRequested(eventId) {
            // Reset the attempt counter for a fresh jump origin.
            messageListView._jumpAttemptsLeft = 10;
            messageListView.jumpToLoadedEvent(eventId);
        }
        // After a back-paginate lands, if there's a pending jump, try
        // again — the target event may now be in the loaded timeline.
        function onOlderMessagesLoaded() {
            if (messageListView._jumpPendingEventId !== "") {
                messageListView.jumpToLoadedEvent(
                    messageListView._jumpPendingEventId);
            }
        }
    }

    // Profile card for clicking on sender names
    UserProfileCard {
        id: messageProfileCard
        parent: Overlay.overlay
    }

    // Modal channel picker for "Forward message". MVP scope is the
    // currently-active server's channels only.
    ForwardDialog {
        id: forwardDialog
        parent: Overlay.overlay
    }
}
