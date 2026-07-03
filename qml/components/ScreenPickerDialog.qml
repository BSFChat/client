import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// ScreenPickerDialog — Windows/Linux stand-in for the native macOS
// share picker. macOS gets SCContentSharingPicker (window/app/display
// selection) via ScreenShareController::showPicker(); on the other
// desktop platforms this modal lists both displays
// (ScreenShareController.availableScreens) and individual windows
// (availableWindows, enumerated fresh on every open), starting
// capture of the clicked entry via startForScreen(index) /
// startForWindow(index).
//
// The Windows section hides itself when enumeration returns nothing
// (e.g. Wayland without the screencast portal) — the dialog then
// degrades to the old displays-only picker.
//
// Dumb view by design: no platform logic here — the caller (VoiceDock)
// decides when to open this vs. the native picker. Styling mirrors
// ServerSidebar's editServerDialog; Theme.* tokens only.
Dialog {
    id: dialog

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: 420
    modal: true
    title: ""

    // Window lists churn constantly — re-enumerate on every open so
    // the user sees what's on their desktop right now.
    onOpened: if (typeof screenShare !== "undefined")
                  screenShare.refreshWindows()

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r2
        border.color: Theme.line
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: Theme.sp.s4

        Text {
            text: "Share your screen"
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.lg
            font.weight: Theme.fontWeight.semibold
            color: Theme.fg0
        }
        Text {
            Layout.fillWidth: true
            text: "Pick a display or window to share with the call. "
                  + "Sharing a display makes everything on it visible "
                  + "to the other participants."
            wrapMode: Text.WordWrap
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.sm
            color: Theme.fg2
        }

        Text {
            // Section label only earns its row when there is a second
            // section to distinguish from.
            visible: windowList.count > 0
            text: "Displays"
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.xs
            font.weight: Theme.fontWeight.semibold
            color: Theme.fg3
        }

        // One clickable card per display. availableScreens is a plain
        // QVariantList so a Repeater over it is fine — the list only
        // changes on monitor hot-(un)plug, which re-resolves the
        // binding wholesale via screensChanged.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.sp.s2

            Repeater {
                model: typeof screenShare !== "undefined"
                       ? screenShare.availableScreens : []

                delegate: Rectangle {
                    id: row
                    required property var modelData

                    Layout.fillWidth: true
                    implicitHeight: rowContent.implicitHeight + Theme.sp.s3 * 2
                    radius: Theme.r2
                    color: rowHover.containsMouse ? Theme.bg3 : Theme.bg2
                    border.color: rowHover.containsMouse ? Theme.accent : Theme.line
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                    RowLayout {
                        id: rowContent
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: Theme.sp.s3
                        anchors.rightMargin: Theme.sp.s3
                        spacing: Theme.sp.s3

                        Icon {
                            name: "screen-share"
                            size: 18
                            color: Theme.fg1
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Text {
                                Layout.fillWidth: true
                                text: row.modelData.name
                                elide: Text.ElideRight
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.md
                                font.weight: Theme.fontWeight.medium
                                color: Theme.fg0
                            }
                            Text {
                                text: row.modelData.width + " × " + row.modelData.height
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.fontSize.xs
                                color: Theme.fg2
                            }
                        }

                        Rectangle {
                            visible: row.modelData.primary === true
                            implicitWidth: primaryLabel.implicitWidth + Theme.sp.s2 * 2
                            implicitHeight: primaryLabel.implicitHeight + Theme.sp.s1 * 2
                            radius: Theme.r1
                            color: Theme.accentGlow

                            Text {
                                id: primaryLabel
                                anchors.centerIn: parent
                                text: "Primary"
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.xs
                                font.weight: Theme.fontWeight.semibold
                                color: Theme.accent
                            }
                        }
                    }

                    MouseArea {
                        id: rowHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            screenShare.startForScreen(row.modelData.index);
                            dialog.close();
                        }
                    }
                }
            }

            // Degenerate case (no screens enumerated) — say so rather
            // than presenting an inexplicably empty dialog.
            Text {
                visible: typeof screenShare === "undefined"
                         || screenShare.availableScreens.length === 0
                Layout.fillWidth: true
                text: "No displays available to share."
                wrapMode: Text.WordWrap
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg3
            }
        }

        Text {
            visible: windowList.count > 0
            text: "Windows"
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.xs
            font.weight: Theme.fontWeight.semibold
            color: Theme.fg3
        }

        // Windows get a capped, scrollable list — a desktop can have
        // dozens where it only ever has a handful of displays.
        ListView {
            id: windowList
            Layout.fillWidth: true
            implicitHeight: Math.min(contentHeight, 240)
            visible: count > 0
            clip: true
            spacing: Theme.sp.s2
            boundsBehavior: Flickable.StopAtBounds

            model: typeof screenShare !== "undefined"
                   ? screenShare.availableWindows : []

            delegate: Rectangle {
                id: winRow
                required property var modelData

                width: ListView.view.width
                implicitHeight: winContent.implicitHeight + Theme.sp.s3 * 2
                radius: Theme.r2
                color: winHover.containsMouse ? Theme.bg3 : Theme.bg2
                border.color: winHover.containsMouse ? Theme.accent : Theme.line
                border.width: 1
                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                RowLayout {
                    id: winContent
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.sp.s3
                    anchors.rightMargin: Theme.sp.s3
                    spacing: Theme.sp.s3

                    Icon {
                        name: "app-window"
                        size: 18
                        color: Theme.fg1
                    }

                    Text {
                        Layout.fillWidth: true
                        text: winRow.modelData.name
                        elide: Text.ElideRight
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.medium
                        color: Theme.fg0
                    }
                }

                MouseArea {
                    id: winHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        screenShare.startForWindow(winRow.modelData.index);
                        dialog.close();
                    }
                }
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: Theme.sp.s3

            Button {
                text: "Cancel"
                flat: true
                contentItem: Text {
                    text: parent.text
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    color: Theme.fg1
                    horizontalAlignment: Text.AlignHCenter
                }
                background: Rectangle {
                    color: parent.hovered ? Theme.bg2 : "transparent"
                    radius: Theme.r2
                }
                onClicked: dialog.close()
            }
        }
    }
}
