import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// Pending invites with Accept / Decline (audit 3, #6).
//
// Used twice: the server sidebar lists that server's channel invites, the @
// view lists DM invites from every server. Each entry is one of
// ServerConnection.pendingInvites' maps; `serverIndex` (set by
// ServerManager.allDirectInvites) routes the answer to the right
// connection, otherwise the active one answers.
ColumnLayout {
    id: inviteList
    property var invites: []
    property string title: qsTr("INVITES")
    spacing: 0
    visible: invites.length > 0

    function _conn(inv) {
        if (inv.serverIndex !== undefined && inv.serverIndex >= 0)
            return serverManager.connectionAt(inv.serverIndex);
        return serverManager.activeServer;
    }
    function _label(inv) {
        if (inv.isDirect) return inv.inviterName || inv.inviterId;
        return inv.roomName ? "#" + inv.roomName : qsTr("a channel");
    }
    function _detail(inv) {
        var who = inv.inviterName || inv.inviterId;
        if (inv.isDirect)
            return inv.serverName ? qsTr("wants to message you on %1").arg(inv.serverName)
                                  : qsTr("wants to message you");
        return who ? qsTr("Invited by %1").arg(who) : qsTr("Invitation");
    }
    function accept(inv) { var c = _conn(inv); if (c) c.acceptInvite(inv.roomId); }
    function decline(inv) { var c = _conn(inv); if (c) c.declineInvite(inv.roomId); }

    Text {
        text: inviteList.title
        textFormat: Text.PlainText
        Layout.fillWidth: true
        Layout.preferredHeight: 32
        Layout.leftMargin: Theme.sp.s7
        verticalAlignment: Text.AlignVCenter
        font.family: Theme.fontSans
        font.pixelSize: Theme.fontSize.xs
        font.weight: Theme.fontWeight.semibold
        font.letterSpacing: Theme.trackWidest.xs
        color: Theme.fg3
        Accessible.role: Accessible.StaticText
        Accessible.name: qsTr("Invites, %n pending", "", inviteList.invites.length)
    }

    Repeater {
        model: inviteList.invites
        delegate: Rectangle {
            id: inviteRow
            required property var modelData
            Layout.fillWidth: true
            Layout.leftMargin: Theme.sp.s2
            Layout.rightMargin: Theme.sp.s2
            implicitHeight: inviteCol.implicitHeight + Theme.sp.s3 * 2
            radius: Theme.r1
            color: Theme.bg2
            Accessible.role: Accessible.ListItem
            Accessible.name: inviteList._label(modelData) + ", "
                             + inviteList._detail(modelData)

            ColumnLayout {
                id: inviteCol
                anchors.fill: parent
                anchors.margins: Theme.sp.s3
                spacing: Theme.sp.s2

                Text {
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    text: inviteList._label(inviteRow.modelData)
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.base
                    font.weight: Theme.fontWeight.semibold
                    color: Theme.fg0
                    elide: Text.ElideRight
                    Accessible.ignored: true
                }
                Text {
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    text: inviteList._detail(inviteRow.modelData)
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xs
                    color: Theme.fg2
                    elide: Text.ElideRight
                    Accessible.ignored: true
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp.s2
                    Button {
                        id: acceptBtn
                        text: qsTr("Accept")
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Accept invite to %1")
                                         .arg(inviteList._label(inviteRow.modelData))
                        onClicked: inviteList.accept(inviteRow.modelData)
                        Keys.onReturnPressed: clicked()
                        Keys.onEnterPressed: clicked()
                        contentItem: Text {
                            textFormat: Text.PlainText
                            text: acceptBtn.text
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.sm
                            font.weight: Theme.fontWeight.semibold
                            color: Theme.onAccent
                            horizontalAlignment: Text.AlignHCenter
                            Accessible.ignored: true
                        }
                        background: Rectangle {
                            radius: Theme.r1
                            color: acceptBtn.hovered ? Theme.accentDim : Theme.accent
                            border.width: acceptBtn.activeFocus ? 2 : 0
                            border.color: Theme.fg0
                        }
                    }
                    Button {
                        id: declineBtn
                        text: qsTr("Decline")
                        flat: true
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Decline invite to %1")
                                         .arg(inviteList._label(inviteRow.modelData))
                        onClicked: inviteList.decline(inviteRow.modelData)
                        Keys.onReturnPressed: clicked()
                        Keys.onEnterPressed: clicked()
                        contentItem: Text {
                            textFormat: Text.PlainText
                            text: declineBtn.text
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.sm
                            color: Theme.fg1
                            horizontalAlignment: Text.AlignHCenter
                            Accessible.ignored: true
                        }
                        background: Rectangle {
                            radius: Theme.r1
                            color: declineBtn.hovered ? Theme.bg3 : "transparent"
                            border.width: declineBtn.activeFocus ? 2 : 1
                            border.color: declineBtn.activeFocus ? Theme.accent : Theme.line
                        }
                    }
                    Item { Layout.fillWidth: true }
                }
            }
        }
    }
    Item { Layout.preferredHeight: Theme.sp.s2 }
}
