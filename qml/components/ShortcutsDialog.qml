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

    // §9. `onOpened: forceActiveFocus()` at the foot of this file puts focus
    // on the popup itself, which is right for a reference sheet: there is
    // nothing to fill in and nothing to confirm, only text to read, and the
    // close control is a plain Rectangle that takes no keyboard focus.

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
            // Icon-only control: the name is the label it would have had.
            // On the Rectangle rather than the MouseArea because attached
            // accessibility only takes effect on the visual item (§2).
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Close")
            Accessible.onPressAction: shortcutsDialog.close()
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
    readonly property bool _mac: Qt.platform.os === "osx" || Qt.platform.os === "macos"
    readonly property string modKey: _mac ? "⌘" : "Ctrl"
    readonly property string altKey: _mac ? "⌥" : "Alt"
    readonly property string shiftKey: _mac ? "⇧" : "Shift"
    // Server cycling is bound to the PHYSICAL Control key on macOS (Meta+Tab
    // in Qt), because ⌘Tab belongs to the OS app switcher (main.qml).
    readonly property string ctrlKey: _mac ? "⌃" : "Ctrl"

    readonly property var sections: [
        {
            title: "Navigation",
            entries: [
                { label: "Cycle servers",           keys: [ctrlKey + " Tab"] },
                { label: "Previous server",         keys: [ctrlKey + " " + shiftKey + " Tab"] },
                { label: "Jump to server N",        keys: [modKey + " 1…9"] },
                { label: "Previous channel / DM",   keys: [altKey + " ↑"] },
                { label: "Next channel / DM",       keys: [altKey + " ↓"] },
                { label: "Search messages",         keys: [modKey + " K"] },
                { label: "Focus the composer",      keys: [modKey + " L"] },
                { label: "Move between controls",   keys: ["Tab"] },
                { label: "Open a focused row",      keys: ["Enter"] },
                { label: "Row menu (channel, server, member, DM)", keys: [shiftKey + " F10"] }
            ]
        },
        {
            title: "Window & Layout",
            entries: [
                { label: "Toggle member list",      keys: [modKey + " M"] },
                { label: "User settings",           keys: [modKey + " ,"] },
                { label: "Client settings",         keys: [modKey + " " + shiftKey + " ,"] },
                { label: "Show shortcuts (this)",   keys: [modKey + " /"] }
            ]
        },
        {
            title: "Messages",
            entries: [
                { label: "Send message",            keys: ["Enter"] },
                { label: "New line in composer",    keys: [shiftKey + " Enter"] },
                { label: "Cancel reply / edit",     keys: ["Esc"] },
                { label: "Edit your last message",  keys: ["s/old/new/"] },
                { label: "Right-click for menu",    keys: ["Right-click"] }
            ]
        },
        {
            // Keyboard message actions (U-3). Handled in MessageView._kbKey;
            // keep the two in step.
            title: "Timeline",
            entries: [
                { label: "Into timeline (empty composer)", keys: ["↑"] },
                { label: "Previous / next message", keys: ["↑ ↓"] },
                { label: "Oldest / newest loaded",  keys: ["Home End"] },
                { label: "Message actions menu",    keys: [shiftKey + " F10"] },
                { label: "Reply",                   keys: ["R"] },
                { label: "Edit (your message)",     keys: ["E"] },
                { label: "Delete",                  keys: ["Del"] },
                { label: "Add reaction",            keys: ["+"] },
                { label: "Back to composer",        keys: ["Esc"] }
            ]
        },
        {
            title: "Emoji Picker",
            entries: [
                { label: "Search",                  keys: ["Type"] },
                { label: "Move through emoji",      keys: ["↑ ↓ ← →"] },
                { label: "Pick",                    keys: ["Enter"] },
                { label: "Categories",              keys: ["Tab"] },
                { label: "Close",                   keys: ["Esc"] }
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
        // On the contentItem: a Popup is not an Item, so Accessible on the
        // root reached no screen reader and logged a warning (audit 3).
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Keyboard Shortcuts")
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
                Accessible.ignored: true
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
                                textFormat: Text.PlainText
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
                                Accessible.ignored: true
                            }
                        }

                        Repeater {
                            model: modelData.entries
                            delegate: RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.sp.s4
                                Text {
                                    textFormat: Text.PlainText
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
                                                textFormat: Text.PlainText
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
