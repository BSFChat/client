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

        // Server list. Each row spans the full sidebar width; the icon is
        // centered inside it and the state indicator is pinned to the
        // sidebar's left edge.
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.spacingNormal
            clip: true
            model: serverManager.servers

            delegate: Item {
                id: serverRow
                width: ListView.view ? ListView.view.width : 72
                height: 48

                readonly property bool isActive: index === serverManager.activeServerIndex
                readonly property bool hasUnread: (model.unreadCount || 0) > 0

                // Active / unread indicator flush with the sidebar's left edge.
                // Long pill = active server. Short pill = non-active server
                // with unread. Hidden otherwise.
                Rectangle {
                    id: indicator
                    width: 4
                    radius: 2
                    color: Theme.textPrimary
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    height: serverRow.isActive ? 36
                          : serverRow.hasUnread ? 8
                          : 0
                    visible: height > 0

                    Behavior on height {
                        NumberAnimation { duration: 150 }
                    }
                }

                // Icon, centered.
                Rectangle {
                    id: serverIcon
                    width: 48
                    height: 48
                    anchors.centerIn: parent
                    radius: serverRow.isActive ? Theme.radiusLarge : 24
                    color: serverRow.isActive ? Theme.accent : Theme.bgLight
                    border.width: 0

                    Behavior on radius {
                        NumberAnimation { duration: 150 }
                    }

                    Text {
                        anchors.centerIn: parent
                        text: (model.displayName || "?").charAt(0).toUpperCase()
                        font.pixelSize: 20
                        font.bold: true
                        color: Theme.textPrimary
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: serverManager.setActiveServer(index)
                }

                ToolTip.visible: hovered
                ToolTip.text: model.displayName || ""

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
