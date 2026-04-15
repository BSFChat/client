import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

Popup {
    id: profileCard

    property string userId: ""
    property string profileDisplayName: ""
    property string profileAvatarUrl: ""
    property string serverName: ""

    width: 300
    height: 250
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
        if (userId !== "" && serverManager.activeServer) {
            serverManager.activeServer.fetchProfile(userId);
        }
    }

    Connections {
        target: serverManager.activeServer
        function onProfileFetched(uid, displayName, avatarUrl) {
            if (uid === profileCard.userId) {
                profileCard.profileDisplayName = displayName || uid;
                profileCard.profileAvatarUrl = avatarUrl || "";
            }
        }
    }

    contentItem: ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacingLarge
        spacing: Theme.spacingLarge

        // Banner area
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 60
            radius: Theme.radiusNormal
            color: Theme.senderColor(profileCard.userId)

            // Avatar overlapping banner bottom
            Rectangle {
                id: avatarCircle
                width: 64
                height: 64
                radius: 32
                color: Theme.bgDark
                border.color: Theme.bgDark
                border.width: 4
                anchors.bottom: parent.bottom
                anchors.bottomMargin: -32
                anchors.left: parent.left
                anchors.leftMargin: Theme.spacingLarge

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 4
                    radius: width / 2
                    color: Theme.senderColor(profileCard.userId)

                    Image {
                        anchors.fill: parent
                        source: {
                            if (profileCard.profileAvatarUrl !== "" && serverManager.activeServer) {
                                return serverManager.activeServer.resolveMediaUrl(profileCard.profileAvatarUrl);
                            }
                            return "";
                        }
                        visible: source !== ""
                        fillMode: Image.PreserveAspectCrop
                        layer.enabled: true
                    }

                    Text {
                        anchors.centerIn: parent
                        text: profileCard.profileDisplayName.charAt(0).toUpperCase()
                        font.pixelSize: 24
                        font.bold: true
                        color: "white"
                        visible: profileCard.profileAvatarUrl === ""
                    }
                }
            }
        }

        // Spacer for avatar overlap
        Item {
            Layout.preferredHeight: 20
        }

        // Display name
        Text {
            text: profileCard.profileDisplayName || profileCard.userId
            font.pixelSize: 20
            font.bold: true
            color: Theme.textPrimary
            Layout.fillWidth: true
            elide: Text.ElideRight
        }

        // User ID
        Text {
            text: profileCard.userId
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.textMuted
            Layout.fillWidth: true
            elide: Text.ElideRight
        }

        // Server name
        Text {
            text: serverManager.activeServer ? serverManager.activeServer.serverUrl : ""
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.textMuted
            Layout.fillWidth: true
            elide: Text.ElideRight
            visible: text !== ""
        }

        Item { Layout.fillHeight: true }

        // Message button
        Button {
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            visible: serverManager.activeServer !== null && profileCard.userId !== serverManager.activeServer.userId
            contentItem: Text {
                text: "Message"
                font.pixelSize: Theme.fontSizeNormal
                color: "white"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: parent.hovered ? Theme.accentHover : Theme.accent
                radius: Theme.radiusSmall
            }
            onClicked: {
                // TODO: Open DM with user
                profileCard.close();
            }
        }
    }
}
