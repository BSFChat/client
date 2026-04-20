import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// ServerRail (per SPEC §3.1) — the 72px leftmost column. Server switcher
// plus (eventually) DM entry point + add-server button.
//
// Key visual vocabulary for the whole app lives here:
//   • 44×44 rounded-square icons that squircle-morph on active
//   • left-edge accent bar (4×28) that grows on hover, extends on active
//   • unread dot (8×8) with 2px bg0 halo, bottom-right
//   • notif count pill (danger bg, onAccent-like fg)
//   • hover scale 1.04, radius tween r3 → r2
Rectangle {
    id: rail
    color: Theme.bg0
    implicitWidth: Theme.layout.serverRailW

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: Theme.sp.s5
        anchors.bottomMargin: Theme.sp.s5
        spacing: Theme.sp.s3

        // DM entry point — placeholder until DMs land. The SPEC has this
        // at the top with its own active state tied to settings.screen.
        Item {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44
            visible: false // hide until DM screen exists

            Rectangle {
                anchors.fill: parent
                radius: Theme.r3
                color: Theme.bg2
                Text {
                    anchors.centerIn: parent
                    text: "\u0040"
                    font.pixelSize: 20
                    font.bold: true
                    color: Theme.fg1
                }
            }
        }

        // Thin divider under the DM icon. Hidden while DM is hidden.
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 28
            Layout.preferredHeight: 1
            color: Theme.lineSoft
            visible: false
        }

        // Server icons.
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.sp.s3
            clip: true

            ScrollBar.vertical: ThemedScrollBar {}
            model: serverManager.servers
            interactive: contentHeight > height

            delegate: Item {
                id: row
                width: ListView.view ? ListView.view.width : Theme.layout.serverRailW
                height: 52

                readonly property bool isActive: index === serverManager.activeServerIndex
                readonly property bool hasUnread: (model.unreadCount || 0) > 0
                readonly property bool isHovered: hoverArea.containsMouse

                // Left-edge active / hover indicator bar. Active = tall,
                // hover (inactive) = half-height nudge, unread (inactive,
                // not hovered) = short dot, otherwise hidden.
                Rectangle {
                    id: edgeBar
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: 4
                    radius: 2
                    color: Theme.accent
                    height: row.isActive ? 28
                          : row.isHovered ? 18
                          : row.hasUnread ? 8
                          : 0
                    visible: height > 0
                    Behavior on height {
                        NumberAnimation { duration: Theme.motion.fastMs
                                          easing.type: Easing.BezierSpline
                                          easing.bezierCurve: Theme.motion.bezier }
                    }
                }

                // Icon tile. Rounded-square → squircle morph on active, via
                // a radius animation. Hover pops a subtle scale.
                Rectangle {
                    id: tile
                    width: 44
                    height: 44
                    anchors.centerIn: parent
                    radius: row.isActive ? Theme.r2 : Theme.r3
                    color: row.isActive ? Theme.accent
                         : row.isHovered ? Theme.bg3
                         : Theme.bg2
                    scale: row.isHovered && !row.isActive ? 1.04 : 1.0
                    border.width: row.isActive ? 0 : 1
                    border.color: Theme.line

                    Behavior on radius { NumberAnimation { duration: Theme.motion.normalMs
                                                           easing.type: Easing.BezierSpline
                                                           easing.bezierCurve: Theme.motion.bezier } }
                    Behavior on scale { NumberAnimation { duration: Theme.motion.fastMs
                                                          easing.type: Easing.BezierSpline
                                                          easing.bezierCurve: Theme.motion.bezier } }
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                    // Two-letter abbreviation if we can derive one, else
                    // the first character. Strips leading non-alphanumerics
                    // so @-prefixed names still produce a sensible glyph.
                    Text {
                        anchors.centerIn: parent
                        text: {
                            var name = (model.displayName || "?");
                            var stripped = name.replace(/^[^a-zA-Z0-9]+/, "");
                            if (stripped.length === 0) return "?";
                            return stripped.charAt(0).toUpperCase();
                        }
                        font.family: Theme.fontSans
                        font.pixelSize: 18
                        font.weight: Theme.fontWeight.semibold
                        color: row.isActive ? Theme.onAccent : Theme.fg0
                    }
                }

                // Unread dot (bottom-right) — only when inactive and has
                // unread. Active servers have all their rooms visible so the
                // dot would be redundant.
                Rectangle {
                    width: 10
                    height: 10
                    radius: 5
                    color: Theme.fg0
                    border.width: 2
                    border.color: Theme.bg0
                    anchors.right: tile.right
                    anchors.bottom: tile.bottom
                    anchors.rightMargin: -1
                    anchors.bottomMargin: -1
                    visible: !row.isActive && row.hasUnread
                }

                MouseArea {
                    id: hoverArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: serverManager.setActiveServer(index)
                }

                ToolTip.visible: hoverArea.containsMouse
                ToolTip.text: model.displayName || ""
                ToolTip.delay: 400
            }
        }

        // Add-server button — ghost style, dashed border, matches the
        // icon footprint so visual rhythm stays intact.
        Item {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44

            Rectangle {
                id: addTile
                anchors.fill: parent
                radius: Theme.r3
                color: addArea.containsMouse ? Theme.bg3 : "transparent"
                border.width: 1
                border.color: addArea.containsMouse ? Theme.accent : Theme.line
                scale: addArea.containsMouse ? 1.04 : 1.0

                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                Behavior on border.color { ColorAnimation { duration: Theme.motion.fastMs } }
                Behavior on scale { NumberAnimation { duration: Theme.motion.fastMs
                                                      easing.type: Easing.BezierSpline
                                                      easing.bezierCurve: Theme.motion.bezier } }

                Text {
                    anchors.centerIn: parent
                    text: "+"
                    font.family: Theme.fontSans
                    font.pixelSize: 22
                    font.weight: Theme.fontWeight.regular
                    color: addArea.containsMouse ? Theme.accent : Theme.fg2
                }

                MouseArea {
                    id: addArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: loginDialog.open()
                }

                ToolTip.visible: addArea.containsMouse
                ToolTip.text: "Add server"
                ToolTip.delay: 400
            }
        }
    }
}
