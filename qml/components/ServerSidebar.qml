import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

Rectangle {
    id: sidebar
    color: Theme.bgDarkest

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: Theme.spacingNormal
        spacing: Theme.spacingNormal

        // Server list
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: (sidebar.width - 48) / 2
            Layout.rightMargin: Layout.leftMargin
            spacing: Theme.spacingNormal
            clip: true
            model: serverManager.servers

            delegate: Item {
                width: 48
                height: 48

                Rectangle {
                    id: serverIcon
                    anchors.fill: parent
                    radius: index === serverManager.activeServerIndex ? Theme.radiusLarge : 24
                    color: index === serverManager.activeServerIndex ? Theme.accent : Theme.bgLight
                    border.width: 0

                    Behavior on radius {
                        NumberAnimation { duration: 150 }
                    }

                    Text {
                        anchors.centerIn: parent
                        text: model.displayName.charAt(0).toUpperCase()
                        font.pixelSize: 20
                        font.bold: true
                        color: Theme.textPrimary
                    }
                }

                // Active indicator
                Rectangle {
                    width: 4
                    height: index === serverManager.activeServerIndex ? 36 : (hovered ? 20 : 8)
                    radius: 2
                    color: Theme.textPrimary
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.left
                    anchors.rightMargin: -14
                    visible: true

                    Behavior on height {
                        NumberAnimation { duration: 150 }
                    }
                }

                // Unread dot
                Rectangle {
                    width: 10
                    height: 10
                    radius: 5
                    color: Theme.danger
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.topMargin: -2
                    anchors.rightMargin: -2
                    visible: {
                        // Show unread indicator for non-active servers that have unread messages
                        if (index === serverManager.activeServerIndex) return false;
                        // Check the connection's hasUnread property
                        var conn = serverManager.activeServer; // We can't easily access other connections from QML
                        return false; // Will be enhanced when ServerListModel gets unread support
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: serverManager.setActiveServer(index)
                }

                ToolTip.visible: hovered
                ToolTip.text: model.displayName

                property bool hovered: false
                HoverHandler {
                    onHoveredChanged: parent.hovered = hovered
                }
            }
        }

        // Add server button
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            Layout.bottomMargin: Theme.spacingNormal

            Rectangle {
                id: addButton
                width: 48
                height: 48
                radius: 24
                color: addButtonArea.containsMouse ? Theme.success : Theme.bgLight
                anchors.centerIn: parent

                Behavior on color {
                    ColorAnimation { duration: 150 }
                }

                Text {
                    anchors.centerIn: parent
                    text: "+"
                    font.pixelSize: 24
                    color: addButtonArea.containsMouse ? "white" : Theme.success
                }

                MouseArea {
                    id: addButtonArea
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    hoverEnabled: true
                    onClicked: loginDialog.open()
                }
            }
        }
    }
}
