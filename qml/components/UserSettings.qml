import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import BSFChat

Popup {
    id: userSettings

    width: 350
    height: 300
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    x: (parent.width - width) / 2
    y: (parent.height - height) / 2

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0.0; to: 1.0; duration: 150; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: 0.95; to: 1.0; duration: 150; easing.type: Easing.OutCubic }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1.0; to: 0.0; duration: 100; easing.type: Easing.InCubic }
        NumberAnimation { property: "scale"; from: 1.0; to: 0.95; duration: 100; easing.type: Easing.InCubic }
    }

    background: Rectangle {
        color: Theme.bgDark
        radius: Theme.radiusNormal
        border.color: Theme.bgLight
        border.width: 1
    }

    onAboutToShow: {
        if (serverManager.activeServer) {
            displayNameField.text = serverManager.activeServer.displayName;
        }
    }

    contentItem: ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacingLarge
        spacing: Theme.spacingLarge

        // Title
        Text {
            text: "User Settings"
            font.pixelSize: 18
            font.bold: true
            color: Theme.textPrimary
            Layout.fillWidth: true
        }

        // Avatar section
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingLarge

            // Avatar circle (clickable)
            Rectangle {
                width: 64
                height: 64
                radius: 32
                color: Theme.senderColor(serverManager.activeServer ? serverManager.activeServer.userId : "")

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
                    font.pixelSize: 24
                    font.bold: true
                    color: "white"
                    visible: !serverManager.activeServer || serverManager.activeServer.avatarUrl === ""
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: avatarFileDialog.open()
                }

                // Hover overlay
                Rectangle {
                    anchors.fill: parent
                    radius: 32
                    color: Qt.rgba(0, 0, 0, 0.4)
                    visible: avatarMouse.containsMouse

                    Text {
                        anchors.centerIn: parent
                        text: "Change"
                        font.pixelSize: Theme.fontSizeSmall
                        font.bold: true
                        color: "white"
                    }

                    MouseArea {
                        id: avatarMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: avatarFileDialog.open()
                    }
                }
            }

            Column {
                Layout.fillWidth: true
                spacing: Theme.spacingSmall

                Text {
                    text: "Click avatar to change"
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.textMuted
                }
            }
        }

        // Display name field
        Column {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall

            Text {
                text: "DISPLAY NAME"
                font.pixelSize: Theme.fontSizeSmall
                font.bold: true
                color: Theme.textSecondary
            }

            RowLayout {
                width: parent.width
                spacing: Theme.spacingNormal

                TextField {
                    id: displayNameField
                    Layout.fillWidth: true
                    placeholderText: "Enter display name"
                    placeholderTextColor: Theme.textMuted
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSizeNormal
                    background: Rectangle {
                        color: Theme.bgDarkest
                        radius: Theme.radiusSmall
                        border.color: displayNameField.activeFocus ? Theme.accent : Theme.bgLight
                        border.width: 1
                    }
                    padding: Theme.spacingNormal

                    Keys.onReturnPressed: saveDisplayName()
                }

                Button {
                    Layout.preferredWidth: 60
                    Layout.preferredHeight: 36
                    contentItem: Text {
                        text: "Save"
                        font.pixelSize: Theme.fontSizeNormal
                        color: "white"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: parent.hovered ? Theme.accentHover : Theme.accent
                        radius: Theme.radiusSmall
                    }
                    onClicked: saveDisplayName()
                }
            }
        }

        // User ID (read-only)
        Column {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall

            Text {
                text: "USER ID"
                font.pixelSize: Theme.fontSizeSmall
                font.bold: true
                color: Theme.textSecondary
            }

            Text {
                text: serverManager.activeServer ? serverManager.activeServer.userId : ""
                font.pixelSize: Theme.fontSizeNormal
                color: Theme.textMuted
                elide: Text.ElideRight
                width: parent.width
            }
        }

        Item { Layout.fillHeight: true }

        // Log Out button
        Button {
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            contentItem: Text {
                text: "Log Out"
                font.pixelSize: Theme.fontSizeNormal
                color: Theme.danger
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: parent.hovered ? Qt.rgba(237/255, 66/255, 69/255, 0.1) : "transparent"
                radius: Theme.radiusSmall
                border.color: Theme.danger
                border.width: 1
            }
            onClicked: {
                if (serverManager.activeServer) {
                    serverManager.activeServer.disconnectFromServer();
                }
                userSettings.close();
            }
        }
    }

    FileDialog {
        id: avatarFileDialog
        title: "Choose Avatar"
        nameFilters: ["Image files (*.png *.jpg *.jpeg *.gif *.webp)"]
        onAccepted: {
            if (!serverManager.activeServer) return;
            // Read the file and upload it
            var filePath = selectedFile;
            uploadAvatar(filePath);
        }
    }

    function saveDisplayName() {
        var name = displayNameField.text.trim();
        if (name.length === 0 || !serverManager.activeServer) return;
        serverManager.activeServer.updateDisplayName(name);
    }

    function uploadAvatar(fileUrl) {
        if (!serverManager.activeServer) return;
        serverManager.activeServer.uploadAvatar(fileUrl.toString());
    }
}
