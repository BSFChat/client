import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

Rectangle {
    color: Theme.bgDark

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Header
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 48

            Text {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacingLarge
                verticalAlignment: Text.AlignVCenter
                text: "MEMBERS"
                font.pixelSize: Theme.fontSizeSmall
                font.bold: true
                color: Theme.textMuted
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.bgDarkest
            }
        }

        // Member list
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: serverManager.activeServer ? serverManager.activeServer.memberListModel : null

            delegate: Rectangle {
                width: ListView.view.width
                height: 42
                color: memberMouse.containsMouse ? Theme.bgLight : "transparent"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingLarge
                    anchors.rightMargin: Theme.spacingLarge
                    spacing: Theme.spacingNormal

                    // Avatar
                    Rectangle {
                        width: 32
                        height: 32
                        radius: 16
                        color: Theme.senderColor(model.userId)

                        Text {
                            anchors.centerIn: parent
                            text: model.displayName.charAt(0).toUpperCase()
                            font.pixelSize: 14
                            font.bold: true
                            color: "white"
                        }
                    }

                    Text {
                        text: model.displayName
                        font.pixelSize: Theme.fontSizeNormal
                        color: Theme.textSecondary
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }

                MouseArea {
                    id: memberMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        memberProfileCard.userId = model.userId;
                        memberProfileCard.profileDisplayName = model.displayName;
                        memberProfileCard.open();
                    }
                }
            }
        }
    }

    // Profile card for clicking on member names
    UserProfileCard {
        id: memberProfileCard
        parent: Overlay.overlay
    }
}
