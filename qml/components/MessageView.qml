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
    id: messageViewRoot
    color: Theme.bg0

    // Called by Ctrl+L shortcut in main.qml. Walks to the composer's
    // TextArea and gives it focus.
    function focusComposer() {
        if (messageInput && messageInput.inputArea)
            messageInput.inputArea.forceActiveFocus();
    }

    // Thread-panel state exposed so the mobile shell's hardware-back
    // handler can close the panel before popping any other UI.
    function threadPanelOpen() {
        return threadPanel && threadPanel.rootEventId !== "";
    }
    function closeThread() {
        if (threadPanel) threadPanel.closePanel();
    }

    // Drag-and-drop file uploads. Anchored over the whole MessageView
    // so dropping anywhere in the chat pane — message list, composer,
    // empty state — uploads the files into the current channel. Only
    // accepts drops while the user has an active channel AND
    // ATTACH_FILES permission (canAttach is a live binding).
    DropArea {
        id: fileDrop
        anchors.fill: parent
        z: 100
        // A single drag-enter session can carry multiple files. We
        // accept the drag if *any* URL looks like a local file; each
        // URL is then re-checked on drop before uploading so we don't
        // send empty selections.
        keys: ["text/uri-list"]

        property bool canUploadHere: {
            var s = serverManager.activeServer;
            if (!s || !s.activeRoomId) return false;
            return s.canAttach(s.activeRoomId);
        }

        onEntered: (drag) => {
            if (!canUploadHere || !drag.hasUrls) {
                drag.accepted = false;
                return;
            }
            drag.accepted = true;
        }
        onDropped: (drop) => {
            if (!canUploadHere) { drop.accepted = false; return; }
            var uploaded = 0;
            for (var i = 0; i < drop.urls.length; ++i) {
                var u = drop.urls[i];
                if (!u) continue;
                // Accept only local file:// URLs for now; Matrix media
                // upload reads the bytes off disk. Remote URLs would
                // need a separate fetch-and-reupload path.
                var s = u.toString();
                if (s.indexOf("file://") === 0) {
                    serverManager.activeServer.sendMediaMessage(s);
                    uploaded++;
                }
            }
            if (uploaded > 0) messageInput.uploading = true;
            drop.accepted = uploaded > 0;
        }
    }

    // Drag overlay — fades in while a drag is hovering the DropArea.
    // Accent-tinted scrim + centered card so there's an unmissable
    // "drop here to upload" affordance regardless of what's underneath.
    Rectangle {
        anchors.fill: parent
        z: 99
        visible: opacity > 0.01
        opacity: fileDrop.containsDrag && fileDrop.canUploadHere ? 1.0 : 0.0
        Behavior on opacity {
            NumberAnimation { duration: Theme.motion.fastMs
                              easing.type: Easing.OutCubic }
        }
        color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.14)
        border.color: Theme.accent
        border.width: 2
        radius: Theme.r2

        ColumnLayout {
            anchors.centerIn: parent
            spacing: Theme.sp.s4

            Rectangle {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 72
                Layout.preferredHeight: 72
                radius: Theme.r3
                color: Theme.bg1
                border.color: Theme.accent
                border.width: 2
                Icon {
                    anchors.centerIn: parent
                    name: "paperclip"
                    size: 28
                    color: Theme.accent
                }
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: "Drop to upload"
                color: Theme.fg0
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.lg
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackTight.lg
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: "Files dropped here are sent to #"
                    + (serverManager.activeServer
                        ? serverManager.activeServer.activeRoomName : "")
                color: Theme.fg2
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
            }
        }
    }

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
        // topic, right-aligned action cluster, bottom divider. Hidden on
        // mobile because MobileMain's own top bar already renders the
        // channel + server name; stacking both would waste vertical space
        // a phone doesn't have.
        Rectangle {
            visible: !Theme.isMobile
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 48 : 0
            color: Theme.bg0

            // Icon-button primitive for the action cluster. 28×28 ghost
            // tile, bg2 hover, optional toggled state that paints the
            // glyph in the accent. Matches ChannelList.FooterButton.
            component HeaderButton: Rectangle {
                id: hbtn
                property string icon: ""
                property string tooltip: ""
                property bool toggled: false
                signal clicked()

                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
                Layout.alignment: Qt.AlignVCenter
                radius: Theme.r1
                color: toggled
                    ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.16)
                    : (hbtnMouse.containsMouse ? Theme.bg2 : "transparent")
                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                Icon {
                    anchors.centerIn: parent
                    name: hbtn.icon
                    size: 16
                    color: hbtn.toggled ? Theme.accent
                         : hbtnMouse.containsMouse ? Theme.fg0
                         : Theme.fg2
                }

                MouseArea {
                    id: hbtnMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: hbtn.clicked()
                }

                ToolTip.visible: hbtnMouse.containsMouse && hbtn.tooltip.length > 0
                ToolTip.text: hbtn.tooltip
                ToolTip.delay: 500
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.sp.s7
                anchors.rightMargin: Theme.sp.s5
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

                // Action cluster — thin vertical rule separates from title,
                // then 28px ghost buttons. Hidden when there's no active
                // channel so we don't advertise actions that can't fire.
                Rectangle {
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 18
                    Layout.alignment: Qt.AlignVCenter
                    color: Theme.line
                    visible: serverManager.activeServer !== null
                           && serverManager.activeServer.activeRoomId !== ""
                }

                HeaderButton {
                    id: pinnedBtn
                    icon: "pin"
                    tooltip: "Pinned messages"
                    visible: serverManager.activeServer !== null
                           && serverManager.activeServer.activeRoomId !== ""
                    onClicked: pinnedPopover.openBelow(pinnedBtn)
                }

                HeaderButton {
                    icon: "search"
                    tooltip: "Search messages  (⌃K)"
                    visible: serverManager.activeServer !== null
                           && serverManager.activeServer.activeRoomId !== ""
                    onClicked: Window.window.openSearch
                        ? Window.window.openSearch()
                        : null
                }

                HeaderButton {
                    icon: "users"
                    tooltip: Window.window.showMemberList
                        ? "Hide member list  (⌃M)"
                        : "Show member list  (⌃M)"
                    toggled: Window.window.showMemberList
                    visible: serverManager.activeServer !== null
                    onClicked: Window.window.showMemberList = !Window.window.showMemberList
                }
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

            // ── Unread-messages divider ───────────────────────────────
            // When the user enters a room, we snapshot the stored
            // "last read" timestamp into `unreadBoundaryMs`. The
            // delegate draws a red "New" divider above the first
            // message whose ts > boundary. The boundary is frozen for
            // the visit so the divider doesn't jump while messages
            // arrive. On leaving the room we write the newest loaded
            // message's ts back into settings, marking it read.
            property real unreadBoundaryMs: 0
            property string unreadDividerEventId: ""
            property string _currentRoomId: ""

            function _recomputeUnreadDivider() {
                var mm = model;
                if (!mm || unreadBoundaryMs <= 0) {
                    unreadDividerEventId = "";
                    return;
                }
                unreadDividerEventId = mm.firstEventIdAfterTs(unreadBoundaryMs) || "";
            }

            function _persistLastReadForCurrent() {
                if (!_currentRoomId) return;
                var mm = model;
                if (!mm) return;
                var ts = mm.newestTimestampMs();
                if (ts > 0) appSettings.setLastReadTs(_currentRoomId, ts);
            }

            // React to room changes: write the OUTGOING room's lastRead,
            // then load the INCOMING room's boundary for divider placement.
            Connections {
                target: serverManager.activeServer
                ignoreUnknownSignals: true
                function onActiveRoomIdChanged() {
                    // Persist newest-seen ts for the room we're leaving.
                    messageListView._persistLastReadForCurrent();
                    var s = serverManager.activeServer;
                    var next = s ? s.activeRoomId : "";
                    messageListView._currentRoomId = next;
                    messageListView.unreadBoundaryMs = next
                        ? appSettings.lastReadTs(next) : 0;
                    // Defer recompute until the model has repopulated
                    // for the new room. Trigger via onCountChanged below.
                    messageListView.unreadDividerEventId = "";
                }
            }

            // Initial-load: pick up whichever room is already active
            // when the view is constructed, so the divider works on
            // first app start (not just after a room switch).
            Component.onCompleted: {
                var s = serverManager.activeServer;
                var rid = s ? s.activeRoomId : "";
                _currentRoomId = rid;
                unreadBoundaryMs = rid ? appSettings.lastReadTs(rid) : 0;
                _recomputeUnreadDivider();
            }

            // Whether the user is pinned to the bottom. Updated ONLY by
            // user-initiated scroll completion (onMovementEnded) so
            // content growth doesn't transiently flip it. Content growth
            // uses the latched value to decide whether to auto-scroll.
            property bool atBottom: true
            property bool initialLoad: true
            // Tolerance for "near the bottom" — wider on mobile so a
            // touch flick's kinetic overshoot doesn't flap `atBottom`
            // false and flicker the jump-to-latest button.
            readonly property int bottomTolerance: Theme.isMobile ? 160 : 80

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
                if (_isAtEnd()) {
                    atBottom = true;
                } else {
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
            // Any user-driven movement cancels the 2-second initial-load
            // grace window. The `atBottom` state is derived from position
            // in onContentYChanged now (bidirectional), so we don't
            // preemptively flip it false here — a short wheel tick that
            // happens to land inside the end band shouldn't be punished
            // by having atBottom first set false and then back true.
            onMovementStarted: {
                initialLoad = false;
                scrollTimer.stop();
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
                _recomputeUnreadDivider();
            }

            // Jump to the end. We used to call the builtin
            // `positionViewAtEnd()`, but during layout flap (async image
            // loads, delegate recycling) it computed targets from stale
            // item heights and parked contentY well past the actual end
            // of content — which then latched atBottom=false because dist
            // went negative. Doing the arithmetic ourselves against the
            // live contentHeight/height is both simpler and stable.
            function _jumpToEnd() {
                if (contentHeight <= height) {
                    contentY = originY;
                } else {
                    contentY = originY + contentHeight - height;
                }
            }

            // Guarded callLater — re-checks the pin state at fire time so
            // a mid-frame user scroll can abort a pending auto-scroll.
            function _scrollToEndSoon() {
                Qt.callLater(function() {
                    if (initialLoad || atBottom) _jumpToEnd();
                });
            }

            Timer {
                id: scrollTimer
                interval: 2000
                onTriggered: {
                    if (messageListView.initialLoad || messageListView.atBottom) {
                        messageListView._jumpToEnd();
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

                // "New messages" divider — accent-coloured hairline with
                // a pill on the right, rendered above the first unread
                // message in the loaded timeline. Computed once per
                // channel-enter via `_recomputeUnreadDivider` and
                // frozen there for the visit, so it doesn't jump as
                // new messages arrive.
                Item {
                    width: parent.width
                    height: visible ? 18 : 0
                    visible: messageListView.unreadDividerEventId !== ""
                          && messageListView.unreadDividerEventId === model.eventId

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: 1
                        color: Theme.danger
                    }

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.sp.s2
                        width: newPill.implicitWidth + Theme.sp.s3 * 2
                        height: 14
                        radius: Theme.r1
                        color: Theme.danger

                        Text {
                            id: newPill
                            anchors.centerIn: parent
                            text: "NEW"
                            font.family: Theme.fontSans
                            font.pixelSize: 9
                            font.weight: Theme.fontWeight.semibold
                            font.letterSpacing: Theme.trackWidest.xs
                            color: "white"
                        }
                    }
                }

                // Date separator — thin `line` rule spanning the full
                // width, with a widest-tracked small-caps pill floating
                // on top. Matches the section-header vocabulary used in
                // ServerSettings / ChannelSettings.
                Item {
                    width: parent.width
                    height: visible ? 44 : 0
                    visible: model.showDateSeparator

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: 1
                        color: Theme.line
                    }

                    Rectangle {
                        anchors.centerIn: parent
                        width: dateSepText.implicitWidth + Theme.sp.s5 * 2
                        height: 22
                        color: Theme.bg1
                        radius: Theme.r4
                        border.color: Theme.line
                        border.width: 1

                        Text {
                            id: dateSepText
                            anchors.centerIn: parent
                            text: messageListView.formatDateSeparator(model.timestamp).toUpperCase()
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.xs
                            font.weight: Theme.fontWeight.semibold
                            font.letterSpacing: Theme.trackWidest.xs
                            color: Theme.fg3
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
                    threadRootId: model.threadRootId || ""
                    threadReplyCount: model.threadReplyCount || 0
                    onThreadOpenRequested: (rootId) => {
                        threadPanel.openFor(rootId);
                    }
                    onEditHistoryRequested: (eid) => {
                        editHistoryDialog.openFor(eid);
                    }

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
                    // Inline image left-click — route to the shared
                    // lightbox. Middle-click bypasses this entirely and
                    // goes straight to the browser (handled in the bubble).
                    onImageOpenRequested: (url, filename, size) => {
                        imageViewer.openFor(url, filename, size);
                    }
                    // Context-menu delete → confirm, then redact. The
                    // server enforces MANAGE_MESSAGES / sender-ownership
                    // so a spoofed client at worst gets a 403, but the
                    // confirmation step prevents honest right-click
                    // slips on your own messages.
                    onDeleteRequested: (targetId) => {
                        deleteConfirm.eventId = targetId;
                        deleteConfirm.preview =
                            (model.body || "").substring(0, 140);
                        deleteConfirm.senderName = model.senderDisplayName || "";
                        deleteConfirm.open();
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

            // Scroll-to-bottom button. Desktop: compact bg1/line pill
            // centred above the composer, hover-tints into accent.
            // Mobile: 44×44 accent fab in the bottom-right corner so
            // it sits in the natural reach zone of a one-handed grip.
            Rectangle {
                id: scrollToBottomBtn
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Theme.sp.s3
                anchors.right: Theme.isMobile ? parent.right : undefined
                anchors.rightMargin: Theme.isMobile ? Theme.sp.s5 : 0
                anchors.horizontalCenter: Theme.isMobile ? undefined : parent.horizontalCenter
                width: Theme.isMobile ? 44 : 40
                height: Theme.isMobile ? 44 : 32
                radius: Theme.isMobile ? 22 : 16
                color: Theme.isMobile
                    ? (scrollBottomMouse.containsMouse ? Theme.accentDim : Theme.accent)
                    : (scrollBottomMouse.containsMouse ? Theme.accent : Theme.bg1)
                border.color: Theme.isMobile
                    ? Theme.bg0
                    : (scrollBottomMouse.containsMouse ? Theme.accent : Theme.line)
                border.width: Theme.isMobile ? 2 : 1
                visible: !messageListView.atBottom && messageListView.count > 0

                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                Behavior on border.color { ColorAnimation { duration: Theme.motion.fastMs } }

                Icon {
                    anchors.centerIn: parent
                    name: "chevron-down"
                    size: Theme.isMobile ? 20 : 16
                    color: Theme.isMobile
                        ? Theme.onAccent
                        : (scrollBottomMouse.containsMouse ? Theme.onAccent : Theme.fg1)
                }

                MouseArea {
                    id: scrollBottomMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        messageListView._jumpToEnd();
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
            // Tighter left/right margins on mobile so the composer
            // gets the full viewport width minus a small gutter.
            Layout.leftMargin: Theme.isMobile ? Theme.sp.s3 : Theme.sp.s7
            Layout.rightMargin: Theme.isMobile ? Theme.sp.s3 : Theme.sp.s7
            Layout.topMargin: Theme.sp.s3
            // Extra bottom margin on mobile for the home-indicator /
            // gesture bar so the composer isn't hugging the edge.
            // When the software keyboard is up `adjustResize` has
            // already shrunk the window — in that state the bar
            // below us is the keyboard itself, not the gesture
            // strip, so the home-indicator inset would waste space.
            // Drop to a small gap so the send button doesn't sit
            // flush against the top row of keys.
            Layout.bottomMargin: Theme.isMobile
                ? (Qt.inputMethod.visible ? Theme.sp.s2 : Theme.sp.s7 + 16)
                : Theme.sp.s7
            Layout.minimumHeight: Theme.isMobile ? 56 : 48
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

    // Full-window image lightbox. Shared across every bubble in the
    // channel so reopening a new image replaces the current one instead
    // of stacking modals.
    ImageViewer {
        id: imageViewer
    }

    // Pinned-messages popover — anchored below the chat-header "pin"
    // button. Shows the current room's pinned events (computed from
    // ServerConnection's m.room.pinned_events cache). Clicking a row
    // scrolls the timeline to that event via the same code path as
    // reply-jump. Unpin button (admins/mods only) removes it live.
    Popup {
        id: pinnedPopover
        parent: Overlay.overlay
        width: 380
        height: Math.min(420, contentColumn.implicitHeight + 24)
        padding: 0
        modal: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        function openBelow(anchor) {
            var p = anchor.mapToItem(Overlay.overlay,
                                      anchor.width, anchor.height + 4);
            x = Math.max(8, p.x - width);
            y = p.y;
            open();
        }

        // Reactive list of pinned ids, rebuilt when the server signals a
        // pinned-events state change. `_gen` gives the binding a real
        // dependency the QML engine can observe.
        property int _gen: 0
        readonly property var pinnedIds: {
            _gen;
            var s = serverManager.activeServer;
            if (!s || !s.activeRoomId) return [];
            return s.pinnedEventIds(s.activeRoomId);
        }
        Connections {
            target: serverManager.activeServer
            ignoreUnknownSignals: true
            function onRoomPinnedEventsChanged(roomId) { pinnedPopover._gen++; }
        }

        background: Rectangle {
            color: Theme.bg1
            radius: Theme.r2
            border.color: Theme.line
            border.width: 1
        }

        contentItem: ColumnLayout {
            id: contentColumn
            spacing: 0

            // Header
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 40
                color: Theme.bg2
                radius: Theme.r2
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.sp.s4
                    anchors.verticalCenter: parent.verticalCenter
                    text: "PINNED MESSAGES"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xs
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackWidest.xs
                    color: Theme.fg3
                }
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width; height: 1; color: Theme.line
                }
            }

            // Body
            ListView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(320, count * 66 + 2)
                visible: pinnedPopover.pinnedIds.length > 0
                clip: true
                model: pinnedPopover.pinnedIds
                spacing: 0
                boundsBehavior: Flickable.StopAtBounds

                delegate: Rectangle {
                    required property string modelData
                    required property int index
                    width: ListView.view.width
                    height: 66
                    color: pinRowHover.containsMouse ? Theme.bg2 : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                    // Look up the pinned event's preview fields from the
                    // MessageModel. If it isn't loaded we still show a
                    // stub so admins know something is pinned.
                    readonly property var preview: {
                        var s = serverManager.activeServer;
                        if (!s || !s.messageModel) return null;
                        var p = s.messageModel.eventPreview(modelData);
                        return (p && p.sender !== undefined) ? p : null;
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.sp.s3
                        spacing: 2

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.sp.s2

                            Text {
                                text: preview ? preview.sender : "Not loaded yet"
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.sm
                                font.weight: Theme.fontWeight.semibold
                                color: Theme.fg0
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            Text {
                                visible: preview !== null
                                text: {
                                    if (!preview) return "";
                                    var d = new Date(preview.timestamp);
                                    return d.toLocaleString(Qt.locale(), "MMM d, h:mm ap");
                                }
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.xs
                                color: Theme.fg3
                            }
                            // Unpin — reveal on hover.
                            Icon {
                                name: "x"
                                size: 12
                                color: unpinMouse.containsMouse
                                    ? Theme.danger : Theme.fg3
                                opacity: pinRowHover.containsMouse ? 1.0 : 0.0
                                Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }
                                MouseArea {
                                    id: unpinMouse
                                    anchors.fill: parent
                                    anchors.margins: -6
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        var s = serverManager.activeServer;
                                        if (s) s.togglePinnedEvent(s.activeRoomId, modelData);
                                    }
                                }
                            }
                        }
                        Text {
                            text: preview && preview.body && preview.body.length > 0
                                ? preview.body
                                : "(scroll up — this pinned message hasn't loaded yet)"
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.sm
                            color: preview ? Theme.fg1 : Theme.fg3
                            font.italic: !preview
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            Layout.fillWidth: true
                        }
                    }

                    MouseArea {
                        id: pinRowHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            messageListView.jumpToLoadedEvent(modelData);
                            pinnedPopover.close();
                        }
                    }
                }
            }

            // Empty state
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 120
                visible: pinnedPopover.pinnedIds.length === 0
                ColumnLayout {
                    anchors.centerIn: parent
                    spacing: Theme.sp.s2
                    Icon {
                        name: "pin"
                        size: 20
                        color: Theme.fg3
                        Layout.alignment: Qt.AlignHCenter
                    }
                    Text {
                        text: "No pinned messages"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                        color: Theme.fg2
                        Layout.alignment: Qt.AlignHCenter
                    }
                    Text {
                        text: "Right-click a message → Pin"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                        color: Theme.fg3
                        Layout.alignment: Qt.AlignHCenter
                    }
                }
            }
        }
    }

    // Confirm popup for message deletion. Fields are populated by the
    // MessageBubble's onDeleteRequested so the modal shows a preview of
    // exactly what's about to be redacted — easy to bail on a misclick.
    Popup {
        id: deleteConfirm
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        width: 420
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: Theme.sp.s7

        property string eventId: ""
        property string preview: ""
        property string senderName: ""

        background: Rectangle {
            color: Theme.bg1
            radius: Theme.r3
            border.color: Theme.line
            border.width: 1
        }

        contentItem: ColumnLayout {
            spacing: Theme.sp.s4

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp.s3
                Rectangle {
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 32
                    radius: Theme.r2
                    color: Qt.rgba(Theme.danger.r, Theme.danger.g,
                                   Theme.danger.b, 0.15)
                    Icon {
                        anchors.centerIn: parent
                        name: "x"
                        size: 16
                        color: Theme.danger
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: "Delete message?"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xl
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackTight.xl
                    color: Theme.fg0
                }
            }

            // Preview — shows what's about to disappear. Capped to 140
            // chars at the emit site so the dialog doesn't balloon on a
            // long message.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: previewCol.implicitHeight + Theme.sp.s4 * 2
                color: Theme.bg2
                radius: Theme.r2
                border.color: Theme.line
                border.width: 1
                visible: deleteConfirm.preview.length > 0
                ColumnLayout {
                    id: previewCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.sp.s4
                    anchors.rightMargin: Theme.sp.s4
                    spacing: 2
                    Text {
                        visible: deleteConfirm.senderName.length > 0
                        text: deleteConfirm.senderName
                        color: Theme.accent
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                        font.weight: Theme.fontWeight.semibold
                    }
                    Text {
                        Layout.fillWidth: true
                        text: deleteConfirm.preview
                        color: Theme.fg1
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                        wrapMode: Text.Wrap
                        elide: Text.ElideRight
                        maximumLineCount: 3
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                text: "This can't be undone. The message will be removed for everyone in the channel."
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.sp.s3
                spacing: Theme.sp.s3
                Item { Layout.fillWidth: true }

                Button {
                    id: cancelDeleteBtn
                    contentItem: Text {
                        text: "Cancel"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.medium
                        color: Theme.fg1
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: cancelDeleteBtn.hovered ? Theme.bg3 : "transparent"
                        border.color: Theme.line
                        border.width: 1
                        radius: Theme.r2
                        implicitWidth: 100
                        implicitHeight: 36
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                    onClicked: deleteConfirm.close()
                }
                Button {
                    id: confirmDeleteBtn
                    contentItem: Text {
                        text: "Delete"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.onAccent
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: confirmDeleteBtn.hovered
                            ? Qt.lighter(Theme.danger, 1.1) : Theme.danger
                        radius: Theme.r2
                        implicitWidth: 120
                        implicitHeight: 36
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                    onClicked: {
                        if (!serverManager.activeServer
                            || deleteConfirm.eventId === "") {
                            deleteConfirm.close();
                            return;
                        }
                        serverManager.activeServer.redactEvent(
                            serverManager.activeServer.activeRoomId,
                            deleteConfirm.eventId);
                        deleteConfirm.close();
                    }
                }
            }
        }
    }

    // Edit history viewer — chronological list of all known
    // revisions of the message. In-memory only for now.
    Popup {
        id: editHistoryDialog
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        width: 480
        height: Math.min(parent ? parent.height * 0.7 : 480, 560)
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        property var entries: []
        function openFor(eid) {
            var s = serverManager.activeServer;
            if (!s || !s.messageModel) return;
            entries = s.messageModel.editHistory(eid);
            open();
        }
        background: Rectangle {
            color: Theme.bg1
            radius: Theme.r3
            border.color: Theme.line
            border.width: 1
        }
        contentItem: ColumnLayout {
            spacing: Theme.sp.s3
            Text {
                text: "EDIT HISTORY"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWidest.xs
                color: Theme.fg3
            }
            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: Theme.sp.s3
                ScrollBar.vertical: ThemedScrollBar {}
                model: editHistoryDialog.entries
                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    readonly property bool current: modelData.isCurrent === true
                    readonly property bool original: index === 0
                    height: col.implicitHeight + Theme.sp.s4 * 2
                    radius: Theme.r2
                    color: current ? Qt.rgba(Theme.accent.r, Theme.accent.g,
                                             Theme.accent.b, 0.08)
                                   : Theme.bg2
                    border.color: current ? Theme.accent : Theme.line
                    border.width: 1
                    ColumnLayout {
                        id: col
                        anchors.fill: parent
                        anchors.margins: Theme.sp.s4
                        spacing: 4
                        RowLayout {
                            Text {
                                text: {
                                    if (original) return "Original";
                                    if (current) return "Current";
                                    return "Revision " + index;
                                }
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.xs
                                font.weight: Theme.fontWeight.semibold
                                font.letterSpacing: Theme.trackWidest.xs
                                color: current ? Theme.accent : Theme.fg3
                                Layout.fillWidth: true
                            }
                            Text {
                                text: {
                                    var d = new Date(modelData.timestamp);
                                    return d.toLocaleString(Qt.locale(),
                                        "MMM d, h:mm:ss ap");
                                }
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.xs
                                color: Theme.fg3
                            }
                        }
                        Text {
                            text: modelData.body
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.base
                            color: Theme.fg0
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }
                    }
                }
            }
        }
    }

    // Threading side-drawer. Overlays the right portion of the chat
    // area; hidden until a thread is explicitly opened. Closes on
    // backdrop click or the X in its header.
    ThreadPanel {
        id: threadPanel
        anchors.fill: parent
        z: 50
    }
}
