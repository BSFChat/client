import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// Keyboard-shortcuts reference. Accessible via ⌘/ (Ctrl+/ on
// Win/Linux). Static data; future shortcuts added to `sections` below.
Popup {
    id: shortcutsDialog
    anchors.centerIn: Overlay.overlay
    width: Math.min(parent ? parent.width * 0.85 : 560, 560)
    height: Math.min(parent ? parent.height * 0.85 : 600, 600)
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1

        Rectangle {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: Theme.sp.s5
            anchors.rightMargin: Theme.sp.s5
            width: 28; height: 28
            radius: Theme.r1
            color: xMouse.containsMouse ? Theme.bg3 : "transparent"
            z: 10
            Icon {
                anchors.centerIn: parent
                name: "x"
                size: 14
                color: xMouse.containsMouse ? Theme.fg0 : Theme.fg2
            }
            MouseArea {
                id: xMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: shortcutsDialog.close()
            }
        }
    }

    // Platform-adaptive modifier — ⌘ on macOS, Ctrl elsewhere. Qt's
    // Shortcut sequences already bind "Ctrl+…" to Cmd on macOS, so the
    // strings here are purely for display.
    readonly property string modKey: Qt.platform.os === "osx" ? "⌘" : "Ctrl"
    readonly property string altKey: Qt.platform.os === "osx" ? "⌥" : "Alt"
    readonly property string shiftKey: "Shift"

    readonly property var sections: [
        {
            title: "Navigation",
            entries: [
                { label: "Cycle servers",           keys: [modKey + " Tab"] },
                { label: "Previous server",         keys: [modKey + " Shift Tab"] },
                { label: "Jump to server N",        keys: [modKey + " 1…9"] },
                { label: "Previous channel",        keys: [altKey + " ↑"] },
                { label: "Next channel",            keys: [altKey + " ↓"] }
            ]
        },
        {
            title: "Window & Layout",
            entries: [
                { label: "Toggle member list",      keys: [modKey + " M"] },
                { label: "User settings",           keys: [modKey + " ,"] },
                { label: "Client settings",         keys: [modKey + " Shift ,"] },
                { label: "Show shortcuts (this)",   keys: [modKey + " /"] }
            ]
        },
        {
            title: "Messages",
            entries: [
                { label: "Send message",            keys: ["Enter"] },
                { label: "New line in composer",    keys: [shiftKey + " Enter"] },
                { label: "Cancel reply / edit",     keys: ["Esc"] },
                { label: "Right-click for menu",    keys: ["Right-click"] }
            ]
        },
        {
            title: "Image Viewer",
            entries: [
                { label: "Zoom in",                 keys: [modKey + " +"] },
                { label: "Zoom out",                keys: [modKey + " −"] },
                { label: "Reset zoom & position",   keys: [modKey + " 0"] },
                { label: "Close",                   keys: ["Esc"] },
                { label: "Open in browser",         keys: ["Middle-click inline"] }
            ]
        }
    ]

    contentItem: ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Title bar — 24px semibold with a thin rule under, matches the
        // TabHeader vocabulary in ServerSettings.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 64
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.sp.s7
                text: "Keyboard Shortcuts"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xxl
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackTight.xxl
                color: Theme.fg0
            }
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.line
            }
        }

        // Scrollable body.
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ScrollBar.vertical: ThemedScrollBar {}
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: shortcutsDialog.width - Theme.sp.s7 * 2
                x: Theme.sp.s7
                y: Theme.sp.s5
                spacing: Theme.sp.s7

                Repeater {
                    model: shortcutsDialog.sections
                    delegate: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.sp.s3

                        // Widest-tracked small-caps fg3 section header
                        // + thin rule — same pattern as ChannelSettings.
                        Item {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 24
                            Text {
                                id: sectionLabel
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.title.toUpperCase()
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.xs
                                font.weight: Theme.fontWeight.semibold
                                font.letterSpacing: Theme.trackWidest.xs
                                color: Theme.fg3
                            }
                            Rectangle {
                                anchors.left: sectionLabel.right
                                anchors.leftMargin: Theme.sp.s3
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                height: 1
                                color: Theme.line
                            }
                        }

                        Repeater {
                            model: modelData.entries
                            delegate: RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.sp.s4
                                Text {
                                    text: modelData.label
                                    color: Theme.fg1
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.fontSize.md
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }

                                // Keys are rendered as a row of kbd-style
                                // chips. A single `keys` string can carry
                                // multiple tokens separated by spaces;
                                // each becomes its own chip for readability.
                                Row {
                                    spacing: 4
                                    Repeater {
                                        model: modelData.keys[0].split(/\s+/)
                                        delegate: Rectangle {
                                            readonly property string token: modelData
                                            height: 22
                                            width: kbdText.implicitWidth + 12
                                            radius: Theme.r1
                                            color: Theme.bg2
                                            border.color: Theme.line
                                            border.width: 1
                                            Text {
                                                id: kbdText
                                                anchors.centerIn: parent
                                                text: parent.token
                                                font.family: Theme.fontMono
                                                font.pixelSize: Theme.fontSize.xs
                                                font.weight: Theme.fontWeight.semibold
                                                color: Theme.fg0
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Item { Layout.preferredHeight: Theme.sp.s5 }
            }
        }
    }

    onOpened: forceActiveFocus()
}
