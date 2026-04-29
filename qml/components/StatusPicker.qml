import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import BSFChat

// Set your own presence + custom status message. Opened from the
// user menu in the channel-list footer (desktop) or the mobile
// overflow menu. Posts to PUT /presence; other clients see the
// update on their next /sync.
//
// Three presence states + a free-form 80-char message line. The
// message is optional and shows up under the user's display name
// across the app (MemberList rows, UserProfileCard, DM peer label).
Popup {
    id: picker
    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    width: Math.min(parent ? parent.width - 32 : 360, 420)
    height: contentColumn.implicitHeight + Theme.sp.s5 * 2
    modal: true
    padding: Theme.sp.s5
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1
    }

    onOpened: {
        var s = serverManager.activeServer;
        if (!s) return;
        // Snapshot current state so cancelling restores it.
        _initialPresence = s.selfPresence();
        _initialMessage = s.selfStatusMessage();
        _selectedPresence = _initialPresence;
        statusField.text = _initialMessage;
        statusField.forceActiveFocus();
    }

    property string _initialPresence: "online"
    property string _initialMessage: ""
    property string _selectedPresence: "online"

    contentItem: ColumnLayout {
        id: contentColumn
        spacing: Theme.sp.s4

        Text {
            text: "Set your status"
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.lg
            font.weight: Theme.fontWeight.semibold
            color: Theme.fg0
        }

        // Presence radio strip — three big tap targets across the
        // popup width. Uses the same dot vocabulary as MemberList
        // so users recognise the tints.
        component PresenceRow: Rectangle {
            id: row
            property string presence: ""
            property string title: ""
            property string subtitle: ""
            property color dotColor: Theme.fg3
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            radius: Theme.r2
            color: picker._selectedPresence === presence
                ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.18)
                : (rowMouse.containsMouse ? Theme.bg2 : "transparent")
            border.color: picker._selectedPresence === presence
                ? Theme.accent : Theme.line
            border.width: 1
            Behavior on color       { ColorAnimation { duration: Theme.motion.fastMs } }
            Behavior on border.color { ColorAnimation { duration: Theme.motion.fastMs } }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.sp.s4
                anchors.rightMargin: Theme.sp.s4
                spacing: Theme.sp.s3

                Rectangle {
                    Layout.preferredWidth: 12
                    Layout.preferredHeight: 12
                    radius: 6
                    color: row.dotColor
                    border.color: Theme.bg1
                    border.width: 2
                }
                ColumnLayout {
                    spacing: 1
                    Layout.fillWidth: true
                    Text {
                        text: row.title
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.base
                        font.weight: Theme.fontWeight.medium
                        color: Theme.fg0
                    }
                    Text {
                        text: row.subtitle
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                        color: Theme.fg3
                    }
                }
                Icon {
                    visible: picker._selectedPresence === row.presence
                    name: "check"; size: 14; color: Theme.accent
                }
            }
            MouseArea {
                id: rowMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: picker._selectedPresence = row.presence
            }
        }

        PresenceRow {
            presence: "online"
            title: "Online"
            subtitle: "Available to everyone"
            dotColor: Theme.online
        }
        PresenceRow {
            presence: "unavailable"
            title: "Away"
            subtitle: "Idle / busy"
            dotColor: Theme.warning
        }
        PresenceRow {
            presence: "offline"
            title: "Offline"
            subtitle: "Hidden — appear offline to everyone"
            dotColor: Theme.fg3
        }

        Item { Layout.preferredHeight: Theme.sp.s2 }

        Text {
            text: "Custom status (optional)"
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.sm
            font.weight: Theme.fontWeight.medium
            color: Theme.fg2
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            radius: Theme.r2
            color: Theme.bg0
            border.color: statusField.activeFocus ? Theme.accent : Theme.line
            border.width: 1
            Behavior on border.color { ColorAnimation { duration: Theme.motion.fastMs } }

            TextField {
                id: statusField
                anchors.fill: parent
                anchors.leftMargin: Theme.sp.s3
                anchors.rightMargin: Theme.sp.s3
                placeholderText: "What are you up to?"
                background: Item {}
                color: Theme.fg0
                placeholderTextColor: Theme.fg3
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.base
                selectByMouse: true
                maximumLength: 80
                Keys.onReturnPressed: picker._save()
            }
        }

        // Action row.
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.sp.s2
            spacing: Theme.sp.s3

            Item { Layout.fillWidth: true }

            Button {
                text: "Cancel"
                onClicked: picker.close()
                contentItem: Text {
                    text: parent.text
                    color: Theme.fg1
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.base
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: parent.hovered ? Theme.bg3 : Theme.bg2
                    border.color: Theme.line
                    border.width: 1
                    radius: Theme.r2
                    implicitWidth: 88
                    implicitHeight: 40
                }
            }
            Button {
                text: "Save"
                onClicked: picker._save()
                contentItem: Text {
                    text: parent.text
                    color: Theme.onAccent
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.base
                    font.weight: Theme.fontWeight.semibold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: parent.hovered ? Theme.accentDim : Theme.accent
                    radius: Theme.r2
                    implicitWidth: 88
                    implicitHeight: 40
                }
            }
        }
    }

    function _save() {
        var s = serverManager.activeServer;
        if (!s) { close(); return; }
        // Push presence first, then message — both fire individual
        // PUT /presence calls; the server coalesces into one entry.
        if (_selectedPresence !== _initialPresence) {
            s.setSelfPresence(_selectedPresence);
        }
        if (statusField.text !== _initialMessage) {
            s.setSelfStatusMessage(statusField.text);
        }
        close();
    }
}
