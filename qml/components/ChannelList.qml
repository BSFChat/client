import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import BSFChat

Rectangle {
    id: channelListRoot
    color: Theme.bg1
    implicitWidth: Theme.layout.channelSidebarW

    // Category collapse state
    property var collapsedCategories: ({})

    // During a cross-category drag this is set to the categoryId the
    // pointer is currently over (or "__none__" when outside any
    // category / over the source). Category delegates watch this to
    // show an accent outline — gives the user a clear "this is where
    // I'm about to drop" affordance. Sentinel rather than "" because
    // "" is the uncategorized bucket's real ID.
    property string dropHoverCategoryId: "__none__"

    // Nudges the channel-row bindings when mute or read-state
    // changes. QSettings reads are not reactive, so we drive a
    // counter that row bindings reference. Bumped on mute toggle,
    // on room-switch (from MessageView via the same appSettings
    // path), and periodically via the refresh timer.
    property int muteGeneration: 0
    property int unreadGeneration: 0

    // Poll for external lastReadTs updates (e.g. MessageView
    // persisting on room-switch). Cheap — only drives bindings.
    Timer {
        interval: 800
        running: true
        repeat: true
        onTriggered: channelListRoot.unreadGeneration++
    }

    function isCategoryCollapsed(catId) {
        return collapsedCategories[catId] === true;
    }

    function toggleCategoryCollapsed(catId) {
        // Copy into a fresh object so QML notices the property change.
        // Mutating-and-reassigning the same reference is a no-op as far as
        // `property var` change signals are concerned, which is why the
        // previous version silently did nothing.
        var c = {};
        for (var k in collapsedCategories) c[k] = collapsedCategories[k];
        c[catId] = !(collapsedCategories[catId] === true);
        collapsedCategories = c;
    }

    // Kind of thing the create-prompt is about to create. "category" |
    // "text" | "voice". Category ID scopes new channels; ignored for
    // "category" since we don't support nesting.
    function openCreatePrompt(kind, categoryId) {
        createPrompt.kind = kind;
        createPrompt.categoryId = categoryId || "";
        createPrompt.nameField.text = "";
        createPrompt.open();
        createPrompt.nameField.forceActiveFocus();
    }

    // Popup menu used by right-click on empty sidebar and by every "+" button.
    function openCreateMenu(x, y, categoryId) {
        createMenu.categoryId = categoryId || "";
        createMenu.popup(x, y);
    }

    // Drag-reorder commit. Given a sibling array and a (fromIdx,
    // toIdx), rewrites every channel's sortOrder so the final
    // ordering matches `siblings` with `fromIdx` spliced into
    // `toIdx`. Stride of 10 leaves room for future single-step
    // inserts without touching every sibling. Issuing all the
    // setChannelOrder writes back-to-back is fine — the server
    // timestamps them individually and the sync echo rebuilds the
    // sidebar from the canonical positions.
    function moveChannelTo(siblings, fromIdx, toIdx) {
        if (!serverManager.activeServer || !siblings) return;
        if (fromIdx === toIdx) return;
        if (fromIdx < 0 || fromIdx >= siblings.length) return;
        if (toIdx   < 0 || toIdx   >= siblings.length) return;
        var arr = siblings.slice();
        var moved = arr.splice(fromIdx, 1)[0];
        arr.splice(toIdx, 0, moved);
        for (var i = 0; i < arr.length; i++) {
            var want = i * 10;
            if (arr[i].sortOrder !== want) {
                serverManager.activeServer.setChannelOrder(arr[i].roomId, want);
            }
        }
    }

    // Look up which category a scene-Y coordinate falls inside. Walks
    // the outer Repeater's instantiated delegates; each exposes
    // `categoryId` + `categoryChannels` as explicit properties so we
    // don't have to peek into the model from here.
    //
    // Returns null if the Y isn't over any category (e.g. below the
    // last row's bottom), so the caller can keep the drag on its
    // source category.
    function dropTargetAt(sceneY) {
        var container = channelColumn;
        if (!container) return null;
        for (var i = 0; i < container.children.length; i++) {
            var cat = container.children[i];
            if (!cat || cat.categoryId === undefined) continue;
            var topLeft = cat.mapToItem(null, 0, 0);
            if (sceneY >= topLeft.y && sceneY <= topLeft.y + cat.height) {
                return {
                    categoryId: cat.categoryId,
                    categoryChannels: cat.categoryChannels,
                    top: topLeft.y,
                    height: cat.height
                };
            }
        }
        return null;
    }

    // Cross-category drop. Moves `roomId` into `destCategoryId`, then
    // writes a sortOrder that puts it at `destSlot` among the
    // destination's existing channels. We don't rewrite all sort
    // orders — inserting at `destSlot` gets a value between the
    // neighbours, leaving siblings unchanged. Matches how Discord
    // feels: one source channel moved, nothing else disturbed.
    function moveChannelAcross(roomId, destCategoryId, destChannels, destSlot) {
        if (!serverManager.activeServer) return;
        var arr = destChannels || [];
        // Clamp slot to [0, arr.length].
        var slot = Math.max(0, Math.min(arr.length, destSlot));
        var before = slot > 0 ? arr[slot - 1].sortOrder : null;
        var after  = slot < arr.length ? arr[slot].sortOrder : null;
        var newOrder;
        if (before === null && after === null) newOrder = 0;
        else if (before === null)              newOrder = after - 10;
        else if (after  === null)              newOrder = before + 10;
        else                                   newOrder = Math.floor((before + after) / 2);
        serverManager.activeServer.moveChannelToCategory(roomId, destCategoryId);
        serverManager.activeServer.setChannelOrder(roomId, newOrder);
    }

    // Root-level right-click handler: anywhere in the sidebar that isn't a
    // channel/category row (those consume their own right-clicks) opens the
    // create menu. acceptedButtons intentionally excludes LeftButton so
    // left-clicks continue to reach the sidebar's children normally.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        onClicked: (mouse) => {
            channelListRoot.openCreateMenu(mouse.x, mouse.y, "");
        }
    }

    // Unified Direct Messages view — overlays the regular channel
    // list while `serverManager.viewingDms` is true. Aggregates
    // every 1:1 room across every connected server into one flat
    // list. Selecting a row routes to the hosting server + room
    // without clearing DM mode, so the rail keeps the @ chip
    // highlighted.
    Item {
        id: dmView
        anchors.fill: parent
        visible: serverManager.viewingDms
        // Rebuilt whenever a connection reports a new DM / message.
        property int _gen: 0
        readonly property var _rooms: {
            dmView._gen;  // dep
            return serverManager.allDirectRooms();
        }

        Connections {
            target: serverManager
            ignoreUnknownSignals: true
            function onActiveServerChanged() { dmView._gen++; }
            function onViewingDmsChanged() {
                if (serverManager.viewingDms) dmView._gen++;
            }
        }
        // Per-connection signal wiring — every connection emits
        // directRoomsChanged / roomListChanged independently, and
        // we want to rebuild when any of them fire.
        Instantiator {
            model: serverManager.servers
            active: true
            delegate: QtObject {
                property var conn: {
                    var i = index;
                    return serverManager.connectionAt(i);
                }
                property var directConn: conn
                    ? conn.directRoomsChanged.connect(function() { dmView._gen++; })
                    : null
                property var listConn: conn
                    ? conn.roomListChanged.connect(function() { dmView._gen++; })
                    : null
                // Typing state tick — bump the generation so
                // delegates re-evaluate peerTyping and swap the
                // timestamp for the typing-dots indicator.
                property var typingConn: conn
                    ? conn.roomTypingChanged.connect(function() { dmView._gen++; })
                    : null
                // Presence changes arrive as per-user events; we
                // don't get a notification per DM peer, so just
                // re-pull on any sync update. Cheap — capped by
                // N DMs.
                property var presenceConn: conn
                    ? conn.messageReceived.connect(function() { dmView._gen++; })
                    : null
            }
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // Header. Matches the normal server-name header's height
            // + type scale so switching modes doesn't reflow the row
            // below on desktop.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 48
                color: Theme.bg1
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.sp.s7
                    anchors.rightMargin: Theme.sp.s5
                    spacing: Theme.sp.s3
                    Icon { name: "at"; size: 16; color: Theme.fg0 }
                    Text {
                        text: "Direct Messages"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.fg0
                        Layout.fillWidth: true
                    }
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width
                    height: 1; color: Theme.line }
            }

            // "New DM" inline composer — accepts @user:host and
            // routes through the currently-active server's
            // createDirectMessage. As the user types, a suggestions
            // list appears below showing matches from every
            // connected server's known-members table so they can
            // pick without remembering full MXIDs.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.sp.s3
                Layout.rightMargin: Theme.sp.s3
                Layout.topMargin: Theme.sp.s3
                spacing: Theme.sp.s2

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 44
                    radius: Theme.r2
                    color: Theme.bg0
                    border.color: newDmField.activeFocus ? Theme.accent : Theme.line
                    border.width: 1
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.sp.s3
                        anchors.rightMargin: Theme.sp.s1
                        spacing: Theme.sp.s3
                        Icon { name: "plus"; size: 13; color: Theme.fg2 }
                        TextField {
                            id: newDmField
                            Layout.fillWidth: true
                            placeholderText: "New DM — search name or @user:server"
                            background: Item {}
                            color: Theme.fg0
                            placeholderTextColor: Theme.fg3
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.sm
                            selectByMouse: true
                            Keys.onReturnPressed: dmView._submitNewDm()
                            onTextChanged: dmView._refreshSuggestions()
                        }
                        Rectangle {
                            Layout.preferredWidth: 32
                            Layout.preferredHeight: 32
                            radius: Theme.r1
                            color: submitMouse.containsMouse
                                   && newDmField.text.trim().length > 0
                                   ? Theme.accentDim : Theme.accent
                            opacity: newDmField.text.trim().length > 0 ? 1.0 : 0.5
                            Icon { anchors.centerIn: parent; name: "send"
                                   size: 12; color: Theme.onAccent }
                            MouseArea {
                                id: submitMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                enabled: newDmField.text.trim().length > 0
                                cursorShape: enabled ? Qt.PointingHandCursor
                                                     : Qt.ArrowCursor
                                onClicked: dmView._submitNewDm()
                            }
                        }
                    }
                }

                // Live match list — only visible while the input
                // has content AND we have results. Click a row to
                // start / open the DM with that user on the
                // server they're known from.
                Rectangle {
                    visible: dmView._suggestions.length > 0
                             && newDmField.text.trim().length > 0
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(
                        dmView._suggestions.length * 44 + 4, 220)
                    radius: Theme.r2
                    color: Theme.bg0
                    border.color: Theme.line
                    border.width: 1

                    ListView {
                        id: suggestionList
                        anchors.fill: parent
                        anchors.margins: 2
                        clip: true
                        model: dmView._suggestions
                        boundsBehavior: Flickable.StopAtBounds

                        delegate: Rectangle {
                            width: ListView.view ? ListView.view.width : 0
                            height: 40
                            radius: Theme.r1
                            color: sugHover.containsMouse
                                   ? Theme.bg2 : "transparent"
                            Behavior on color {
                                ColorAnimation { duration: Theme.motion.fastMs }
                            }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.sp.s3
                                anchors.rightMargin: Theme.sp.s3
                                spacing: Theme.sp.s3

                                Rectangle {
                                    Layout.preferredWidth: 22
                                    Layout.preferredHeight: 22
                                    radius: Theme.r1
                                    color: Theme.senderColor(modelData.userId)
                                    Text {
                                        anchors.centerIn: parent
                                        text: {
                                            var n = modelData.displayName
                                                  || modelData.userId;
                                            var s = n.replace(/^[^a-zA-Z0-9]+/, "");
                                            return (s.length > 0 ? s.charAt(0) : "?")
                                                .toUpperCase();
                                        }
                                        font.family: Theme.fontSans
                                        font.pixelSize: 11
                                        font.weight: Theme.fontWeight.semibold
                                        color: Theme.onAccent
                                    }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Text {
                                        text: modelData.displayName
                                              || modelData.userId
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.sm
                                        color: Theme.fg0
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
                                    Text {
                                        text: modelData.userId
                                              + (modelData.serverName
                                                  ? " · " + modelData.serverName
                                                  : "")
                                        font.family: Theme.fontMono
                                        font.pixelSize: 10
                                        color: Theme.fg3
                                        elide: Text.ElideMiddle
                                        Layout.fillWidth: true
                                    }
                                }
                            }
                            MouseArea {
                                id: sugHover
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: dmView._pickSuggestion(modelData)
                            }
                        }
                    }
                }
            }

            ListView {
                id: dmListView
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.topMargin: Theme.sp.s2
                clip: true
                spacing: 0
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ThemedScrollBar {}
                model: dmView._rooms

                Text {
                    anchors.centerIn: parent
                    visible: dmListView.count === 0
                    text: "No direct messages yet.\n"
                        + "Start one with @user:server above."
                    horizontalAlignment: Text.AlignHCenter
                    color: Theme.fg3
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                }

                delegate: Rectangle {
                    id: dmRow
                    width: ListView.view ? ListView.view.width : 0
                    height: Theme.isMobile ? 60 : 52
                    readonly property bool isActive: {
                        var s = serverManager.activeServer;
                        return s && s.serverUrl === modelData.serverUrl
                            && s.activeRoomId === modelData.roomId;
                    }
                    color: isActive ? Theme.bg3
                         : rowMouse.containsMouse ? Theme.bg2 : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.sp.s4
                        anchors.rightMargin: Theme.sp.s4
                        spacing: Theme.sp.s3

                        Item {
                            Layout.preferredWidth: 32
                            Layout.preferredHeight: 32
                            Rectangle {
                                anchors.fill: parent
                                radius: Theme.r2
                                color: Theme.senderColor(modelData.peerId)
                                Text {
                                    anchors.centerIn: parent
                                    text: {
                                        var n = modelData.peerDisplayName
                                              || modelData.peerId;
                                        var s = n.replace(/^[^a-zA-Z0-9]+/, "");
                                        return (s.length > 0 ? s.charAt(0) : "?")
                                            .toUpperCase();
                                    }
                                    font.family: Theme.fontSans
                                    font.pixelSize: 13
                                    font.weight: Theme.fontWeight.semibold
                                    color: Theme.onAccent
                                }
                            }
                            // Presence dot overlaid on the bottom-
                            // right of the avatar chip. Same
                            // vocabulary as MemberList's presence
                            // halo. Hidden when the server hasn't
                            // told us anything about this user
                            // (empty string) so we don't render a
                            // misleading grey dot.
                            Rectangle {
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.rightMargin: -1
                                anchors.bottomMargin: -1
                                width: 10; height: 10
                                radius: 5
                                border.color: Theme.bg1
                                border.width: 2
                                visible: modelData.peerPresence !== undefined
                                         && modelData.peerPresence.length > 0
                                color: {
                                    switch (modelData.peerPresence) {
                                    case "online":      return Theme.online;
                                    case "unavailable": return Theme.warning;
                                    default:            return Theme.fg3;
                                    }
                                }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Text {
                                text: modelData.peerDisplayName
                                      || modelData.peerId
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.base
                                font.weight: modelData.unreadCount > 0
                                    ? Theme.fontWeight.semibold
                                    : Theme.fontWeight.medium
                                color: Theme.fg0
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            // Sub-label: prefer the peer's custom
                            // status message if they've set one,
                            // else show the hosting server (which
                            // is useful when the user is in DMs
                            // across multiple servers).
                            Text {
                                text: {
                                    if (modelData.peerStatusMessage
                                        && modelData.peerStatusMessage.length > 0) {
                                        return modelData.peerStatusMessage;
                                    }
                                    return modelData.serverName
                                        || modelData.serverUrl;
                                }
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.xs
                                font.italic: modelData.peerStatusMessage
                                    && modelData.peerStatusMessage.length > 0
                                color: Theme.fg3
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }

                        // Right-edge trailing: typing indicator
                        // wins over the unread pill — if the peer
                        // is live-typing we want that signal front
                        // and centre. Three pulsing dots, same
                        // vocabulary as MessageInput's upload
                        // activity indicator.
                        Row {
                            visible: modelData.peerTyping === true
                            spacing: 3
                            Repeater {
                                model: 3
                                delegate: Rectangle {
                                    required property int index
                                    width: 5; height: 5; radius: 2.5
                                    color: Theme.accent
                                    opacity: 0.35
                                    SequentialAnimation on opacity {
                                        loops: Animation.Infinite
                                        running: parent.parent.visible
                                        PauseAnimation { duration: index * 140 }
                                        NumberAnimation { to: 1.0; duration: 280 }
                                        NumberAnimation { to: 0.35; duration: 280 }
                                        PauseAnimation { duration: (2 - index) * 140 }
                                    }
                                }
                            }
                        }
                        // Unread pill, suppressed while typing.
                        Rectangle {
                            visible: modelData.unreadCount > 0
                                     && modelData.peerTyping !== true
                            implicitWidth: Math.max(20, unreadText.implicitWidth + 10)
                            implicitHeight: 18
                            radius: 9
                            color: Theme.danger
                            Text {
                                id: unreadText
                                anchors.centerIn: parent
                                text: modelData.unreadCount > 99
                                    ? "99+" : modelData.unreadCount
                                color: Theme.onAccent
                                font.family: Theme.fontSans
                                font.pixelSize: 10
                                font.weight: Theme.fontWeight.semibold
                            }
                        }
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            // Switch to the owning server without
                            // clearing DM mode, then select the DM
                            // room. The @ chip stays highlighted.
                            serverManager.setActiveServer(
                                modelData.serverIndex);
                            if (serverManager.activeServer) {
                                serverManager.activeServer
                                    .setActiveRoom(modelData.roomId);
                            }
                        }
                    }
                }
            }
        }

        property var _suggestions: []

        function _refreshSuggestions() {
            var q = newDmField.text.trim();
            if (q.length === 0) { _suggestions = []; return; }
            _suggestions = serverManager.searchKnownUsers(q, 8);
        }

        function _pickSuggestion(m) {
            // Route createDirectMessage through the correct
            // connection — otherwise a user known only on server B
            // but typed while server A is active would end up with
            // a DM against a different homeserver that doesn't
            // host the account.
            serverManager.setActiveServer(m.serverIndex);
            var s = serverManager.activeServer;
            if (!s) return;
            if (m.userId === s.userId) return;
            // Look for an existing DM so we jump instead of
            // duplicating. matches directRooms() shape.
            var existing = "";
            var dms = s.directRooms();
            for (var i = 0; i < dms.length; i++) {
                if (dms[i].peerId === m.userId) {
                    existing = dms[i].roomId;
                    break;
                }
            }
            if (existing.length > 0) {
                s.setActiveRoom(existing);
            } else {
                s.createDirectMessage(m.userId);
            }
            newDmField.text = "";
            _suggestions = [];
        }

        function _submitNewDm() {
            // If there's exactly one suggestion and the user hits
            // Enter without picking, take the first suggestion —
            // that's the typical "I typed what I wanted, just go"
            // flow. Otherwise fall back to raw MXID parsing.
            if (_suggestions.length > 0) {
                _pickSuggestion(_suggestions[0]);
                return;
            }
            var target = newDmField.text.trim();
            if (target.length === 0) return;
            if (!target.startsWith("@") && target.indexOf(":") > 0) {
                target = "@" + target;
            }
            var s = serverManager.activeServer;
            if (!s) {
                if (serverManager.servers.rowCount() > 0) {
                    serverManager.setActiveServer(0);
                    s = serverManager.activeServer;
                }
            }
            if (!s) return;
            if (target === s.userId) { newDmField.text = ""; return; }
            s.createDirectMessage(target);
            newDmField.text = "";
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        visible: !serverManager.viewingDms

        // Server name header (SPEC §3.2 top, 48h)
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            color: Theme.bg1

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.sp.s7
                anchors.rightMargin: Theme.sp.s5
                spacing: Theme.sp.s3

                Text {
                    Layout.fillWidth: true
                    verticalAlignment: Text.AlignVCenter
                    text: serverManager.activeServer ? serverManager.activeServer.serverName : "BSFChat"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.lg
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackTight.lg
                    color: Theme.fg0
                    elide: Text.ElideRight
                }

                // Settings gear icon — visible whenever the user has any
                // admin-level permission (ADMINISTRATOR short-circuits to all
                // flags, so admins always see it).
                Icon {
                    name: "settings"
                    size: 18
                    color: settingsGearMouse.containsMouse ? Theme.fg0 : Theme.fg2
                    visible: {
                        if (!serverManager.activeServer) return false;
                        var sc = serverManager.activeServer;
                        // permissionsGeneration is a real int dependency the
                        // AOT compiler won't eliminate; bumped by every
                        // apply*Event handler in ServerConnection.
                        if (sc.permissionsGeneration < 0) return false;
                        var rid = sc.activeRoomId || "";
                        return sc.canManageRoles(rid) || sc.canManageChannel(rid) || sc.canKick(rid) || sc.canBan(rid);
                    }
                    Layout.alignment: Qt.AlignVCenter

                    MouseArea {
                        id: settingsGearMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: serverSettings.open()
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

        // Direct Messages section — rendered above the channel list.
        // Uses ServerConnection.directRooms() which returns one entry
        // per 1:1 room the current user has ever created/persisted on
        // this server. Empty ⇒ whole section collapses to zero height.
        Item {
            id: dmSection
            Layout.fillWidth: true
            Layout.preferredHeight: dmSectionColumn.implicitHeight
            visible: serverManager.activeServer !== null
                  && dmRepeater.count > 0

            readonly property int _dmGen: {
                if (!serverManager.activeServer) return 0;
                // Tick on every DM mutation so the Repeater rebuilds.
                serverManager.activeServer.directRoomsChanged;
                return 0;
            }

            ColumnLayout {
                id: dmSectionColumn
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 0

                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 40
                    Layout.leftMargin: Theme.sp.s7
                    Layout.rightMargin: Theme.sp.s3

                    Text {
                        text: "DIRECT MESSAGES"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                        font.weight: Theme.fontWeight.semibold
                        font.letterSpacing: Theme.trackWidest.xs
                        color: Theme.fg3
                        Layout.fillWidth: true
                        verticalAlignment: Text.AlignVCenter
                    }
                    Text {
                        id: dmPlus
                        text: "+"
                        font.pixelSize: Theme.fontSize.xl
                        color: dmPlusMouse.containsMouse ? Theme.fg0 : Theme.fg2
                        Layout.alignment: Qt.AlignVCenter
                        MouseArea {
                            id: dmPlusMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: newDmPrompt.open()
                        }
                    }
                }

                Repeater {
                    id: dmRepeater
                    model: serverManager.activeServer
                        ? serverManager.activeServer.directRooms() : []

                    delegate: Item {
                        width: channelListRoot.width
                        height: 36

                        readonly property bool _active: serverManager.activeServer
                            && modelData.roomId === serverManager.activeServer.activeRoomId

                        Rectangle {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.sp.s2
                            anchors.rightMargin: Theme.sp.s2
                            radius: Theme.r1
                            color: _active ? Theme.bg3
                                 : (dmRowMouse.containsMouse ? Theme.bg2 : "transparent")
                            Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.sp.s3
                                anchors.rightMargin: Theme.sp.s3
                                spacing: Theme.sp.s3

                                Item {
                                    Layout.preferredWidth: 22
                                    Layout.preferredHeight: 22
                                    Rectangle {
                                        anchors.fill: parent
                                        radius: Theme.r2
                                        color: Theme.senderColor(modelData.peerId)
                                        Text {
                                            anchors.centerIn: parent
                                            text: {
                                                var n = (modelData.peerDisplayName || modelData.peerId || "?");
                                                var stripped = n.replace(/^[^a-zA-Z0-9]+/, "");
                                                return (stripped.charAt(0) || "?").toUpperCase();
                                            }
                                            font.family: Theme.fontSans
                                            font.pixelSize: 11
                                            font.weight: Theme.fontWeight.semibold
                                            color: Theme.onAccent
                                        }
                                    }
                                    // Tiny presence dot bottom-right.
                                    Rectangle {
                                        anchors.right: parent.right
                                        anchors.bottom: parent.bottom
                                        anchors.rightMargin: -1
                                        anchors.bottomMargin: -1
                                        width: 7; height: 7; radius: 3.5
                                        readonly property string _state: {
                                            var s = serverManager.activeServer;
                                            return s ? s.presenceFor(modelData.peerId) : "offline";
                                        }
                                        color: _state === "online" ? Theme.online : Theme.bg1
                                        border.width: 1.5
                                        border.color: _state === "offline" ? Theme.fg3 : Theme.bg1
                                    }
                                }

                                Text {
                                    text: modelData.peerDisplayName || modelData.peerId
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.fontSize.base
                                    color: _active ? Theme.fg0 : Theme.fg1
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }

                            MouseArea {
                                id: dmRowMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if (serverManager.activeServer)
                                        serverManager.activeServer.setActiveRoom(modelData.roomId);
                                }
                            }
                        }
                    }
                }

                // Thin divider separating DMs from regular channels.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.sp.s7
                    Layout.rightMargin: Theme.sp.s5
                    Layout.topMargin: Theme.sp.s3
                    Layout.preferredHeight: 1
                    color: Theme.lineSoft
                }
            }
        }

        // Channel category header with "+" button
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            visible: serverManager.activeServer !== null

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.sp.s7
                anchors.rightMargin: Theme.sp.s3

                Text {
                    text: "CHANNELS"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xs
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackWidest.xs
                    color: Theme.fg3
                    Layout.fillWidth: true
                }

                // Create room/category button — opens the same context menu
                // as right-click on empty sidebar, for discoverability.
                Text {
                    id: topPlus
                    text: "+"
                    font.pixelSize: Theme.fontSize.xl
                    color: topPlusMouse.containsMouse ? Theme.fg0 : Theme.fg2
                    Layout.alignment: Qt.AlignVCenter

                    MouseArea {
                        id: topPlusMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            var p = topPlus.mapToItem(channelListRoot, 0, topPlus.height);
                            channelListRoot.openCreateMenu(p.x, p.y, "");
                        }
                    }
                }
            }
        }

        // Category-structured channel list
        Flickable {
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentHeight: channelColumn.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            ScrollBar.vertical: ThemedScrollBar {}

            Column {
                id: channelColumn
                width: parent.width

                Repeater {
                    model: serverManager.activeServer ? serverManager.activeServer.categorizedRooms : []

                    delegate: Item {
                        id: categoryDelegate
                        width: channelColumn.width
                        // Size to the inner content Column; Column's
                        // implicitHeight tracks the stacked kids so we
                        // re-flow when channels collapse/expand.
                        implicitHeight: categoryContent.implicitHeight
                        height: implicitHeight

                        // Exposed to channelListRoot's drop-target
                        // scanner so it can attribute a scene-Y to
                        // the right category without hit-testing the
                        // private delegate hierarchy.
                        readonly property string categoryId: modelData.categoryId || ""
                        readonly property var    categoryChannels: modelData.channels || []

                        // Drop-target highlight: accent-tinted backdrop
                        // + dashed-feel border when this category is the
                        // active drop target. Sits behind the content
                        // (z: -1) so channels remain fully interactive.
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 2
                            z: -1
                            radius: Theme.r2
                            visible: channelListRoot.dropHoverCategoryId === categoryDelegate.categoryId
                            color: Qt.rgba(Theme.accent.r, Theme.accent.g,
                                           Theme.accent.b, 0.12)
                            border.color: Theme.accent
                            border.width: 1
                            Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }
                        }

                        Column {
                            id: categoryContent
                            width: parent.width

                        // Category header (skip for uncategorized).
                        Item {
                            id: catHeader
                            width: parent.width
                            height: modelData.categoryId !== "" ? 32 : 0
                            visible: modelData.categoryId !== ""

                            // Backdrop click handler — left-click toggles
                            // collapse/expand; right-click opens the
                            // create menu scoped to this category. Child
                            // MouseAreas (the "+" add button) steal their
                            // own clicks because they're declared later
                            // and land on top.
                            MouseArea {
                                id: catHeaderMouse
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                cursorShape: Qt.PointingHandCursor
                                hoverEnabled: true
                                onClicked: (mouse) => {
                                    if (mouse.button === Qt.RightButton) {
                                        var p = mapToItem(channelListRoot, mouse.x, mouse.y);
                                        channelListRoot.openCreateMenu(
                                            p.x, p.y, modelData.categoryId);
                                    } else {
                                        toggleCategoryCollapsed(modelData.categoryId);
                                    }
                                }
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.sp.s2
                                anchors.rightMargin: Theme.sp.s2

                                // Collapse chevron — rotates rather than
                                // swapping glyphs so the transition is
                                // continuous. Angle 0 = pointing right
                                // (collapsed); 90 = pointing down (open).
                                Icon {
                                    name: "chevron-right"
                                    size: 12
                                    color: catHeaderMouse.containsMouse
                                        ? Theme.fg0 : Theme.fg3
                                    rotation: isCategoryCollapsed(modelData.categoryId) ? 0 : 90
                                    Behavior on rotation {
                                        NumberAnimation { duration: Theme.motion.fastMs
                                                          easing.type: Easing.BezierSpline
                                                          easing.bezierCurve: Theme.motion.bezier }
                                    }
                                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                }

                                Text {
                                    text: modelData.categoryName ? modelData.categoryName.toUpperCase() : ""
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.fontSize.xs
                                    font.weight: Theme.fontWeight.semibold
                                    font.letterSpacing: Theme.trackWidest.xs
                                    color: catHeaderMouse.containsMouse
                                        ? Theme.fg1 : Theme.fg3
                                    Layout.fillWidth: true
                                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                }

                                // "+" to add channel in this category.
                                // Reveal-on-hover so the resting header
                                // stays quiet; covers the chevron+label
                                // vocabulary used in ServerSettings.
                                Rectangle {
                                    id: catPlus
                                    Layout.preferredWidth: 22
                                    Layout.preferredHeight: 22
                                    Layout.alignment: Qt.AlignVCenter
                                    radius: Theme.r1
                                    color: catPlusMouse.containsMouse
                                        ? Theme.bg3 : "transparent"
                                    opacity: catHeaderMouse.containsMouse
                                          || catPlusMouse.containsMouse ? 1.0 : 0.0
                                    Behavior on color   { ColorAnimation { duration: Theme.motion.fastMs } }
                                    Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }

                                    Icon {
                                        anchors.centerIn: parent
                                        name: "plus"
                                        size: 12
                                        color: catPlusMouse.containsMouse
                                            ? Theme.accent : Theme.fg2
                                    }

                                    MouseArea {
                                        id: catPlusMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            var p = catPlus.mapToItem(channelListRoot, 0, catPlus.height);
                                            channelListRoot.openCreateMenu(p.x, p.y, modelData.categoryId);
                                        }
                                    }

                                    ToolTip.visible: catPlusMouse.containsMouse
                                    ToolTip.text: "Add channel"
                                    ToolTip.delay: 500
                                }
                            }
                        }

                        // Channels in this category (hidden when collapsed)
                        Column {
                            width: parent.width
                            visible: !isCategoryCollapsed(modelData.categoryId)

                            Repeater {
                                id: channelRepeater
                                model: modelData.channels

                                delegate: Item {
                                    id: channelDelegate
                                    width: parent.width
                                    height: channelItemContent.implicitHeight + 4

                                    readonly property int channelIndex: index
                                    // Live mute + unread flags. Gated by
                                    // muteGeneration / unreadGeneration so
                                    // QSettings changes flow into bindings.
                                    readonly property bool isMuted: {
                                        channelListRoot.muteGeneration;
                                        return appSettings.isRoomMuted(modelData.roomId);
                                    }
                                    readonly property bool hasUnread: {
                                        channelListRoot.unreadGeneration;
                                        channelListRoot.muteGeneration;
                                        if (isMuted) return false;
                                        if (modelData.isVoice) return false;
                                        var lastMsg = modelData.lastMessageTime || 0;
                                        if (lastMsg <= 0) return false;
                                        var lastRead = appSettings.lastReadTs(modelData.roomId);
                                        // lastRead == 0 means we've never recorded a
                                        // read marker for this room yet. Treat that as
                                        // "caught up" rather than "all messages new"
                                        // — otherwise every channel in the tree lights
                                        // up on first launch.
                                        if (lastRead <= 0) return false;
                                        return lastMsg > lastRead;
                                    }
                                    // Drag state — while `dragging` is
                                    // true, the row lifts via bg + z
                                    // and translates by `dragY` to
                                    // follow the cursor. On release
                                    // we snap `dragY` to 0 and write
                                    // new sortOrders via the backend;
                                    // the sync echo then rebuilds the
                                    // final channel order.
                                    property bool dragging: false
                                    property real dragY: 0

                                    readonly property bool isActiveText:
                                        !modelData.isVoice && serverManager.activeServer
                                        && modelData.roomId === serverManager.activeServer.activeRoomId
                                    readonly property bool isActiveVoice:
                                        modelData.isVoice && serverManager.activeServer
                                        && modelData.roomId === serverManager.activeServer.activeVoiceRoomId
                                    readonly property bool isActive: isActiveText || isActiveVoice

                                    Rectangle {
                                        id: channelItemBg
                                        anchors.fill: parent
                                        anchors.leftMargin: Theme.sp.s2
                                        anchors.rightMargin: Theme.sp.s2
                                        // Lift the row above its siblings while dragging
                                        // so it floats over the rest of the list. Anchor
                                        // offsets move the background with the drag.
                                        anchors.topMargin: channelDelegate.dragging
                                            ? channelDelegate.dragY : 0
                                        anchors.bottomMargin: channelDelegate.dragging
                                            ? -channelDelegate.dragY : 0
                                        z: channelDelegate.dragging ? 100 : 0
                                        opacity: channelDelegate.dragging ? 0.9
                                               : (channelDelegate.isMuted ? 0.5 : 1.0)
                                        radius: Theme.r1
                                        color: {
                                            if (channelDelegate.dragging) return Theme.bg3;
                                            if (parent.isActive) return Theme.bg3;
                                            if (channelItemMouse.containsMouse) return Theme.bg2;
                                            return "transparent";
                                        }
                                        border.color: channelDelegate.dragging
                                            ? Theme.accent : "transparent"
                                        border.width: 1

                                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                        Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }

                                        // Invisible proxy for MouseArea.drag.target. We don't
                                        // actually move this item (it's unanchored, stays at
                                        // 0,0) — but handing MouseArea a drag target is what
                                        // unlocks its built-in drag-threshold behaviour, which
                                        // guarantees that a press-release under threshold falls
                                        // straight through to `onClicked` without ever
                                        // activating drag. The reorder logic reads mouseY
                                        // itself instead of relying on target movement.
                                        Item { id: dragProxy; width: 1; height: 1 }

                                        // Left accent stripe — on ANY active channel (text or voice),
                                        // per SPEC §3.2. Used to be voice-only.
                                        Rectangle {
                                            visible: parent.parent.isActive
                                            width: 2
                                            height: 16
                                            anchors.left: parent.left
                                            anchors.leftMargin: -1
                                            anchors.verticalCenter: parent.verticalCenter
                                            radius: 1
                                            color: Theme.accent
                                        }

                                        Column {
                                            id: channelItemContent
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.verticalCenter: parent.verticalCenter
                                            anchors.leftMargin: Theme.sp.s4
                                            anchors.rightMargin: Theme.sp.s3

                                            RowLayout {
                                                width: parent.width
                                                // Fatter rows on mobile so the
                                                // tap target matches the 44 pt
                                                // Apple / Material guideline.
                                                height: Theme.isMobile ? 44 : 28
                                                spacing: Theme.sp.s3

                                                // Screen reader: announce
                                                // as "Channel displayName,
                                                // N unread" (or "Voice
                                                // channel…") with Button
                                                // role so TalkBack treats
                                                // it as activatable.
                                                Accessible.role: Accessible.Button
                                                Accessible.name: (modelData.isVoice
                                                    ? "Voice channel " : "Channel ")
                                                    + modelData.displayName
                                                    + (modelData.unreadCount > 0
                                                       ? ", " + modelData.unreadCount
                                                         + " unread" : "")

                                                Icon {
                                                    name: modelData.isVoice ? "volume" : "hash"
                                                    size: 14
                                                    color: channelItemContent.parent.parent.isActive
                                                           ? Theme.accent
                                                           : Theme.fg2
                                                }

                                                Text {
                                                    text: modelData.displayName
                                                    font.family: Theme.fontSans
                                                    font.pixelSize: Theme.fontSize.base
                                                    font.weight: (channelItemContent.parent.parent.isActive
                                                                  || modelData.unreadCount > 0)
                                                                 ? Theme.fontWeight.medium
                                                                 : Theme.fontWeight.regular
                                                    color: channelItemContent.parent.parent.isActive
                                                           ? Theme.fg0
                                                           : (modelData.unreadCount > 0
                                                              ? Theme.fg0
                                                              : Theme.fg1)
                                                    elide: Text.ElideRight
                                                    Layout.fillWidth: true
                                                }

                                                // Typing indicator — three pulsing accent
                                                // dots on any channel (other than the
                                                // active one, which already shows the
                                                // full typing banner below the messages)
                                                // where at least one non-self user is
                                                // typing. Hidden otherwise. Binds to
                                                // typingGeneration so sync updates
                                                // re-evaluate the roomHasTyping check.
                                                Row {
                                                    readonly property int _typingGen:
                                                        serverManager.activeServer
                                                            ? serverManager.activeServer.typingGeneration : 0
                                                    readonly property bool hasTyping: {
                                                        _typingGen;
                                                        var s = serverManager.activeServer;
                                                        if (!s) return false;
                                                        if (modelData.roomId === s.activeRoomId) return false;
                                                        return s.roomHasTyping(modelData.roomId);
                                                    }
                                                    visible: hasTyping
                                                    spacing: 2
                                                    Layout.alignment: Qt.AlignVCenter
                                                    Repeater {
                                                        model: 3
                                                        delegate: Rectangle {
                                                            required property int index
                                                            width: 3; height: 3; radius: 1.5
                                                            color: Theme.accent
                                                            opacity: 0.3
                                                            SequentialAnimation on opacity {
                                                                loops: Animation.Infinite
                                                                running: parent.parent.visible
                                                                PauseAnimation { duration: index * 140 }
                                                                NumberAnimation { to: 1.0; duration: 280 }
                                                                NumberAnimation { to: 0.3; duration: 280 }
                                                                PauseAnimation { duration: (2 - index) * 140 }
                                                            }
                                                        }
                                                    }
                                                }

                                                // Mute indicator — shown for muted channels
                                                // so the dim styling doesn't look like a bug.
                                                Icon {
                                                    visible: channelDelegate.isMuted
                                                    name: "volume-off"
                                                    size: 12
                                                    color: Theme.fg3
                                                    Layout.alignment: Qt.AlignVCenter
                                                }

                                                // Unread dot — 8px accent circle, shown when
                                                // the room has messages newer than the stored
                                                // lastReadTs. Hidden for muted channels.
                                                Rectangle {
                                                    visible: channelDelegate.hasUnread
                                                    Layout.preferredWidth: 8
                                                    Layout.preferredHeight: 8
                                                    Layout.alignment: Qt.AlignVCenter
                                                    radius: 4
                                                    color: Theme.accent
                                                }

                                                // Voice participant count pill — subtle
                                                // bg3 chip with a `users` icon + count.
                                                // Only shown on voice channels with at
                                                // least one member; live-updates via
                                                // voiceMemberCount from the room model.
                                                Rectangle {
                                                    visible: modelData.isVoice && modelData.voiceMemberCount > 0
                                                    Layout.preferredWidth: voiceCountRow.implicitWidth + 10
                                                    Layout.preferredHeight: 18
                                                    radius: 9
                                                    color: modelData.roomId === (serverManager.activeServer
                                                            ? serverManager.activeServer.activeVoiceRoomId : "")
                                                        ? Qt.rgba(Theme.accent.r, Theme.accent.g,
                                                                  Theme.accent.b, 0.18)
                                                        : Theme.bg3
                                                    border.color: modelData.roomId === (serverManager.activeServer
                                                            ? serverManager.activeServer.activeVoiceRoomId : "")
                                                        ? Theme.accent : Theme.line
                                                    border.width: 1

                                                    Row {
                                                        id: voiceCountRow
                                                        anchors.centerIn: parent
                                                        spacing: 3
                                                        Icon {
                                                            anchors.verticalCenter: parent.verticalCenter
                                                            name: "users"
                                                            size: 10
                                                            color: modelData.roomId === (serverManager.activeServer
                                                                    ? serverManager.activeServer.activeVoiceRoomId : "")
                                                                ? Theme.accent : Theme.fg2
                                                        }
                                                        Text {
                                                            anchors.verticalCenter: parent.verticalCenter
                                                            text: modelData.voiceMemberCount
                                                            font.family: Theme.fontMono
                                                            font.pixelSize: 11
                                                            font.weight: Theme.fontWeight.semibold
                                                            color: modelData.roomId === (serverManager.activeServer
                                                                    ? serverManager.activeServer.activeVoiceRoomId : "")
                                                                ? Theme.accent : Theme.fg2
                                                        }
                                                    }
                                                }
                                            }

                                            // Voice members nested list
                                            // (SPEC §3.2: 16px indent, 22h rows, 16×16 avatar).
                                            // `voiceMembers` on the connection already carries
                                            // displayName + peerState, so we render directly
                                            // without another lookup.
                                            Column {
                                                visible: modelData.isVoice && modelData.voiceMemberCount > 0
                                                leftPadding: 16
                                                spacing: 2
                                                bottomPadding: 4
                                                width: parent.width

                                                Repeater {
                                                    model: {
                                                        if (!serverManager.activeServer) return [];
                                                        if (modelData.roomId !== serverManager.activeServer.activeVoiceRoomId) return [];
                                                        return serverManager.activeServer.voiceMembers;
                                                    }
                                                    delegate: Item {
                                                        id: voiceMemberRow
                                                        required property var modelData
                                                        width: parent.width
                                                        height: 22

                                                        readonly property bool isSelf:
                                                            serverManager.activeServer
                                                            && modelData.user_id === serverManager.activeServer.userId
                                                        readonly property bool muted:
                                                            modelData.muted === true
                                                        readonly property bool deafened:
                                                            modelData.deafened === true
                                                        readonly property real micLevel:
                                                            isSelf && serverManager.activeServer
                                                            ? serverManager.activeServer.micLevel : 0
                                                        readonly property bool speaking:
                                                            isSelf && !muted && micLevel > 0.05

                                                        Row {
                                                            anchors.verticalCenter: parent.verticalCenter
                                                            anchors.left: parent.left
                                                            anchors.right: parent.right
                                                            spacing: Theme.sp.s3

                                                            // 16×16 avatar. Green ring pulses when
                                                            // the local mic detects speech above
                                                            // the silence floor — only for self,
                                                            // since we don't get remote-speaking
                                                            // signalling yet.
                                                            Item {
                                                                width: 18; height: 18
                                                                Rectangle {
                                                                    anchors.centerIn: parent
                                                                    width: 16; height: 16
                                                                    radius: 8
                                                                    color: Theme.senderColor(
                                                                        voiceMemberRow.modelData.user_id || "")
                                                                    opacity: voiceMemberRow.muted
                                                                          || voiceMemberRow.deafened ? 0.5 : 1.0
                                                                    Text {
                                                                        anchors.centerIn: parent
                                                                        text: {
                                                                            var m = voiceMemberRow.modelData;
                                                                            var n = (m.displayName || m.user_id || "?");
                                                                            var s = n.replace(/^[^a-zA-Z0-9]+/, "");
                                                                            return (s.length > 0 ? s.charAt(0) : "?").toUpperCase();
                                                                        }
                                                                        font.family: Theme.fontSans
                                                                        font.pixelSize: 9
                                                                        font.weight: Theme.fontWeight.semibold
                                                                        color: Theme.onAccent
                                                                    }
                                                                }
                                                                Rectangle {
                                                                    anchors.centerIn: parent
                                                                    width: 18 + voiceMemberRow.micLevel * 6
                                                                    height: width
                                                                    radius: width / 2
                                                                    color: "transparent"
                                                                    border.color: Theme.online
                                                                    border.width: 1.5
                                                                    opacity: voiceMemberRow.speaking ? 0.8 : 0
                                                                    visible: opacity > 0.01
                                                                    Behavior on opacity { NumberAnimation { duration: 80 } }
                                                                    Behavior on width { NumberAnimation { duration: 60 } }
                                                                }
                                                            }

                                                            Text {
                                                                anchors.verticalCenter: parent.verticalCenter
                                                                text: voiceMemberRow.modelData.displayName
                                                                      || voiceMemberRow.modelData.user_id
                                                                      || ""
                                                                font.family: Theme.fontSans
                                                                font.pixelSize: Theme.fontSize.sm
                                                                font.weight: voiceMemberRow.isSelf
                                                                    ? Theme.fontWeight.semibold
                                                                    : Theme.fontWeight.regular
                                                                color: voiceMemberRow.muted
                                                                    || voiceMemberRow.deafened
                                                                    ? Theme.fg3 : Theme.fg1
                                                                elide: Text.ElideRight
                                                                width: voiceMemberRow.width - 18 - 24 - Theme.sp.s3 * 2
                                                            }

                                                            // Trailing status icon: deafened >
                                                            // muted > speaking (none). Tiny 12px
                                                            // glyph in danger red so the row stays
                                                            // scannable.
                                                            Item {
                                                                anchors.verticalCenter: parent.verticalCenter
                                                                width: 14; height: 14
                                                                Icon {
                                                                    anchors.centerIn: parent
                                                                    visible: voiceMemberRow.deafened
                                                                           || voiceMemberRow.muted
                                                                    name: voiceMemberRow.deafened
                                                                          ? "headphones-off" : "mic-off"
                                                                    size: 11
                                                                    color: Theme.danger
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        // Unified click + drag handler. Uses MouseArea's
                                        // built-in drag (with drag.target: dragProxy) so
                                        // taps under the 6-pixel threshold fall straight
                                        // through to onClicked for channel selection, and
                                        // only a deliberate drag past threshold activates
                                        // reorder mode — fixes the "clicks don't work"
                                        // pathology from the old DragHandler approach.
                                        MouseArea {
                                            id: channelItemMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                                            drag.target: dragProxy
                                            drag.axis: Drag.YAxis
                                            drag.threshold: 6

                                            // Scene-Y of the press point — invariant to the
                                            // row's own lift via anchor.topMargin (which
                                            // moves the MouseArea and would otherwise feed
                                            // back into local mouse.y, causing oscillation).
                                            property real _pressSceneY: 0

                                            onPressed: (mouse) => {
                                                _pressSceneY = mapToItem(null, mouse.x, mouse.y).y;
                                            }

                                            onPositionChanged: (mouse) => {
                                                if (!drag.active) return;
                                                channelDelegate.dragging = true;
                                                var scene = mapToItem(null, mouse.x, mouse.y);
                                                channelDelegate.dragY = scene.y - _pressSceneY;
                                                var t = channelListRoot.dropTargetAt(scene.y);
                                                channelListRoot.dropHoverCategoryId =
                                                    (t && t.categoryId !== categoryDelegate.categoryId)
                                                        ? t.categoryId : "__none__";
                                            }

                                            onReleased: (mouse) => {
                                                if (!channelDelegate.dragging) return;
                                                var scene = mapToItem(null, mouse.x, mouse.y);
                                                var t = channelListRoot.dropTargetAt(scene.y);
                                                var sourceCat = categoryDelegate.categoryId;
                                                channelListRoot.dropHoverCategoryId = "__none__";
                                                if (t && t.categoryId !== sourceCat) {
                                                    var relY = scene.y - t.top;
                                                    var slot = Math.round(relY / channelDelegate.height);
                                                    channelListRoot.moveChannelAcross(
                                                        modelData.roomId, t.categoryId,
                                                        t.categoryChannels, slot);
                                                } else {
                                                    var slots = Math.round(
                                                        channelDelegate.dragY / channelDelegate.height);
                                                    var fromIdx = channelDelegate.channelIndex;
                                                    var toIdx = Math.max(0, Math.min(
                                                        channelRepeater.count - 1, fromIdx + slots));
                                                    if (toIdx !== fromIdx) {
                                                        channelListRoot.moveChannelTo(
                                                            channelRepeater.model, fromIdx, toIdx);
                                                    }
                                                }
                                                channelDelegate.dragY = 0;
                                                channelDelegate.dragging = false;
                                            }

                                            onClicked: (mouse) => {
                                                if (mouse.button === Qt.RightButton) {
                                                    roomContextMenu.roomId = modelData.roomId;
                                                    roomContextMenu.roomName = modelData.displayName;
                                                    roomContextMenu.popup();
                                                    return;
                                                }
                                                if (!serverManager.activeServer) return;
                                                if (modelData.isVoice) {
                                                    // Mobile voice needs TWO permissions before join:
                                                    //   RECORD_AUDIO         — for the mic
                                                    //   POST_NOTIFICATIONS   — for the foreground
                                                    //                          service notification.
                                                    // Without POST_NOTIFICATIONS on Android 13+ the
                                                    // FGS silently fails to surface and the OS may
                                                    // demote the service, killing the call the
                                                    // moment the app backgrounds. We chain the two
                                                    // dialogs so the user answers them in sequence.
                                                    var rid = modelData.roomId;
                                                    var srv = serverManager.activeServer;
                                                    var join = function() {
                                                        if (srv.activeVoiceRoomId === rid) {
                                                            srv.showVoiceRoom();
                                                        } else {
                                                            srv.joinVoiceChannel(rid);
                                                        }
                                                    };

                                                    if (!Theme.isMobile
                                                        || typeof androidPerms === "undefined") {
                                                        join();
                                                        return;
                                                    }

                                                    var askMic = function(afterMic) {
                                                        if (androidPerms.hasMicrophone()) {
                                                            afterMic(true);
                                                            return;
                                                        }
                                                        var once = null;
                                                        once = function(granted) {
                                                            androidPerms.microphoneResult.disconnect(once);
                                                            afterMic(granted);
                                                        };
                                                        androidPerms.microphoneResult.connect(once);
                                                        androidPerms.requestMicrophone();
                                                    };
                                                    var askNotifs = function(afterNotifs) {
                                                        if (androidPerms.hasNotifications()) {
                                                            afterNotifs(true);
                                                            return;
                                                        }
                                                        var once = null;
                                                        once = function(granted) {
                                                            androidPerms.notificationsResult.disconnect(once);
                                                            // Non-fatal if user denies — the
                                                            // foreground service will still run,
                                                            // just without the persistent
                                                            // notification, which Android may then
                                                            // demote out of FGS state. We warn
                                                            // but proceed.
                                                            afterNotifs(granted);
                                                        };
                                                        androidPerms.notificationsResult.connect(once);
                                                        androidPerms.requestNotifications();
                                                    };

                                                    askMic(function(micGranted) {
                                                        if (!micGranted) {
                                                            var w = Window.window;
                                                            if (w && w.toast) {
                                                                w.toast("Microphone permission "
                                                                      + "is required to join voice.",
                                                                      "error");
                                                            }
                                                            return;
                                                        }
                                                        askNotifs(function(notifGranted) {
                                                            if (!notifGranted) {
                                                                var w = Window.window;
                                                                if (w && w.toast) {
                                                                    w.toast("Allow notifications "
                                                                          + "to keep voice running "
                                                                          + "in the background.",
                                                                          "info");
                                                                }
                                                            }
                                                            join();
                                                        });
                                                    });
                                                    return;
                                                } else {
                                                    serverManager.activeServer.setActiveRoom(modelData.roomId);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        } // categoryContent Column
                    }
                }
            }
        }

        // VoiceStatusCard (SPEC §3.2 bottom) — compact "you're in a voice
        // room" summary shown in the channel sidebar. Distinct from the
        // main-column VoiceDock (which carries the full control set). Only
        // visible when actually connected to a voice channel.
        Item {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.sp.s3
            Layout.rightMargin: Theme.sp.s3
            Layout.topMargin: Theme.sp.s3
            Layout.preferredHeight: visible ? 72 : 0
            visible: serverManager.activeServer !== null
                     && serverManager.activeServer.inVoiceChannel

            Rectangle {
                id: voiceStatusCard
                anchors.fill: parent
                radius: Theme.r2
                color: voiceCardHover.containsMouse && !serverManager.activeServer.viewingVoiceRoom
                       ? Theme.bg3 : Theme.bg2
                border.width: 1
                border.color: serverManager.activeServer.viewingVoiceRoom
                              ? Theme.accent : Theme.line
                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                Behavior on border.color { ColorAnimation { duration: Theme.motion.fastMs } }

                // Faint accent tab along the left edge — echoes the active-
                // channel stripe in the list above so the connection status
                // reads at a glance.
                Rectangle {
                    width: 3
                    height: parent.height - 16
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 0
                    radius: 1.5
                    color: Theme.accent
                }

                // Click anywhere on the card (except the disconnect button)
                // to jump back into the VoiceRoom view. The disconnect
                // MouseArea sits above this one in QML ordering so its
                // onClicked consumes the event first.
                MouseArea {
                    id: voiceCardHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (serverManager.activeServer)
                                   serverManager.activeServer.showVoiceRoom()
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.sp.s5
                    anchors.rightMargin: Theme.sp.s3
                    anchors.topMargin: Theme.sp.s3
                    anchors.bottomMargin: Theme.sp.s3
                    spacing: Theme.sp.s3

                    // Signal-bars icon tinted accent — stands in for "live
                    // voice connection" in the same spot where the active-
                    // stripe would sit on a normal row.
                    Icon {
                        name: "signal"
                        size: 18
                        color: Theme.accent
                        Layout.alignment: Qt.AlignVCenter
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0

                        // "Voice connected" label in fg2, small caps wide-
                        // tracked so it reads as a status line not a title.
                        Text {
                            text: "VOICE CONNECTED"
                            font.family: Theme.fontSans
                            font.pixelSize: 10
                            font.weight: Theme.fontWeight.semibold
                            font.letterSpacing: Theme.trackWidest.xl
                            color: Theme.fg3
                            Layout.fillWidth: true
                        }
                        // Active voice channel name — the "#room" line.
                        Text {
                            text: {
                                var s = serverManager.activeServer;
                                if (!s || !s.activeVoiceRoomId) return "";
                                var n = s.roomListModel
                                        ? s.roomListModel.roomDisplayName(s.activeVoiceRoomId)
                                        : s.activeVoiceRoomId;
                                return "#" + n;
                            }
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.base
                            font.weight: Theme.fontWeight.semibold
                            color: Theme.fg0
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        // Latency placeholder — the controller doesn't
                        // publish one yet, so we fall back to the server
                        // host. When CallController.latencyMs lands, swap.
                        Text {
                            text: serverManager.activeServer
                                  ? serverManager.activeServer.serverName : ""
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fontSize.xs
                            color: Theme.fg2
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }

                    // Ghost-style disconnect button — danger-tinted X that
                    // solidifies to a filled red square on hover. Keeps the
                    // card quiet until the user means it.
                    Rectangle {
                        Layout.preferredWidth: 28
                        Layout.preferredHeight: 28
                        Layout.alignment: Qt.AlignVCenter
                        radius: Theme.r2
                        color: disconnectArea.containsMouse ? Theme.danger : "transparent"
                        border.width: disconnectArea.containsMouse ? 0 : 1
                        border.color: Theme.line
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                        Icon {
                            anchors.centerIn: parent
                            name: "phone-off"
                            size: 14
                            color: disconnectArea.containsMouse ? "white" : Theme.danger
                        }
                        MouseArea {
                            id: disconnectArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: if (serverManager.activeServer)
                                           serverManager.activeServer.leaveVoiceChannel()
                        }
                        ToolTip.visible: disconnectArea.containsMouse
                        ToolTip.text: "Disconnect"
                        ToolTip.delay: 400
                    }
                }
            }

            Behavior on Layout.preferredHeight {
                NumberAnimation { duration: Theme.motion.normalMs
                                  easing.type: Easing.BezierSpline
                                  easing.bezierCurve: Theme.motion.bezier }
            }
        }

        // "Join room by ID" field — legacy dev affordance, dropped from
        // the SPEC. Invite-based joining replaces it when that feature
        // lands. Kept here as a 0-height placeholder so removing the
        // block doesn't break any layout.
        Item { Layout.preferredHeight: 0 }

        // Self-user panel at bottom (SPEC §3.2, 52h, bg bg0).
        // Left: profile cluster (clickable — opens the user menu).
        // Right: mute / deafen / settings icon buttons.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 52
            color: Theme.bg0

            // Top divider — softens the join between scrolling channel list
            // and the fixed footer.
            Rectangle {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 1
                color: Theme.line
            }

            // Shared spec for the three 28×28 icon buttons on the right
            // (mute / deafen / settings). `toggled` paints the glyph in
            // danger — SPEC §3.5 DockButton convention applied here too,
            // so the vocabulary reads the same in both places.
            component FooterButton: Rectangle {
                id: fbtn
                property string icon: ""
                property bool   toggled: false
                property string tooltip: ""
                signal clicked()

                implicitWidth: 28
                implicitHeight: 28
                radius: Theme.r1
                color: toggled
                       ? Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.18)
                       : fbtnHover.containsMouse ? Theme.bg3 : "transparent"
                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                Icon {
                    anchors.centerIn: parent
                    name: fbtn.icon
                    size: 16
                    color: fbtn.toggled ? Theme.danger
                         : fbtnHover.containsMouse ? Theme.fg0
                         : Theme.fg1
                }

                MouseArea {
                    id: fbtnHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: fbtn.clicked()
                }
                ToolTip.visible: fbtnHover.containsMouse && tooltip.length > 0
                ToolTip.text: tooltip
                ToolTip.delay: 500
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.sp.s4
                anchors.rightMargin: Theme.sp.s2
                spacing: Theme.sp.s3

                // Profile cluster on the left — avatar + name + mxid. The
                // cluster itself is the menu opener; the buttons on the
                // right get their own click handlers via FooterButton.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.topMargin: 4
                    Layout.bottomMargin: 4
                    radius: Theme.r1
                    color: userInfoMouse.containsMouse ? Theme.bg2 : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 4
                        anchors.rightMargin: 4
                        spacing: Theme.sp.s3

                        // Avatar with integrated presence dot — status
                        // overlays the bottom-right of the avatar rather
                        // than floating separately beside it.
                        Item {
                            Layout.preferredWidth: Theme.avatar.md
                            Layout.preferredHeight: Theme.avatar.md
                            Layout.alignment: Qt.AlignVCenter

                            Rectangle {
                                anchors.fill: parent
                                radius: Theme.r2
                                color: Theme.senderColor(serverManager.activeServer
                                                         ? serverManager.activeServer.userId : "")

                                Image {
                                    anchors.fill: parent
                                    source: {
                                        if (serverManager.activeServer && serverManager.activeServer.avatarUrl !== "") {
                                            return serverManager.activeServer.resolveMediaUrl(serverManager.activeServer.avatarUrl);
                                        }
                                        return "";
                                    }
                                    visible: source !== ""
                                    fillMode: Image.PreserveAspectCrop
                                    layer.enabled: true
                                }

                                Text {
                                    anchors.centerIn: parent
                                    text: {
                                        if (!serverManager.activeServer) return "?";
                                        var name = serverManager.activeServer.displayName
                                                || serverManager.activeServer.userId;
                                        var i = name.charAt(0) === '@' ? 1 : 0;
                                        return i < name.length ? name.charAt(i).toUpperCase() : "?";
                                    }
                                    font.family: Theme.fontSans
                                    font.pixelSize: 13
                                    font.weight: Theme.fontWeight.semibold
                                    color: Theme.onAccent
                                    visible: !serverManager.activeServer || serverManager.activeServer.avatarUrl === ""
                                }
                            }

                            // Presence-style dot — tracks connection status.
                            Rectangle {
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.rightMargin: -2
                                anchors.bottomMargin: -2
                                width: 12
                                height: 12
                                radius: 6
                                border.color: Theme.bg0
                                border.width: 2
                                color: {
                                    if (!serverManager.activeServer) return Theme.fg3;
                                    switch (serverManager.activeServer.connectionStatus) {
                                    case 1: return Theme.online;
                                    case 2: return Theme.warn;
                                    default: return Theme.danger;
                                    }
                                }
                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0

                            Text {
                                text: serverManager.activeServer ? serverManager.activeServer.displayName : ""
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.md
                                font.weight: Theme.fontWeight.semibold
                                color: Theme.fg0
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }

                            Text {
                                text: serverManager.activeServer ? serverManager.activeServer.userId : ""
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.fontSize.xs
                                color: Theme.fg3
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }
                    }

                    MouseArea {
                        id: userInfoMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            // Anchor above the profile block so it opens
                            // upward rather than clipping off-screen.
                            userMenu.popup(0, -userMenu.implicitHeight);
                        }
                    }
                }

                // Mute / deafen / settings trio (SPEC §3.2). Bound to the
                // active ServerConnection so toggling here tracks the
                // state shown in the VoiceDock and vice-versa. Hidden
                // on mobile since voice is off there and the buttons
                // eat precious horizontal space that the user's
                // display name + MXID need (otherwise they elide to
                // "@j…" / "@jo…" in the 272 px channel drawer).
                FooterButton {
                    Layout.alignment: Qt.AlignVCenter
                    icon: serverManager.activeServer
                          && serverManager.activeServer.voiceMuted ? "mic-off" : "mic"
                    toggled: serverManager.activeServer
                             && serverManager.activeServer.voiceMuted
                    tooltip: toggled ? "Unmute" : "Mute microphone"
                    onClicked: if (serverManager.activeServer)
                                   serverManager.activeServer.toggleMute()
                }
                FooterButton {
                    Layout.alignment: Qt.AlignVCenter
                    icon: serverManager.activeServer
                          && serverManager.activeServer.voiceDeafened
                          ? "headphones-off" : "headphones"
                    toggled: serverManager.activeServer
                             && serverManager.activeServer.voiceDeafened
                    tooltip: toggled ? "Undeafen" : "Deafen headphones"
                    onClicked: if (serverManager.activeServer)
                                   serverManager.activeServer.toggleDeafen()
                }
                FooterButton {
                    Layout.alignment: Qt.AlignVCenter
                    icon: "settings"
                    tooltip: "Client settings"
                    onClicked: Window.window.openClientSettings()
                }
            }

            // Menu for the bottom user-profile block. Inline-styled items
            // so the custom chrome actually renders (Menu.delegate only
            // applies to model-bound items, not declared children).
            Menu {
                id: userMenu
                background: Rectangle {
                    color: Theme.bg1
                    radius: Theme.r2
                    border.color: Theme.line
                    border.width: 1
                    implicitWidth: 220
                }

                component ThemedUserItem: MenuItem {
                    id: ui
                    implicitHeight: visible ? 34 : 0
            height: implicitHeight
                    property string iconName: ""
                    property color labelColor: Theme.fg0
                    contentItem: RowLayout {
                        spacing: Theme.sp.s3
                        Icon {
                            name: ui.iconName
                            size: 14
                            color: ui.hovered ? Theme.fg0 : Theme.fg2
                            Layout.leftMargin: Theme.sp.s3
                        }
                        Text {
                            text: ui.text
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.md
                            color: ui.labelColor
                            Layout.fillWidth: true
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    background: Rectangle {
                        color: ui.hovered ? Theme.bg2 : "transparent"
                        radius: Theme.r1
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                }

                // Quick access to the presence + custom status
                // picker. Users open this most often, hence top
                // of menu. Renders a tiny presence dot in the
                // text label so it doubles as a state indicator.
                ThemedUserItem {
                    text: {
                        var s = serverManager.activeServer;
                        if (!s) return "Set status…";
                        var msg = s.selfStatusMessage();
                        if (msg && msg.length > 0)
                            return "Status: " + msg;
                        switch (s.selfPresence()) {
                        case "online":      return "Set status…  (Online)";
                        case "unavailable": return "Set status…  (Away)";
                        case "offline":     return "Set status…  (Offline)";
                        }
                        return "Set status…";
                    }
                    iconName: "smile"
                    onTriggered: Window.window.openStatusPicker()
                }

                MenuSeparator { }

                ThemedUserItem {
                    text: "Manage Account"
                    iconName: "shield"
                    onTriggered: {
                        // Open the identity portal in the default browser.
                        // Uses the provider URL from the OIDC login if
                        // available; falls back to id.bsfchat.com.
                        var base = serverManager.activeServer
                            ? serverManager.activeServer.identityProviderUrl()
                            : "";
                        if (!base) base = "https://id.bsfchat.com";
                        Qt.openUrlExternally(base + "/profile.html");
                    }
                }
                ThemedUserItem {
                    text: "Edit Server Profile"
                    iconName: "edit"
                    onTriggered: Window.window.openUserSettings()
                }
                ThemedUserItem {
                    text: "Client Settings"
                    iconName: "settings"
                    onTriggered: Window.window.openClientSettings()
                }
                ThemedUserItem {
                    text: "Keyboard Shortcuts"
                    iconName: "crosshair"
                    visible: !Theme.isMobile
                    onTriggered: Window.window.openShortcutsDialog()
                }
            }
        }
    }

    // Empty state — shown when the user hasn't selected a server (no
    // servers added, or they're on the add-server flow). Mirrors the
    // empty-state vocabulary we use elsewhere: accent-tinted icon tile,
    // headline, subtext, then a primary action pulling the LoginDialog.
    ColumnLayout {
        anchors.centerIn: parent
        visible: serverManager.activeServer === null
        width: Math.min(parent.width - Theme.sp.s7 * 2, 280)
        spacing: Theme.sp.s4

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 56
            Layout.preferredHeight: 56
            radius: Theme.r3
            color: Theme.bg2
            border.color: Theme.line
            border.width: 1
            Icon {
                anchors.centerIn: parent
                name: "hash"
                size: 24
                color: Theme.accent
            }
        }
        Text {
            Layout.alignment: Qt.AlignHCenter
            text: {
                var count = serverManager.servers ? serverManager.servers.rowCount() : 0;
                return count === 0 ? "Welcome to BSFChat" : "No server selected";
            }
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
            text: {
                var count = serverManager.servers ? serverManager.servers.rowCount() : 0;
                return count === 0
                    ? "Add a server to start chatting. Sign in with a BSFChat ID to pull in all the servers you've joined."
                    : "Pick a server from the sidebar to see its channels.";
            }
            color: Theme.fg2
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.sm
            wrapMode: Text.WordWrap
        }

        // Primary action — accent pill opens the login / add-server
        // dialog. Only shown in the true "no servers at all" state; once
        // the user has servers, the "Pick a server" subtext directs them
        // to the rail without another competing CTA.
        Button {
            id: addServerCta
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: Theme.sp.s2
            visible: serverManager.servers && serverManager.servers.rowCount() === 0
            contentItem: RowLayout {
                spacing: Theme.sp.s2
                Icon {
                    name: "plus"; size: 14; color: Theme.onAccent
                    Layout.alignment: Qt.AlignVCenter
                }
                Text {
                    text: "Add a server"
                    color: Theme.onAccent
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    font.weight: Theme.fontWeight.semibold
                    Layout.alignment: Qt.AlignVCenter
                }
            }
            background: Rectangle {
                color: addServerCta.hovered ? Theme.accentDim : Theme.accent
                radius: Theme.r2
                implicitHeight: 40
                implicitWidth: 160
                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
            }
            onClicked: loginDialog.open()
        }
    }

    // Room context menu — inline-styled items, danger action uses
    // `labelColor: Theme.danger` to distinguish itself.
    Menu {
        id: roomContextMenu
        property string roomId: ""
        property string roomName: ""

        background: Rectangle {
            color: Theme.bg1
            radius: Theme.r2
            border.color: Theme.line
            border.width: 1
            implicitWidth: 200
        }

        component ThemedRoomItem: MenuItem {
            id: ri
            implicitHeight: visible ? 34 : 0
            height: implicitHeight
            property string iconName: ""
            property color labelColor: Theme.fg0
            contentItem: RowLayout {
                spacing: Theme.sp.s3
                Icon {
                    name: ri.iconName
                    size: 14
                    color: !ri.enabled ? Theme.fg3
                         : (ri.hovered ? ri.labelColor : Theme.fg2)
                    Layout.leftMargin: Theme.sp.s3
                }
                Text {
                    text: ri.text
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    color: !ri.enabled ? Theme.fg3 : ri.labelColor
                    Layout.fillWidth: true
                    verticalAlignment: Text.AlignVCenter
                }
            }
            background: Rectangle {
                color: ri.hovered && ri.enabled ? Theme.bg2 : "transparent"
                radius: Theme.r1
                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
            }
        }

        ThemedRoomItem {
            text: "Mark as Read"
            iconName: "check"
            enabled: {
                channelListRoot.unreadGeneration;
                if (!serverManager.activeServer) return false;
                var rid = roomContextMenu.roomId;
                if (!rid) return false;
                // Only offer when there's actually something unread.
                var s = serverManager.activeServer;
                var groups = s.categorizedRooms;
                for (var i = 0; i < groups.length; i++) {
                    var ch = groups[i].channels || [];
                    for (var j = 0; j < ch.length; j++) {
                        if (ch[j].roomId === rid) {
                            var lastMsg = ch[j].lastMessageTime || 0;
                            var lastRead = appSettings.lastReadTs(rid);
                            return lastMsg > 0 && lastRead < lastMsg;
                        }
                    }
                }
                return false;
            }
            onTriggered: {
                var rid = roomContextMenu.roomId;
                appSettings.setLastReadTs(rid, Date.now());
                channelListRoot.unreadGeneration++;
            }
        }

        ThemedRoomItem {
            text: appSettings.isRoomMuted(roomContextMenu.roomId)
                  ? "Unmute Channel" : "Mute Channel"
            iconName: "volume-off"
            onTriggered: {
                var rid = roomContextMenu.roomId;
                appSettings.setRoomMuted(rid, !appSettings.isRoomMuted(rid));
                channelListRoot.muteGeneration++;
            }
        }

        // Three-state notification mode selector via stacked menu
        // items (Menu doesn't support submenus that feel native on
        // mobile, so we list the choices inline with a check on the
        // currently-selected mode). Changes are persisted
        // immediately and consumed by NotificationManager at the
        // next inbound message.
        MenuSeparator {
            contentItem: Rectangle {
                implicitWidth: 180
                implicitHeight: 1
                color: Theme.line
            }
        }
        ThemedRoomItem {
            text: appSettings.roomNotificationMode(roomContextMenu.roomId)
                  === "all"
                  ? "Notifications: All ✓" : "Notifications: All"
            iconName: "inbox"
            onTriggered: {
                appSettings.setRoomNotificationMode(
                    roomContextMenu.roomId, "all");
                channelListRoot.muteGeneration++;
            }
        }
        ThemedRoomItem {
            text: appSettings.roomNotificationMode(roomContextMenu.roomId)
                  === "mentions"
                  ? "Notifications: @Mentions only ✓"
                  : "Notifications: @Mentions only"
            iconName: "at"
            onTriggered: {
                appSettings.setRoomNotificationMode(
                    roomContextMenu.roomId, "mentions");
                channelListRoot.muteGeneration++;
            }
        }
        ThemedRoomItem {
            text: appSettings.roomNotificationMode(roomContextMenu.roomId)
                  === "none"
                  ? "Notifications: None ✓"
                  : "Notifications: None"
            iconName: "minus"
            onTriggered: {
                appSettings.setRoomNotificationMode(
                    roomContextMenu.roomId, "none");
                channelListRoot.muteGeneration++;
            }
        }

        ThemedRoomItem {
            text: "Channel Settings…"
            iconName: "settings"
            enabled: {
                if (!serverManager.activeServer) return false;
                if (serverManager.activeServer.permissionsGeneration < 0) return false;
                return serverManager.activeServer.canManageChannel(roomContextMenu.roomId);
            }
            onTriggered: {
                channelSettingsPopup.roomId = roomContextMenu.roomId;
                channelSettingsPopup.roomName = roomContextMenu.roomName;
                channelSettingsPopup.open();
            }
        }

        ThemedRoomItem {
            text: "Delete Channel"
            iconName: "x"
            labelColor: Theme.danger
            enabled: {
                if (!serverManager.activeServer) return false;
                if (serverManager.activeServer.permissionsGeneration < 0) return false;
                return serverManager.activeServer.canManageChannel(roomContextMenu.roomId);
            }
            onTriggered: {
                deleteChannelConfirm.roomId = roomContextMenu.roomId;
                deleteChannelConfirm.roomName = roomContextMenu.roomName;
                deleteChannelConfirm.open();
            }
        }
    }

    // Confirmation popup for channel deletion. Keeps us from nuking a
    // channel with a misclick; MANAGE_CHANNELS already gates the action
    // server-side but the UX still needs the prompt.
    Popup {
        id: deleteChannelConfirm
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        width: 400
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: Theme.sp.s7

        property string roomId: ""
        property string roomName: ""

        // Match the rest of the dialog vocabulary: bg1 body + r3 + line
        // border, not the older bg2/bg3/r2 treatment this used to use.
        background: Rectangle {
            color: Theme.bg1
            radius: Theme.r3
            border.color: Theme.line
            border.width: 1
        }

        contentItem: ColumnLayout {
            spacing: Theme.sp.s4

            // Danger icon + title, so a destructive confirmation reads at a
            // glance even without the button colour cue.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp.s3
                Rectangle {
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 32
                    radius: Theme.r2
                    color: Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.15)
                    Icon {
                        anchors.centerIn: parent
                        name: "x"
                        size: 16
                        color: Theme.danger
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: "Delete #" + deleteChannelConfirm.roomName + "?"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xl
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackTight.xl
                    color: Theme.fg0
                    wrapMode: Text.WordWrap
                }
            }
            Text {
                Layout.fillWidth: true
                text: "This removes the channel and every message in it for everyone. Cannot be undone."
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.sp.s1
                spacing: Theme.sp.s3

                Item { Layout.fillWidth: true }

                Button {
                    id: deleteCancelBtn
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
                        color: deleteCancelBtn.hovered ? Theme.bg3 : "transparent"
                        border.color: Theme.line
                        border.width: 1
                        radius: Theme.r2
                        implicitWidth: 100
                        implicitHeight: 36
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                    onClicked: deleteChannelConfirm.close()
                }
                Button {
                    id: deleteConfirmBtn
                    contentItem: Text {
                        text: "Delete channel"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.onAccent
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: deleteConfirmBtn.hovered ? Qt.lighter(Theme.danger, 1.1) : Theme.danger
                        radius: Theme.r2
                        implicitWidth: 140
                        implicitHeight: 36
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                    onClicked: {
                        if (serverManager.activeServer && deleteChannelConfirm.roomId !== "") {
                            serverManager.activeServer.deleteChannel(deleteChannelConfirm.roomId);
                        }
                        deleteChannelConfirm.close();
                    }
                }
            }
        }
    }

    ChannelSettings {
        id: channelSettingsPopup
        // Must reparent to the application Overlay so width/height bindings
        // resolve against the full window, not the 240-px channel-list pane.
        parent: Overlay.overlay
    }

    // Create-item context menu: category / text channel / voice channel.
    // Category context (if any) is set by the caller; "Create Category" ignores
    // it because categories can't nest in our model.
    //
    // Each MenuItem styles itself inline. `Menu.delegate` only applies to
    // model-bound items, so declared children have to carry their own
    // contentItem/background.
    Menu {
        id: createMenu
        property string categoryId: ""

        background: Rectangle {
            color: Theme.bg1
            radius: Theme.r2
            border.color: Theme.line
            border.width: 1
            implicitWidth: 220
        }

        component ThemedCreateItem: MenuItem {
            id: ci
            implicitHeight: visible ? 34 : 0
            height: implicitHeight
            property string iconName: ""
            contentItem: RowLayout {
                spacing: Theme.sp.s3
                Icon {
                    name: ci.iconName
                    size: 14
                    color: ci.hovered ? Theme.fg0 : Theme.fg2
                    Layout.leftMargin: Theme.sp.s3
                }
                Text {
                    text: ci.text
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    color: Theme.fg0
                    Layout.fillWidth: true
                    verticalAlignment: Text.AlignVCenter
                }
            }
            background: Rectangle {
                color: ci.hovered ? Theme.bg2 : "transparent"
                radius: Theme.r1
                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
            }
        }

        ThemedCreateItem {
            text: "Create Category"
            iconName: "chevron-down"
            onTriggered: channelListRoot.openCreatePrompt("category", "")
        }
        ThemedCreateItem {
            text: "Create Text Channel"
            iconName: "hash"
            onTriggered: channelListRoot.openCreatePrompt("text", createMenu.categoryId)
        }
        ThemedCreateItem {
            text: "Create Voice Channel"
            iconName: "volume"
            onTriggered: channelListRoot.openCreatePrompt("voice", createMenu.categoryId)
        }
    }

    // Minimal "just give it a name" prompt used for all three create flows.
    // The type was already chosen in the context menu so we don't re-ask here.
    Popup {
        id: createPrompt
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        width: 380
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: Theme.sp.s7

        property string kind: "text"            // category|text|voice
        property string categoryId: ""          // where to place a text/voice channel
        property alias nameField: nameInput
        property bool makePrivate: false        // only used for text/voice

        readonly property string promptTitle: kind === "category" ? "Create Category"
                                             : kind === "voice"   ? "Create Voice Channel"
                                             :                      "Create Text Channel"
        readonly property string placeholder: kind === "category" ? "Announcements"
                                            : kind === "voice"    ? "General Voice"
                                            :                       "general"

        // Match the rest of the dialog vocabulary: bg1 body + r3 + line.
        background: Rectangle {
            color: Theme.bg1
            radius: Theme.r3
            border.color: Theme.line
            border.width: 1
        }

        function submit() {
            var name = nameInput.text.trim();
            if (!name || !serverManager.activeServer) return;
            if (createPrompt.kind === "category") {
                serverManager.activeServer.createCategory(name);
            } else {
                var voice = createPrompt.kind === "voice";
                serverManager.activeServer.createChannelInCategory(
                    name, createPrompt.categoryId, voice, createPrompt.makePrivate);
            }
            createPrompt.close();
        }

        onOpened: {
            // Reset transient state whenever a fresh prompt opens.
            makePrivate = false;
        }

        contentItem: ColumnLayout {
            spacing: Theme.sp.s4

            // Title + divider — matches the SPEC §3.10 section-header
            // vocabulary used across the other settings dialogs.
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp.s3
                Text {
                    text: createPrompt.promptTitle
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xl
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackTight.xl
                    color: Theme.fg0
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.line }
            }

            // Channel-name label — widest-tracked small-caps fg3 matches
            // the rest of the app's label vocabulary.
            Text {
                text: createPrompt.kind === "category" ? "CATEGORY NAME" : "CHANNEL NAME"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWidest.xs
                color: Theme.fg3
            }

            TextField {
                id: nameInput
                Layout.fillWidth: true
                placeholderText: createPrompt.placeholder
                placeholderTextColor: Theme.fg2
                color: Theme.fg0
                font.pixelSize: Theme.fontSize.md
                background: Rectangle {
                    color: Theme.bg0
                    radius: Theme.r2
                    border.color: nameInput.activeFocus ? Theme.accent : Theme.line
                    border.width: 1
                }
                padding: Theme.sp.s3
                Keys.onReturnPressed: createPrompt.submit()
                Keys.onEscapePressed: createPrompt.close()
            }

            // Privacy toggle. Only meaningful for text/voice — categories
            // don't carry overrides themselves. When on, we apply an
            // @everyone DENY VIEW_CHANNEL on the new room so it's hidden
            // from non-admin roles until explicitly allowed.
            RowLayout {
                Layout.fillWidth: true
                visible: createPrompt.kind !== "category"
                spacing: Theme.sp.s3

                Column {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        text: "Private channel"
                        color: Theme.fg0
                        font.pixelSize: Theme.fontSize.md
                    }
                    Text {
                        text: "Only roles that explicitly allow View channel will see it."
                        color: Theme.fg2
                        font.pixelSize: Theme.fontSize.sm
                        wrapMode: Text.WordWrap
                        width: 260
                    }
                }
                ThemedSwitch {
                    checked: createPrompt.makePrivate
                    onToggled: createPrompt.makePrivate = checked
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.sp.s1
                spacing: Theme.sp.s3

                Item { Layout.fillWidth: true }

                Button {
                    id: createCancelBtn
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
                        color: createCancelBtn.hovered ? Theme.bg3 : "transparent"
                        border.color: Theme.line
                        border.width: 1
                        radius: Theme.r2
                        implicitWidth: 100
                        implicitHeight: 36
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                    onClicked: createPrompt.close()
                }
                Button {
                    id: createSubmitBtn
                    enabled: nameInput.text.trim().length > 0
                    contentItem: Text {
                        text: createPrompt.kind === "category"
                              ? "Create category"
                              : (createPrompt.kind === "voice"
                                 ? "Create voice channel"
                                 : "Create channel")
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.semibold
                        color: createSubmitBtn.enabled ? Theme.onAccent : Theme.fg3
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: !createSubmitBtn.enabled
                               ? Theme.bg2
                               : (createSubmitBtn.hovered ? Theme.accentDim : Theme.accent)
                        radius: Theme.r2
                        implicitWidth: 160
                        implicitHeight: 36
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                    onClicked: createPrompt.submit()
                }
            }
        }
    }


    // Per-server settings popup — stays local since only the gear in this
    // component reaches it. UserSettings / ClientSettings live in main.qml
    // so the File menu and this pane share a single instance.
    ServerSettings {
        id: serverSettings
        parent: Overlay.overlay
        // Channels-tab rows emit this when clicked — hop into the per-room
        // ChannelSettings popup declared above.
        onChannelSettingsRequested: function(rid, rname) {
            channelSettingsPopup.roomId = rid;
            channelSettingsPopup.roomName = rname;
            channelSettingsPopup.open();
        }
        // Channels-tab trash icon. Route through the same confirm popup the
        // sidebar's right-click menu uses — single source of truth for
        // delete confirmations.
        onChannelDeleteRequested: function(rid, rname) {
            deleteChannelConfirm.roomId = rid;
            deleteChannelConfirm.roomName = rname;
            deleteChannelConfirm.open();
        }
        // "+ Add category" / per-category "+ text" / "+ voice" — seed and
        // open the shared create-prompt.
        onCreateChannelRequested: function(kind, catId) {
            channelListRoot.openCreatePrompt(kind, catId);
        }
    }

    // New DM popup — asks for a user MXID (e.g. @alice:bsfchat.com)
    // and calls createDirectMessage on the active server. On success
    // the channel list rebuilds and drops the user into the new room.
    Popup {
        id: newDmPrompt
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        width: 400
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: Theme.sp.s7

        background: Rectangle {
            color: Theme.bg1
            radius: Theme.r3
            border.color: Theme.line
            border.width: 1
        }

        function submit() {
            var target = dmTargetField.text.trim();
            if (target.length === 0) return;
            if (!serverManager.activeServer) return;
            serverManager.activeServer.createDirectMessage(target);
            close();
        }

        onOpened: { dmTargetField.text = ""; dmTargetField.forceActiveFocus(); }

        contentItem: ColumnLayout {
            spacing: Theme.sp.s4

            Text {
                text: "New Direct Message"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.lg
                font.weight: Theme.fontWeight.semibold
                color: Theme.fg0
            }

            Text {
                text: "Enter the Matrix ID of a user on this server."
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            TextField {
                id: dmTargetField
                Layout.fillWidth: true
                placeholderText: "@alice:bsfchat.com"
                color: Theme.fg0
                placeholderTextColor: Theme.fg3
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.md
                background: Rectangle {
                    color: Theme.bg0
                    radius: Theme.r2
                    border.color: dmTargetField.activeFocus ? Theme.accent : Theme.line
                    border.width: 1
                }
                leftPadding: Theme.sp.s4
                rightPadding: Theme.sp.s4
                topPadding: Theme.sp.s3
                bottomPadding: Theme.sp.s3
                Keys.onReturnPressed: newDmPrompt.submit()
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.sp.s3
                spacing: Theme.sp.s3
                Item { Layout.fillWidth: true }

                Button {
                    text: "Cancel"
                    onClicked: newDmPrompt.close()
                    contentItem: Text {
                        text: parent.text
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        color: Theme.fg1
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: parent.hovered ? Theme.bg3 : Theme.bg2
                        radius: Theme.r2
                        implicitWidth: 100
                        implicitHeight: 36
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                }

                Button {
                    id: dmSubmitBtn
                    text: "Start DM"
                    enabled: dmTargetField.text.trim().length > 0
                    onClicked: newDmPrompt.submit()
                    contentItem: Text {
                        text: parent.text
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.semibold
                        color: dmSubmitBtn.enabled ? Theme.onAccent : Theme.fg3
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: !dmSubmitBtn.enabled
                            ? Theme.bg2
                            : (dmSubmitBtn.hovered ? Theme.accentDim : Theme.accent)
                        radius: Theme.r2
                        implicitWidth: 120
                        implicitHeight: 36
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                }
            }
        }
    }
}
