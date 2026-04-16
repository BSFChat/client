import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import BSFChat

Rectangle {
    id: channelListRoot
    color: Theme.bgDark

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

        // Server name header
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            color: Theme.bgDark

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacingLarge
                anchors.rightMargin: Theme.spacingNormal
                spacing: Theme.spacingSmall

                Text {
                    Layout.fillWidth: true
                    verticalAlignment: Text.AlignVCenter
                    text: serverManager.activeServer ? serverManager.activeServer.serverName : "BSFChat"
                    font.pixelSize: Theme.fontSizeLarge
                    font.bold: true
                    color: Theme.textPrimary
                    elide: Text.ElideRight
                }

                // Settings gear icon — visible whenever the user has any
                // admin-level permission (ADMINISTRATOR short-circuits to all
                // flags, so admins always see it).
                Text {
                    text: "\u2699"
                    font.pixelSize: Theme.fontSizeLarge
                    color: settingsGearMouse.containsMouse ? Theme.textPrimary : Theme.textMuted
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
                color: Theme.bgDarkest
            }
        }

        // Channel category header with "+" button
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            visible: serverManager.activeServer !== null

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacingLarge
                anchors.rightMargin: Theme.spacingNormal

                Text {
                    text: "CHANNELS"
                    font.pixelSize: Theme.fontSizeSmall
                    font.bold: true
                    color: Theme.textMuted
                    Layout.fillWidth: true
                }

                // Create room/category button — opens the same context menu
                // as right-click on empty sidebar, for discoverability.
                Text {
                    id: topPlus
                    text: "+"
                    font.pixelSize: Theme.fontSizeLarge
                    color: topPlusMouse.containsMouse ? Theme.textPrimary : Theme.textMuted
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
                                anchors.leftMargin: Theme.spacingNormal
                                anchors.rightMargin: Theme.spacingNormal

                                // Collapse arrow
                                Text {
                                    text: isCategoryCollapsed(modelData.categoryId) ? "\u25B6" : "\u25BC"
                                    font.pixelSize: 8
                                    color: Theme.textMuted
                                }

                                Text {
                                    text: modelData.categoryName ? modelData.categoryName.toUpperCase() : ""
                                    font.pixelSize: Theme.fontSizeSmall
                                    font.bold: true
                                    color: Theme.textMuted
                                    Layout.fillWidth: true
                                }

                                // "+" to add channel in this category
                                Text {
                                    id: catPlus
                                    text: "+"
                                    font.pixelSize: Theme.fontSizeLarge
                                    color: catPlusMouse.containsMouse ? Theme.textPrimary : Theme.textMuted

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

                                    Rectangle {
                                        id: channelItemBg
                                        anchors.fill: parent
                                        anchors.leftMargin: Theme.spacingNormal
                                        anchors.rightMargin: Theme.spacingNormal
                                        radius: Theme.radiusSmall
                                        color: {
                                            if (modelData.isVoice && serverManager.activeServer
                                                && modelData.roomId === serverManager.activeServer.activeVoiceRoomId)
                                                return Theme.bgLight;
                                            if (!modelData.isVoice && serverManager.activeServer
                                                && modelData.roomId === serverManager.activeServer.activeRoomId)
                                                return Theme.bgLight;
                                            if (channelItemMouse.containsMouse)
                                                return Qt.darker(Theme.bgMedium, 0.9);
                                            return "transparent";
                                        }

                                        // Left green accent bar for active voice channel
                                        Rectangle {
                                            visible: modelData.isVoice && serverManager.activeServer
                                                     && modelData.roomId === serverManager.activeServer.activeVoiceRoomId
                                            width: 3
                                            height: parent.height - 4
                                            anchors.left: parent.left
                                            anchors.verticalCenter: parent.verticalCenter
                                            radius: 2
                                            color: Theme.success
                                        }

                                        Column {
                                            id: channelItemContent
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.verticalCenter: parent.verticalCenter
                                            anchors.leftMargin: Theme.spacingNormal
                                            anchors.rightMargin: Theme.spacingNormal

                                            RowLayout {
                                                width: parent.width
                                                height: 28
                                                spacing: Theme.spacingSmall

                                                Text {
                                                    text: modelData.isVoice ? "\u25CF" : "#"
                                                    font.pixelSize: Theme.fontSizeNormal
                                                    color: modelData.isVoice && serverManager.activeServer
                                                           && modelData.roomId === serverManager.activeServer.activeVoiceRoomId
                                                           ? Theme.success : Theme.textMuted
                                                }

                                                Text {
                                                    text: modelData.displayName
                                                    font.pixelSize: Theme.fontSizeNormal
                                                    font.bold: modelData.unreadCount > 0
                                                    color: {
                                                        if (modelData.isVoice && serverManager.activeServer
                                                            && modelData.roomId === serverManager.activeServer.activeVoiceRoomId)
                                                            return Theme.success;
                                                        if (serverManager.activeServer
                                                            && modelData.roomId === serverManager.activeServer.activeRoomId)
                                                            return Theme.textPrimary;
                                                        if (modelData.unreadCount > 0)
                                                            return Theme.textPrimary;
                                                        return Theme.textSecondary;
                                                    }
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
                                                        color: "white"
                                                    }
                                                }
                                            }

                                            // Voice members list
                                            Column {
                                                visible: modelData.isVoice && modelData.voiceMemberCount > 0
                                                leftPadding: 22
                                                spacing: 1
                                                bottomPadding: 4

                                                Repeater {
                                                    model: {
                                                        if (!serverManager.activeServer) return [];
                                                        if (modelData.roomId !== serverManager.activeServer.activeVoiceRoomId) return [];
                                                        return serverManager.activeServer.voiceMembers;
                                                    }
                                                    delegate: Text {
                                                        text: modelData.user_id || ""
                                                        font.pixelSize: 11
                                                        color: Theme.textMuted
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
                                                } else {
                                                    if (!serverManager.activeServer) return;
                                                    if (modelData.isVoice) {
                                                        serverManager.activeServer.joinVoiceChannel(modelData.roomId);
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
        }

        // Voice panel
        VoicePanel {
            Layout.fillWidth: true
            visible: serverManager.activeServer !== null
                     && serverManager.activeServer.inVoiceChannel
        }

        // Join room input
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 42 : 0
            Layout.leftMargin: Theme.spacingNormal
            Layout.rightMargin: Theme.spacingNormal
            color: "transparent"
            visible: serverManager.activeServer !== null

            TextField {
                id: joinRoomField
                anchors.fill: parent
                anchors.topMargin: Theme.spacingSmall
                anchors.bottomMargin: Theme.spacingSmall
                placeholderText: "Join room by ID..."
                placeholderTextColor: Theme.textMuted
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeSmall
                background: Rectangle {
                    color: Theme.bgDarkest
                    radius: Theme.radiusSmall
                    border.color: joinRoomField.activeFocus ? Theme.accent : Theme.bgLight
                    border.width: 1
                }
                leftPadding: Theme.spacingNormal
                rightPadding: Theme.spacingNormal

                Keys.onReturnPressed: {
                    var id = joinRoomField.text.trim();
                    if (id.length > 0 && serverManager.activeServer) {
                        serverManager.activeServer.joinRoom(id);
                        joinRoomField.text = "";
                    }
                }
            }
        }

        // User info bar at bottom
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 52
            color: userInfoMouse.containsMouse ? Theme.bgLight : Qt.darker(Theme.bgDark, 1.1)

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacingNormal
                anchors.rightMargin: Theme.spacingNormal
                spacing: Theme.spacingNormal

                // Connection status dot
                Rectangle {
                    width: 10
                    height: 10
                    radius: 5
                    color: {
                        if (!serverManager.activeServer) return Theme.textMuted;
                        switch (serverManager.activeServer.connectionStatus) {
                        case 1: return Theme.success;
                        case 2: return Theme.warning;
                        default: return Theme.danger;
                        }
                    }
                }

                // Avatar
                Rectangle {
                    width: 32
                    height: 32
                    radius: 16
                    color: Theme.accent

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
                        text: serverManager.activeServer ? serverManager.activeServer.userId.charAt(1).toUpperCase() : "?"
                        font.pixelSize: 14
                        font.bold: true
                        color: "white"
                        visible: !serverManager.activeServer || serverManager.activeServer.avatarUrl === ""
                    }
                }

                Column {
                    Layout.fillWidth: true

                    Text {
                        text: serverManager.activeServer ? serverManager.activeServer.displayName : ""
                        font.pixelSize: Theme.fontSizeNormal
                        font.bold: true
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                        width: parent.width
                    }

                    Text {
                        text: serverManager.activeServer ? serverManager.activeServer.userId : ""
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.textMuted
                        elide: Text.ElideRight
                        width: parent.width
                    }
                }
            }

            MouseArea {
                id: userInfoMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    // Anchor the menu above the profile block so it opens
                    // upward rather than clipping off-screen.
                    userMenu.popup(0, -userMenu.implicitHeight);
                }
            }

            // Menu for the bottom user-profile block.
            Menu {
                id: userMenu
                background: Rectangle {
                    color: Theme.bgDarkest
                    radius: Theme.radiusSmall
                    border.color: Theme.bgLight
                    border.width: 1
                    implicitWidth: 200
                }
                MenuItem {
                    text: "Edit Server Profile"
                    contentItem: Text {
                        text: parent.text
                        font.pixelSize: Theme.fontSizeNormal
                        color: Theme.textPrimary
                    }
                    background: Rectangle {
                        color: parent.hovered ? Theme.bgLight : "transparent"
                    }
                    onTriggered: Window.window.openUserSettings()
                }
                MenuItem {
                    text: "Client Settings"
                    contentItem: Text {
                        text: parent.text
                        font.pixelSize: Theme.fontSizeNormal
                        color: Theme.textPrimary
                    }
                    background: Rectangle {
                        color: parent.hovered ? Theme.bgLight : "transparent"
                    }
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
        font.pixelSize: Theme.fontSizeNormal
        color: Theme.textMuted
    }

    // Room context menu
    Menu {
        id: roomContextMenu
        property string roomId: ""
        property string roomName: ""

        background: Rectangle {
            color: Theme.bgDarkest
            radius: Theme.radiusSmall
            border.color: Theme.bgLight
            border.width: 1
            implicitWidth: 160
        }

        MenuItem {
            text: "Channel Settings…"
            enabled: {
                if (!serverManager.activeServer) return false;
                if (serverManager.activeServer.permissionsGeneration < 0) return false;
                return serverManager.activeServer.canManageChannel(roomContextMenu.roomId);
            }
            contentItem: Text {
                text: parent.text
                font.pixelSize: Theme.fontSizeNormal
                color: parent.enabled ? Theme.textPrimary : Theme.textMuted
            }
            background: Rectangle {
                color: parent.hovered ? Theme.bgLight : "transparent"
            }
            onTriggered: {
                channelSettingsPopup.roomId = roomContextMenu.roomId;
                channelSettingsPopup.roomName = roomContextMenu.roomName;
                channelSettingsPopup.open();
            }
        }

        MenuItem {
            text: "Delete Channel"
            enabled: {
                if (!serverManager.activeServer) return false;
                if (serverManager.activeServer.permissionsGeneration < 0) return false;
                return serverManager.activeServer.canManageChannel(roomContextMenu.roomId);
            }
            contentItem: Text {
                text: parent.text
                font.pixelSize: Theme.fontSizeNormal
                color: parent.enabled ? Theme.danger : Theme.textMuted
            }
            background: Rectangle {
                color: parent.hovered ? Theme.bgLight : "transparent"
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
        width: 360
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        property string roomId: ""
        property string roomName: ""

        background: Rectangle {
            color: Theme.bgMedium
            radius: Theme.radiusNormal
            border.color: Theme.bgLight
            border.width: 1
        }

        contentItem: ColumnLayout {
            spacing: Theme.spacingNormal

            Text {
                text: "Delete #" + deleteChannelConfirm.roomName + "?"
                font.pixelSize: 16
                font.bold: true
                color: Theme.textPrimary
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Text {
                Layout.fillWidth: true
                text: "This removes the channel and every message in it for everyone. Cannot be undone."
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textMuted
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spacingSmall
                spacing: Theme.spacingNormal

                Item { Layout.fillWidth: true }

                Button {
                    text: "Cancel"
                    contentItem: Text {
                        text: parent.text
                        font.pixelSize: Theme.fontSizeNormal
                        color: Theme.textSecondary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: parent.hovered ? Theme.bgLight : Theme.bgDark
                        radius: Theme.radiusSmall
                        implicitWidth: 90
                        implicitHeight: Theme.buttonHeight
                    }
                    onClicked: deleteChannelConfirm.close()
                }
                Button {
                    text: "Delete"
                    contentItem: Text {
                        text: parent.text
                        font.pixelSize: Theme.fontSizeNormal
                        color: "white"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: parent.hovered ? Qt.lighter(Theme.danger, 1.1) : Theme.danger
                        radius: Theme.radiusSmall
                        implicitWidth: 90
                        implicitHeight: Theme.buttonHeight
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
    Menu {
        id: createMenu
        property string categoryId: ""

        background: Rectangle {
            color: Theme.bgDarkest
            radius: Theme.radiusSmall
            border.color: Theme.bgLight
            border.width: 1
            implicitWidth: 200
        }

        MenuItem {
            text: "Create Category"
            contentItem: Text {
                text: parent.text
                font.pixelSize: Theme.fontSizeNormal
                color: Theme.textPrimary
            }
            background: Rectangle {
                color: parent.hovered ? Theme.bgLight : "transparent"
            }
            onTriggered: channelListRoot.openCreatePrompt("category", "")
        }
        MenuItem {
            text: "Create Text Channel"
            contentItem: Text {
                text: parent.text
                font.pixelSize: Theme.fontSizeNormal
                color: Theme.textPrimary
            }
            background: Rectangle {
                color: parent.hovered ? Theme.bgLight : "transparent"
            }
            onTriggered: channelListRoot.openCreatePrompt("text", createMenu.categoryId)
        }
        MenuItem {
            text: "Create Voice Channel"
            contentItem: Text {
                text: parent.text
                font.pixelSize: Theme.fontSizeNormal
                color: Theme.textPrimary
            }
            background: Rectangle {
                color: parent.hovered ? Theme.bgLight : "transparent"
            }
            onTriggered: channelListRoot.openCreatePrompt("voice", createMenu.categoryId)
        }
    }

    // Minimal "just give it a name" prompt used for all three create flows.
    // The type was already chosen in the context menu so we don't re-ask here.
    Popup {
        id: createPrompt
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        width: 340
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

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

        background: Rectangle {
            color: Theme.bgMedium
            radius: Theme.radiusNormal
            border.color: Theme.bgLight
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
            spacing: Theme.spacingNormal

            Text {
                text: createPrompt.promptTitle
                font.pixelSize: 16
                font.bold: true
                color: Theme.textPrimary
            }

            TextField {
                id: nameInput
                Layout.fillWidth: true
                placeholderText: createPrompt.placeholder
                placeholderTextColor: Theme.textMuted
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeNormal
                background: Rectangle {
                    color: Theme.bgDarkest
                    radius: Theme.radiusSmall
                    border.color: nameInput.activeFocus ? Theme.accent : Theme.bgLight
                    border.width: 1
                }
                padding: Theme.spacingNormal
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
                spacing: Theme.spacingNormal

                Column {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        text: "Private channel"
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSizeNormal
                    }
                    Text {
                        text: "Only roles that explicitly allow View channel will see it."
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WordWrap
                        width: 260
                    }
                }
                Switch {
                    checked: createPrompt.makePrivate
                    onToggled: createPrompt.makePrivate = checked
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spacingSmall
                spacing: Theme.spacingNormal

                Item { Layout.fillWidth: true }

                Button {
                    text: "Cancel"
                    contentItem: Text {
                        text: parent.text
                        font.pixelSize: Theme.fontSizeNormal
                        color: Theme.textSecondary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: parent.hovered ? Theme.bgLight : Theme.bgDark
                        radius: Theme.radiusSmall
                        implicitWidth: 90
                        implicitHeight: Theme.buttonHeight
                    }
                    onClicked: createPrompt.close()
                }
                Button {
                    text: "Create"
                    enabled: nameInput.text.trim().length > 0
                    contentItem: Text {
                        text: parent.text
                        font.pixelSize: Theme.fontSizeNormal
                        color: "white"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        opacity: parent.enabled ? 1.0 : 0.5
                    }
                    background: Rectangle {
                        color: parent.enabled
                            ? (parent.hovered ? Theme.accentHover : Theme.accent)
                            : Theme.bgLight
                        radius: Theme.radiusSmall
                        implicitWidth: 90
                        implicitHeight: Theme.buttonHeight
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
    }
}
