import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

Rectangle {
    color: Theme.bgDark

    // Category collapse state
    property var collapsedCategories: ({})

    function isCategoryCollapsed(catId) {
        return collapsedCategories[catId] === true;
    }

    function toggleCategoryCollapsed(catId) {
        var c = collapsedCategories;
        c[catId] = !c[catId];
        collapsedCategories = c;
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
                    text: serverManager.activeServer ? serverManager.activeServer.displayName : "BSFChat"
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
                        // Depend on serverRoles so that when
                        // applyServerRolesEvent / applyMemberRolesEvent fire
                        // serverRolesChanged, this binding re-evaluates.
                        sc.serverRoles;
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

                // Create room/category button
                Text {
                    text: "+"
                    font.pixelSize: Theme.fontSizeLarge
                    color: Theme.textMuted
                    Layout.alignment: Qt.AlignVCenter

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            createRoomDialog.selectedCategoryId = "";
                            createRoomDialog.open();
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
                                    text: "+"
                                    font.pixelSize: Theme.fontSizeLarge
                                    color: catPlusMouse.containsMouse ? Theme.textPrimary : Theme.textMuted

                                    MouseArea {
                                        id: catPlusMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            createRoomDialog.selectedCategoryId = modelData.categoryId;
                                            createRoomDialog.open();
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
                onClicked: userSettingsPopup.open()
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
                serverManager.activeServer.serverRoles; // dep touch
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
            text: "Leave Room"
            contentItem: Text {
                text: parent.text
                font.pixelSize: Theme.fontSizeNormal
                color: Theme.danger
            }
            background: Rectangle {
                color: parent.hovered ? Theme.bgLight : "transparent"
            }
            onTriggered: {
                if (serverManager.activeServer && roomContextMenu.roomId !== "") {
                    serverManager.activeServer.leaveRoom(roomContextMenu.roomId);
                }
            }
        }
    }

    ChannelSettings {
        id: channelSettingsPopup
    }

    // Create room dialog
    Dialog {
        id: createRoomDialog
        title: "Create Channel"
        anchors.centerIn: Overlay.overlay
        width: 360
        modal: true
        standardButtons: Dialog.NoButton

        property string selectedCategoryId: ""

        background: Rectangle {
            color: Theme.bgMedium
            radius: Theme.radiusNormal
            border.color: Theme.bgLight
            border.width: 1
        }

        header: Rectangle {
            color: "transparent"
            height: 50
            Text {
                anchors.centerIn: parent
                text: "Create Channel"
                font.pixelSize: 18
                font.bold: true
                color: Theme.textPrimary
            }
        }

        contentItem: Column {
            spacing: Theme.spacingNormal
            padding: Theme.spacingLarge

            Text {
                text: "CHANNEL NAME"
                font.pixelSize: Theme.fontSizeSmall
                font.bold: true
                color: Theme.textSecondary
            }

            TextField {
                id: roomNameField
                width: parent.width - parent.padding * 2
                placeholderText: "general"
                placeholderTextColor: Theme.textMuted
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeNormal
                background: Rectangle {
                    color: Theme.bgDarkest
                    radius: Theme.radiusSmall
                    border.color: roomNameField.activeFocus ? Theme.accent : Theme.bgLight
                    border.width: 1
                }
                padding: Theme.spacingNormal
                Keys.onReturnPressed: createRoomDialog.doCreateRoom()
            }

            // Voice channel toggle
            Row {
                spacing: Theme.spacingNormal
                width: parent.width - parent.padding * 2
                visible: !categoryCheck.checked

                CheckBox {
                    id: voiceChannelCheck
                    checked: false
                    indicator: Rectangle {
                        implicitWidth: 20
                        implicitHeight: 20
                        y: parent.height / 2 - height / 2
                        radius: Theme.radiusSmall
                        color: voiceChannelCheck.checked ? Theme.accent : Theme.bgDarkest
                        border.color: voiceChannelCheck.checked ? Theme.accent : Theme.bgLight
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "\u2713"
                            font.pixelSize: 14
                            font.bold: true
                            color: "white"
                            visible: voiceChannelCheck.checked
                        }
                    }
                }

                Text {
                    text: "Voice Channel"
                    font.pixelSize: Theme.fontSizeNormal
                    color: Theme.textSecondary
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            // Create as category toggle
            Row {
                spacing: Theme.spacingNormal
                width: parent.width - parent.padding * 2

                CheckBox {
                    id: categoryCheck
                    checked: false
                    indicator: Rectangle {
                        implicitWidth: 20
                        implicitHeight: 20
                        y: parent.height / 2 - height / 2
                        radius: Theme.radiusSmall
                        color: categoryCheck.checked ? Theme.accent : Theme.bgDarkest
                        border.color: categoryCheck.checked ? Theme.accent : Theme.bgLight
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "\u2713"
                            font.pixelSize: 14
                            font.bold: true
                            color: "white"
                            visible: categoryCheck.checked
                        }
                    }
                }

                Text {
                    text: "Create as Category"
                    font.pixelSize: Theme.fontSizeNormal
                    color: Theme.textSecondary
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            // Category picker (for channels, not categories)
            Text {
                text: "CATEGORY"
                font.pixelSize: Theme.fontSizeSmall
                font.bold: true
                color: Theme.textSecondary
                visible: !categoryCheck.checked
            }

            ComboBox {
                id: categoryPicker
                width: parent.width - parent.padding * 2
                visible: !categoryCheck.checked
                model: {
                    if (!serverManager.activeServer) return ["None"];
                    var cats = serverManager.activeServer.categorizedRooms;
                    var items = ["None"];
                    for (var i = 0; i < cats.length; i++) {
                        if (cats[i].categoryId !== "") {
                            items.push(cats[i].categoryName);
                        }
                    }
                    return items;
                }
                currentIndex: {
                    if (createRoomDialog.selectedCategoryId === "") return 0;
                    if (!serverManager.activeServer) return 0;
                    var cats = serverManager.activeServer.categorizedRooms;
                    for (var i = 0; i < cats.length; i++) {
                        if (cats[i].categoryId === createRoomDialog.selectedCategoryId) {
                            return i + 1;
                        }
                    }
                    return 0;
                }

                background: Rectangle {
                    color: Theme.bgDarkest
                    radius: Theme.radiusSmall
                    border.color: Theme.bgLight
                    border.width: 1
                }
                contentItem: Text {
                    leftPadding: Theme.spacingNormal
                    text: categoryPicker.displayText
                    font.pixelSize: Theme.fontSizeNormal
                    color: Theme.textPrimary
                    verticalAlignment: Text.AlignVCenter
                }
                popup: Popup {
                    y: categoryPicker.height
                    width: categoryPicker.width
                    padding: 1
                    contentItem: ListView {
                        clip: true
                        implicitHeight: contentHeight
                        model: categoryPicker.delegateModel
                    }
                    background: Rectangle {
                        color: Theme.bgDarkest
                        radius: Theme.radiusSmall
                        border.color: Theme.bgLight
                    }
                }
                delegate: ItemDelegate {
                    width: categoryPicker.width
                    contentItem: Text {
                        text: modelData
                        font.pixelSize: Theme.fontSizeNormal
                        color: Theme.textPrimary
                    }
                    background: Rectangle {
                        color: parent.hovered ? Theme.bgLight : "transparent"
                    }
                }
            }

            Text {
                text: "TOPIC (optional)"
                font.pixelSize: Theme.fontSizeSmall
                font.bold: true
                color: Theme.textSecondary
                visible: !voiceChannelCheck.checked && !categoryCheck.checked
            }

            TextField {
                id: roomTopicField
                visible: !voiceChannelCheck.checked && !categoryCheck.checked
                width: parent.width - parent.padding * 2
                placeholderText: "What's this channel about?"
                placeholderTextColor: Theme.textMuted
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeNormal
                background: Rectangle {
                    color: Theme.bgDarkest
                    radius: Theme.radiusSmall
                    border.color: roomTopicField.activeFocus ? Theme.accent : Theme.bgLight
                    border.width: 1
                }
                padding: Theme.spacingNormal
                Keys.onReturnPressed: createRoomDialog.doCreateRoom()
            }

            Row {
                spacing: Theme.spacingNormal
                width: parent.width - parent.padding * 2

                Button {
                    text: "Cancel"
                    width: (parent.width - parent.spacing) / 2
                    contentItem: Text {
                        text: parent.text
                        font.pixelSize: Theme.fontSizeNormal
                        color: Theme.textSecondary
                        horizontalAlignment: Text.AlignHCenter
                    }
                    background: Rectangle {
                        color: parent.hovered ? Theme.bgLight : Theme.bgDark
                        radius: Theme.radiusSmall
                    }
                    onClicked: createRoomDialog.close()
                }

                Button {
                    text: "Create"
                    width: (parent.width - parent.spacing) / 2
                    contentItem: Text {
                        text: parent.text
                        font.pixelSize: Theme.fontSizeNormal
                        color: "white"
                        horizontalAlignment: Text.AlignHCenter
                    }
                    background: Rectangle {
                        color: parent.hovered ? Theme.accentHover : Theme.accent
                        radius: Theme.radiusSmall
                    }
                    onClicked: createRoomDialog.doCreateRoom()
                }
            }
        }

        function doCreateRoom() {
            var name = roomNameField.text.trim();
            if (name.length === 0) return;
            if (!serverManager.activeServer) return;

            if (categoryCheck.checked) {
                serverManager.activeServer.createCategory(name);
            } else {
                // Determine selected category ID
                var catId = "";
                if (categoryPicker.currentIndex > 0) {
                    var cats = serverManager.activeServer.categorizedRooms;
                    var catIdx = 0;
                    for (var i = 0; i < cats.length; i++) {
                        if (cats[i].categoryId !== "") {
                            catIdx++;
                            if (catIdx === categoryPicker.currentIndex) {
                                catId = cats[i].categoryId;
                                break;
                            }
                        }
                    }
                }
                if (catId === "" && selectedCategoryId !== "") {
                    catId = selectedCategoryId;
                }

                serverManager.activeServer.createChannelInCategory(name, catId, voiceChannelCheck.checked);
            }

            roomNameField.text = "";
            roomTopicField.text = "";
            voiceChannelCheck.checked = false;
            categoryCheck.checked = false;
            selectedCategoryId = "";
            createRoomDialog.close();
        }
    }

    // User settings popup
    UserSettings {
        id: userSettingsPopup
        parent: Overlay.overlay
    }

    // Server settings popup
    ServerSettings {
        id: serverSettings
        parent: Overlay.overlay
    }
}
