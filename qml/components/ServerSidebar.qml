import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
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

        // DMs destination — sits at the top of the rail as a peer
        // to the server icons, even though each DM is physically
        // hosted on some server. Clicking flips the channel panel
        // into DM-aggregate mode (every 1:1 across every connected
        // server in one list). Clicking any server icon below
        // reverts the mode; the sidebar is single-select across DMs
        // + servers.
        Item {
            id: dmEntry
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44

            readonly property bool _active: serverManager.viewingDms

            Rectangle {
                anchors.fill: parent
                radius: dmMouse.containsMouse || dmEntry._active
                    ? Theme.r2 : Theme.r3
                color: dmEntry._active ? Theme.accent
                     : dmMouse.containsMouse ? Theme.bg3 : Theme.bg2
                Behavior on radius { NumberAnimation { duration: Theme.motion.fastMs } }
                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                Icon {
                    anchors.centerIn: parent
                    name: "at"
                    size: 20
                    color: dmEntry._active ? Theme.onAccent : Theme.fg1
                }

                MouseArea {
                    id: dmMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: serverManager.setViewingDms(true)
                }

                ToolTip.visible: dmMouse.containsMouse
                ToolTip.text: "Direct messages"
                ToolTip.delay: 500
            }

            // Left-edge accent bar mirrors the server icons'
            // selection affordance so the DMs destination looks
            // native to the rail.
            Rectangle {
                visible: dmEntry._active
                anchors.left: parent.left
                anchors.leftMargin: -8
                anchors.verticalCenter: parent.verticalCenter
                width: 4
                height: 28
                radius: 2
                color: Theme.fg0
            }
        }

        // Thin divider separating DMs from the server list — makes
        // the "DMs live above servers" grouping legible.
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 28
            Layout.preferredHeight: 1
            color: Theme.lineSoft
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

                // A server is "active" only when we're not viewing
                // DMs. Otherwise the DM chip above owns the single
                // selection state and the server tiles render as
                // inactive, even though one of them is technically
                // m_activeServer underneath.
                readonly property bool isActive:
                    index === serverManager.activeServerIndex
                    && !serverManager.viewingDms
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

                    // Initial letter — shown when there's no icon
                    // uploaded, or the icon is still loading.
                    Text {
                        anchors.centerIn: parent
                        visible: serverIcon.status !== Image.Ready
                              || !model.iconUrl
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

                    // Uploaded server icon. The containing Rectangle is
                    // rounded, so small margins hide any sharp image
                    // corners inside the radius; if the image fails
                    // the initial letter above fades back in via its
                    // `visible` binding.
                    Image {
                        id: serverIcon
                        anchors.fill: parent
                        anchors.margins: 1
                        source: model.iconUrl || ""
                        visible: status === Image.Ready
                        fillMode: Image.PreserveAspectCrop
                        smooth: true
                        asynchronous: true
                        cache: true
                    }
                }

                // Unread dot (bottom-right) — only when inactive and has
                // unread. Active servers have all their rooms visible so
                // the dot would be redundant. Uses the danger palette
                // because any server-level unread is "you have mentions
                // or activity across the whole server" — worth a stronger
                // cue than the neutral-grey channel-row unread.
                Rectangle {
                    width: 10
                    height: 10
                    radius: 5
                    color: Theme.danger
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
                    onClicked: {
                        // Picking a server icon is the primary
                        // way to leave DM view — it's single-
                        // select across the rail.
                        serverManager.setViewingDms(false);
                        serverManager.setActiveServer(index);
                    }
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

                Icon {
                    anchors.centerIn: parent
                    name: "plus"
                    size: 18
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
