import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// A yes/no confirmation in the app's dialog vocabulary.
//
// Shared by every "are you sure" that had none (leave a channel, close a DM,
// kick, ban, block, rotate a bot token). Focus starts on Cancel so Enter
// on a misfired shortcut never confirms something destructive, Esc cancels,
// and the accessible name/role sit on the contentItem — on a Popup root
// they reach no screen reader (audit 3).
//
//   ConfirmDialog {
//       id: leaveConfirm
//       title: qsTr("Leave #general?")
//       message: qsTr("…")
//       confirmText: qsTr("Leave")
//       onConfirmed: server.leaveRoom(roomId)
//   }
Popup {
    id: dlg
    property string title: ""
    property string message: ""
    property string confirmText: qsTr("Confirm")
    property string cancelText: qsTr("Cancel")
    property bool destructive: true
    // Free slot for callers that open one dialog for many rows.
    property var payload: null
    signal confirmed(var payload)

    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    width: Math.min(400, (Overlay.overlay ? Overlay.overlay.width : 400) - 32)
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: Theme.sp.s7

    onOpened: cancelBtn.forceActiveFocus()

    function accept() {
        var p = dlg.payload;
        dlg.close();
        dlg.confirmed(p);
    }

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: Theme.sp.s4
        Accessible.role: Accessible.Dialog
        Accessible.name: dlg.title
        Accessible.description: dlg.message

        Text {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            text: dlg.title
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.xl
            font.weight: Theme.fontWeight.semibold
            color: Theme.fg0
            wrapMode: Text.WordWrap
            Accessible.ignored: true
        }
        Text {
            Layout.fillWidth: true
            visible: dlg.message.length > 0
            textFormat: Text.PlainText
            text: dlg.message
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.sm
            color: Theme.fg2
            wrapMode: Text.WordWrap
            Accessible.ignored: true
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.sp.s1
            spacing: Theme.sp.s3
            Item { Layout.fillWidth: true }
            Button {
                id: cancelBtn
                text: dlg.cancelText
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: text
                onClicked: dlg.close()
                Keys.onReturnPressed: dlg.close()
                Keys.onEnterPressed: dlg.close()
                contentItem: Text {
                    textFormat: Text.PlainText
                    text: cancelBtn.text
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    color: Theme.fg1
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    Accessible.ignored: true
                }
                background: Rectangle {
                    implicitWidth: 100
                    implicitHeight: 36
                    radius: Theme.r2
                    color: cancelBtn.hovered ? Theme.bg3 : "transparent"
                    border.color: cancelBtn.activeFocus ? Theme.accent : Theme.line
                    border.width: cancelBtn.activeFocus ? 2 : 1
                }
            }
            Button {
                id: okBtn
                text: dlg.confirmText
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: text
                Accessible.description: dlg.message
                onClicked: dlg.accept()
                Keys.onReturnPressed: dlg.accept()
                Keys.onEnterPressed: dlg.accept()
                contentItem: Text {
                    textFormat: Text.PlainText
                    text: okBtn.text
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    font.weight: Theme.fontWeight.semibold
                    color: Theme.onAccent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    Accessible.ignored: true
                }
                background: Rectangle {
                    implicitWidth: 120
                    implicitHeight: 36
                    radius: Theme.r2
                    readonly property color base: dlg.destructive ? Theme.danger : Theme.accent
                    color: okBtn.hovered ? Qt.lighter(base, 1.1) : base
                    border.color: Theme.fg0
                    border.width: okBtn.activeFocus ? 2 : 0
                }
            }
        }
    }
}
