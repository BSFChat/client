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

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

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

                    delegate: Column {
                        width: channelColumn.width

                        // Category header (skip for uncategorized)
                        Item {
                            width: parent.width
                            height: modelData.categoryId !== "" ? 32 : 0
                            visible: modelData.categoryId !== ""

                            // Backdrop click handler — left-click toggles
                            // collapse/expand; right-click opens the create
                            // menu scoped to this category so the new channel
                            // lands inside it. Declared before RowLayout so
                            // child MouseAreas (notably "+") still win on
                            // their own hit tests.
                            MouseArea {
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
                                anchors.leftMargin: Theme.sp.s3
                                anchors.rightMargin: Theme.sp.s3

                                // Collapse chevron — rotates rather than
                                // swapping glyphs so the transition is
                                // continuous. Angle 0 = pointing right
                                // (collapsed); 90 = pointing down (open).
                                Icon {
                                    name: "chevron-right"
                                    size: 12
                                    color: Theme.fg3
                                    rotation: isCategoryCollapsed(modelData.categoryId) ? 0 : 90
                                    Behavior on rotation {
                                        NumberAnimation { duration: Theme.motion.fastMs
                                                          easing.type: Easing.BezierSpline
                                                          easing.bezierCurve: Theme.motion.bezier }
                                    }
                                }

                                Text {
                                    text: modelData.categoryName ? modelData.categoryName.toUpperCase() : ""
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.fontSize.xs
                                    font.weight: Theme.fontWeight.semibold
                                    font.letterSpacing: Theme.trackWidest.xs
                                    color: Theme.fg3
                                    Layout.fillWidth: true
                                }

                                // "+" to add channel in this category
                                Text {
                                    id: catPlus
                                    text: "+"
                                    font.pixelSize: Theme.fontSize.xl
                                    color: catPlusMouse.containsMouse ? Theme.fg0 : Theme.fg2

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
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                z: -1
                                onClicked: toggleCategoryCollapsed(modelData.categoryId)
                            }
                        }

                        // Channels in this category (hidden when collapsed)
                        Column {
                            width: parent.width
                            visible: !isCategoryCollapsed(modelData.categoryId)

                            Repeater {
                                model: modelData.channels

                                delegate: Item {
                                    width: parent.width
                                    height: channelItemContent.implicitHeight + 4

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
                                        radius: Theme.r1
                                        color: {
                                            if (parent.isActive) return Theme.bg3;
                                            if (channelItemMouse.containsMouse) return Theme.bg2;
                                            return "transparent";
                                        }

                                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

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
                                                height: 28
                                                spacing: Theme.sp.s3

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

                                                // Unread count badge (text channels only)
                                                Rectangle {
                                                    visible: !modelData.isVoice && modelData.unreadCount > 0
                                                    Layout.preferredWidth: Math.max(20, unreadBadgeText.implicitWidth + 8)
                                                    Layout.preferredHeight: 18
                                                    radius: 9
                                                    color: Theme.danger

                                                    Text {
                                                        id: unreadBadgeText
                                                        anchors.centerIn: parent
                                                        text: modelData.unreadCount > 99 ? "99+" : modelData.unreadCount
                                                        font.pixelSize: 11
                                                        font.bold: true
                                                        color: Theme.onAccent
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
                                                        required property var modelData
                                                        width: parent.width
                                                        height: 22

                                                        Row {
                                                            anchors.verticalCenter: parent.verticalCenter
                                                            spacing: Theme.sp.s3

                                                            // 16×16 circular avatar.
                                                            Rectangle {
                                                                width: 16; height: 16
                                                                radius: 8
                                                                color: Theme.senderColor(
                                                                    parent.parent.modelData.user_id || "")
                                                                Text {
                                                                    anchors.centerIn: parent
                                                                    text: {
                                                                        var m = parent.parent.parent.modelData;
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

                                                            Text {
                                                                anchors.verticalCenter: parent.verticalCenter
                                                                text: parent.parent.modelData.displayName
                                                                      || parent.parent.modelData.user_id
                                                                      || ""
                                                                font.family: Theme.fontSans
                                                                font.pixelSize: Theme.fontSize.sm
                                                                color: Theme.fg1
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        MouseArea {
                                            id: channelItemMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                                            onClicked: (mouse) => {
                                                if (mouse.button === Qt.RightButton) {
                                                    roomContextMenu.roomId = modelData.roomId;
                                                    roomContextMenu.roomName = modelData.displayName;
                                                    roomContextMenu.popup();
                                                    return;
                                                }
                                                if (!serverManager.activeServer) return;
                                                if (modelData.isVoice) {
                                                    // If we're already in this voice channel,
                                                    // just flip the view (no rejoin). Otherwise
                                                    // joinVoiceChannel leaves+rejoins AND sets
                                                    // the view to VoiceRoom.
                                                    if (serverManager.activeServer.activeVoiceRoomId === modelData.roomId) {
                                                        serverManager.activeServer.showVoiceRoom();
                                                    } else {
                                                        serverManager.activeServer.joinVoiceChannel(modelData.roomId);
                                                    }
                                                } else {
                                                    serverManager.activeServer.setActiveRoom(modelData.roomId);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
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
                // state shown in the VoiceDock and vice-versa.
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
                    implicitHeight: 34
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
            }
        }
    }

    // Empty state
    Text {
        anchors.centerIn: parent
        visible: serverManager.activeServer === null
        text: "Add a server to get started"
        font.pixelSize: Theme.fontSize.md
        color: Theme.fg2
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
            implicitHeight: 34
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
            implicitHeight: 34
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
}
