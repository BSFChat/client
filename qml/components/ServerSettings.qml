import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

Popup {
    id: serverSettingsPopup
    anchors.centerIn: Overlay.overlay
    width: parent ? parent.width * 0.85 : 800
    height: parent ? parent.height * 0.85 : 600
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    property int selectedSection: 0

    background: Rectangle {
        color: Theme.bgDark
        radius: Theme.radiusNormal
        border.color: Theme.bgLight
        border.width: 1
    }

    contentItem: RowLayout {
        spacing: 0

        // Left nav sidebar
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 200
            color: Theme.bgDarkest
            radius: Theme.radiusNormal

            // Clip right radius
            Rectangle {
                anchors.right: parent.right
                width: Theme.radiusNormal
                height: parent.height
                color: Theme.bgDarkest
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.spacingNormal
                spacing: 2

                Text {
                    text: "SERVER SETTINGS"
                    font.pixelSize: Theme.fontSizeSmall
                    font.bold: true
                    color: Theme.textMuted
                    Layout.leftMargin: Theme.spacingNormal
                    Layout.topMargin: Theme.spacingNormal
                    Layout.bottomMargin: Theme.spacingNormal
                }

                Repeater {
                    model: ["Overview", "Roles", "Members", "Channels", "Bans"]
                    delegate: Rectangle {
                        Layout.fillWidth: true
                        height: 36
                        radius: Theme.radiusSmall
                        color: selectedSection === index ? Theme.bgLight : navItemMouse.containsMouse ? Qt.darker(Theme.bgMedium, 0.9) : "transparent"

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: Theme.spacingNormal
                            text: modelData
                            color: selectedSection === index ? Theme.textPrimary : Theme.textSecondary
                            font.pixelSize: Theme.fontSizeNormal
                        }

                        MouseArea {
                            id: navItemMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: selectedSection = index
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                // Close button at bottom
                Rectangle {
                    Layout.fillWidth: true
                    height: 36
                    radius: Theme.radiusSmall
                    color: closeNavMouse.containsMouse ? Theme.bgLight : "transparent"

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.spacingNormal
                        text: "Close"
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSizeNormal
                    }

                    MouseArea {
                        id: closeNavMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: serverSettingsPopup.close()
                    }
                }
            }
        }

        // Content area
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: selectedSection

            // ---- Overview (index 0) ----
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingLarge * 2
                    spacing: Theme.spacingLarge

                    Text {
                        text: "Server Overview"
                        font.pixelSize: 22
                        font.bold: true
                        color: Theme.textPrimary
                    }

                    Text {
                        text: "SERVER NAME"
                        font.pixelSize: Theme.fontSizeSmall
                        font.bold: true
                        color: Theme.textSecondary
                    }

                    TextField {
                        id: serverNameField
                        Layout.fillWidth: true
                        Layout.maximumWidth: 400
                        text: serverManager.activeServer ? serverManager.activeServer.displayName : ""
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSizeNormal
                        background: Rectangle {
                            color: Theme.bgDarkest
                            radius: Theme.radiusSmall
                            border.color: serverNameField.activeFocus ? Theme.accent : Theme.bgLight
                            border.width: 1
                        }
                        padding: Theme.spacingNormal
                    }

                    Button {
                        text: "Save"
                        contentItem: Text {
                            text: parent.text
                            font.pixelSize: Theme.fontSizeNormal
                            color: "white"
                            horizontalAlignment: Text.AlignHCenter
                        }
                        background: Rectangle {
                            color: parent.hovered ? Theme.accentHover : Theme.accent
                            radius: Theme.radiusSmall
                            implicitWidth: 100
                            implicitHeight: Theme.buttonHeight
                        }
                        onClicked: {
                            if (serverManager.activeServer) {
                                serverManager.activeServer.updateDisplayName(serverNameField.text.trim());
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            // ---- Roles (index 1) ----
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingLarge * 2
                    spacing: Theme.spacingLarge

                    Text {
                        text: "Roles"
                        font.pixelSize: 22
                        font.bold: true
                        color: Theme.textPrimary
                    }

                    // Roles list
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: serverManager.activeServer ? serverManager.activeServer.serverRoles : []
                        spacing: 4

                        delegate: Rectangle {
                            width: ListView.view ? ListView.view.width : 300
                            height: 44
                            radius: Theme.radiusSmall
                            color: Theme.bgMedium

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.spacingNormal
                                anchors.rightMargin: Theme.spacingNormal
                                spacing: Theme.spacingNormal

                                // Color circle
                                Rectangle {
                                    width: 14
                                    height: 14
                                    radius: 7
                                    color: modelData.color || Theme.accent
                                }

                                Text {
                                    text: modelData.name || ""
                                    font.pixelSize: Theme.fontSizeNormal
                                    color: Theme.textPrimary
                                    Layout.fillWidth: true
                                }

                                Text {
                                    text: "Level: " + (modelData.level !== undefined ? modelData.level : 0)
                                    font.pixelSize: Theme.fontSizeSmall
                                    color: Theme.textMuted
                                }
                            }
                        }

                        // Empty state
                        Text {
                            anchors.centerIn: parent
                            visible: parent.count === 0
                            text: "No roles configured"
                            font.pixelSize: Theme.fontSizeNormal
                            color: Theme.textMuted
                        }
                    }

                    // Add role form
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: addRoleExpanded ? 160 : 36
                        radius: Theme.radiusSmall
                        color: Theme.bgMedium
                        clip: true

                        property bool addRoleExpanded: false

                        Behavior on Layout.preferredHeight {
                            NumberAnimation { duration: 150 }
                        }

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: Theme.spacingNormal
                            spacing: Theme.spacingNormal

                            // Toggle bar
                            Text {
                                text: parent.parent.addRoleExpanded ? "- Cancel" : "+ Add Role"
                                font.pixelSize: Theme.fontSizeNormal
                                color: Theme.accent

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: parent.parent.parent.addRoleExpanded = !parent.parent.parent.addRoleExpanded
                                }
                            }

                            // Form fields
                            RowLayout {
                                visible: parent.parent.addRoleExpanded
                                Layout.fillWidth: true
                                spacing: Theme.spacingNormal

                                TextField {
                                    id: newRoleNameField
                                    Layout.fillWidth: true
                                    placeholderText: "Role name"
                                    placeholderTextColor: Theme.textMuted
                                    color: Theme.textPrimary
                                    font.pixelSize: Theme.fontSizeNormal
                                    background: Rectangle {
                                        color: Theme.bgDarkest
                                        radius: Theme.radiusSmall
                                        border.color: Theme.bgLight
                                        border.width: 1
                                    }
                                    padding: Theme.spacingNormal
                                }

                                SpinBox {
                                    id: newRoleLevelSpin
                                    from: 0
                                    to: 100
                                    value: 0

                                    background: Rectangle {
                                        color: Theme.bgDarkest
                                        radius: Theme.radiusSmall
                                        border.color: Theme.bgLight
                                        border.width: 1
                                        implicitWidth: 100
                                    }
                                    contentItem: TextInput {
                                        text: newRoleLevelSpin.textFromValue(newRoleLevelSpin.value, newRoleLevelSpin.locale)
                                        font.pixelSize: Theme.fontSizeNormal
                                        color: Theme.textPrimary
                                        horizontalAlignment: Qt.AlignHCenter
                                        verticalAlignment: Qt.AlignVCenter
                                        readOnly: !newRoleLevelSpin.editable
                                        validator: newRoleLevelSpin.validator
                                    }
                                }
                            }

                            // Color presets
                            Row {
                                visible: parent.parent.addRoleExpanded
                                spacing: Theme.spacingSmall

                                property string selectedColor: "#5865f2"

                                Repeater {
                                    model: ["#5865f2", "#57f287", "#fee75c", "#ed4245", "#f47067", "#e0823d",
                                            "#39c5cf", "#dcbdfb", "#768390", "#f69d50"]
                                    delegate: Rectangle {
                                        width: 24
                                        height: 24
                                        radius: 12
                                        color: modelData
                                        border.color: parent.parent.selectedColor === modelData ? Theme.textPrimary : "transparent"
                                        border.width: 2

                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: parent.parent.parent.selectedColor = modelData
                                        }
                                    }
                                }
                            }

                            Button {
                                visible: parent.parent.addRoleExpanded
                                text: "Save Role"
                                contentItem: Text {
                                    text: parent.text
                                    font.pixelSize: Theme.fontSizeNormal
                                    color: "white"
                                    horizontalAlignment: Text.AlignHCenter
                                }
                                background: Rectangle {
                                    color: parent.hovered ? Theme.accentHover : Theme.accent
                                    radius: Theme.radiusSmall
                                    implicitWidth: 100
                                    implicitHeight: Theme.buttonHeight
                                }
                                onClicked: {
                                    if (!serverManager.activeServer) return;
                                    var name = newRoleNameField.text.trim();
                                    if (name.length === 0) return;

                                    // Build updated roles array
                                    var roles = [];
                                    var existing = serverManager.activeServer.serverRoles;
                                    for (var i = 0; i < existing.length; i++) {
                                        roles.push(existing[i]);
                                    }
                                    // Find the color row's selectedColor
                                    var colorRow = parent.children[2]; // color presets Row
                                    roles.push({
                                        "name": name,
                                        "level": newRoleLevelSpin.value,
                                        "color": colorRow.selectedColor || "#5865f2"
                                    });

                                    serverManager.activeServer.updateServerRoles(roles);
                                    newRoleNameField.text = "";
                                    newRoleLevelSpin.value = 0;
                                    parent.parent.addRoleExpanded = false;
                                }
                            }
                        }
                    }
                }
            }

            // ---- Members (index 2) ----
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingLarge * 2
                    spacing: Theme.spacingLarge

                    Text {
                        text: "Members"
                        font.pixelSize: 22
                        font.bold: true
                        color: Theme.textPrimary
                    }

                    // Search field
                    TextField {
                        id: memberSearchField
                        Layout.fillWidth: true
                        Layout.maximumWidth: 400
                        placeholderText: "Search members..."
                        placeholderTextColor: Theme.textMuted
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSizeNormal
                        background: Rectangle {
                            color: Theme.bgDarkest
                            radius: Theme.radiusSmall
                            border.color: memberSearchField.activeFocus ? Theme.accent : Theme.bgLight
                            border.width: 1
                        }
                        padding: Theme.spacingNormal
                    }

                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: serverManager.activeServer ? serverManager.activeServer.memberListModel : null
                        spacing: 2

                        delegate: Rectangle {
                            width: ListView.view ? ListView.view.width : 400
                            height: 48
                            radius: Theme.radiusSmall
                            color: memberItemMouse.containsMouse ? Theme.bgLight : Theme.bgMedium
                            visible: {
                                var search = memberSearchField.text.toLowerCase();
                                if (search.length === 0) return true;
                                var dn = model.displayName ? model.displayName.toLowerCase() : "";
                                var uid = model.userId ? model.userId.toLowerCase() : "";
                                return dn.indexOf(search) >= 0 || uid.indexOf(search) >= 0;
                            }
                            clip: true

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.spacingNormal
                                anchors.rightMargin: Theme.spacingNormal
                                spacing: Theme.spacingNormal

                                // Avatar placeholder
                                Rectangle {
                                    width: 32
                                    height: 32
                                    radius: 16
                                    color: Theme.accent

                                    Text {
                                        anchors.centerIn: parent
                                        text: model.displayName ? model.displayName.charAt(0).toUpperCase() : "?"
                                        font.pixelSize: 14
                                        font.bold: true
                                        color: "white"
                                    }
                                }

                                Column {
                                    Layout.fillWidth: true

                                    Text {
                                        text: model.displayName || ""
                                        font.pixelSize: Theme.fontSizeNormal
                                        color: Theme.textPrimary
                                    }

                                    Text {
                                        text: model.userId || ""
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.textMuted
                                    }
                                }
                            }

                            MouseArea {
                                id: memberItemMouse
                                anchors.fill: parent
                                hoverEnabled: true
                            }
                        }
                    }
                }
            }

            // ---- Channels (index 3) ----
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingLarge * 2
                    spacing: Theme.spacingLarge

                    Text {
                        text: "Channels"
                        font.pixelSize: 22
                        font.bold: true
                        color: Theme.textPrimary
                    }

                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: serverManager.activeServer ? serverManager.activeServer.categorizedRooms : []
                        spacing: 4

                        delegate: Column {
                            width: ListView.view ? ListView.view.width : 400

                            // Category header
                            Rectangle {
                                width: parent.width
                                height: 36
                                radius: Theme.radiusSmall
                                color: Theme.bgMedium
                                visible: modelData.categoryId !== ""

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: Theme.spacingNormal
                                    anchors.rightMargin: Theme.spacingNormal

                                    Text {
                                        text: modelData.categoryName || ""
                                        font.pixelSize: Theme.fontSizeNormal
                                        font.bold: true
                                        color: Theme.textPrimary
                                        Layout.fillWidth: true
                                    }

                                    Text {
                                        text: modelData.channels ? modelData.channels.length + " channels" : "0 channels"
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.textMuted
                                    }
                                }
                            }

                            // Channels in category
                            Repeater {
                                model: modelData.channels

                                delegate: Rectangle {
                                    width: parent.width
                                    height: 32
                                    color: chSettingsMouse.containsMouse ? Theme.bgLight : "transparent"

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: Theme.spacingLarge + Theme.spacingNormal
                                        anchors.rightMargin: Theme.spacingNormal
                                        spacing: Theme.spacingSmall

                                        Text {
                                            text: modelData.isVoice ? "\u25CF" : "#"
                                            font.pixelSize: Theme.fontSizeNormal
                                            color: Theme.textMuted
                                        }

                                        Text {
                                            text: modelData.displayName || ""
                                            font.pixelSize: Theme.fontSizeNormal
                                            color: Theme.textSecondary
                                            Layout.fillWidth: true
                                        }

                                        Text {
                                            text: modelData.roomType || "text"
                                            font.pixelSize: Theme.fontSizeSmall
                                            color: Theme.textMuted
                                        }
                                    }

                                    MouseArea {
                                        id: chSettingsMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ---- Bans (index 4) ----
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingLarge * 2
                    spacing: Theme.spacingLarge

                    Text {
                        text: "Bans"
                        font.pixelSize: 22
                        font.bold: true
                        color: Theme.textPrimary
                    }

                    Text {
                        text: "Banned users will appear here."
                        font.pixelSize: Theme.fontSizeNormal
                        color: Theme.textMuted
                        Layout.fillWidth: true
                    }

                    Item { Layout.fillHeight: true }
                }
            }
        }
    }

    // Close button (top-right X)
    Text {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: Theme.spacingNormal
        anchors.rightMargin: Theme.spacingNormal
        text: "\u2715"
        font.pixelSize: Theme.fontSizeLarge
        color: closeXMouse.containsMouse ? Theme.textPrimary : Theme.textMuted
        z: 10

        MouseArea {
            id: closeXMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: serverSettingsPopup.close()
        }
    }

    onOpened: selectedSection = 0
}
