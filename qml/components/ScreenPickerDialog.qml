import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// ScreenPickerDialog — Windows/Linux stand-in for the native macOS
// share picker. macOS gets SCContentSharingPicker (window/app/display
// selection) via ScreenShareController::showPicker(); on the other
// desktop platforms Qt's QScreenCapture path can only capture whole
// displays, so this modal lists ScreenShareController.availableScreens
// ({index, name, width, height, primary}) and starts capture of the
// clicked one via startForScreen(index).
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
            text: "Pick a display to share with the call. Everything on it "
                  + "will be visible to the other participants."
            wrapMode: Text.WordWrap
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.sm
            color: Theme.fg2
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
