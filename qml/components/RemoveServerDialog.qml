import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// Confirm before a server leaves this device. One component for every place
// that can do it — the server rail's "Remove server…", desktop settings' "Log
// out" and the mobile overflow's "Sign out" — because all three call the same
// serverManager.leaveServer() and drop the saved login with it (U-1). The two
// sign-out entry points used to do that on the first tap, with no way back
// short of finding the server's address and signing in again.
//
// `signOut` only changes the words: it is the same act either way, and saying
// "sign out" where the user pressed "sign out" is what makes the dialog read
// as an answer to their click rather than a different question.
Dialog {
    id: removeServerDialog
    property int serverIndex: -1
    // What to call the server in the sentence. Falls back to "this server"
    // so an unnamed entry never produces "signed out of ."
    property string serverLabel: ""
    property bool signOut: false
    property string _url: ""

    // Opens the confirm for the roster row `index`. The label is read here,
    // once, so a name arriving from sync while the dialog is up does not
    // rewrite the sentence under the user's cursor.
    function openFor(index, label, asSignOut) {
        if (index < 0) return;
        removeServerDialog.serverIndex = index;
        var c = serverManager.connectionAt(index);
        removeServerDialog._url = c ? c.serverUrl : "";
        removeServerDialog.serverLabel = label || "";
        removeServerDialog.signOut = !!asSignOut;
        removeServerDialog.open();
    }

    // Resolved again at confirm time, by address: the roster can renumber
    // while the dialog is up (another server removed, a failed add dropped),
    // and a stale index would sign the user out of a different server.
    function confirm() {
        var idx = -1;
        var n = serverManager.servers ? serverManager.servers.rowCount() : 0;
        for (var i = 0; i < n; ++i) {
            var c = serverManager.connectionAt(i);
            if (c && c.serverUrl === removeServerDialog._url) { idx = i; break; }
        }
        if (removeServerDialog._url === "") idx = removeServerDialog.serverIndex;
        removeServerDialog.close();
        if (idx >= 0) serverManager.leaveServer(idx);
    }

    readonly property string _name: serverLabel.length > 0 ? serverLabel : "this server"

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(360, (parent ? parent.width : 360) - 32)
    modal: true
    title: ""

    Accessible.role: Accessible.Dialog
    Accessible.name: signOut ? qsTr("Sign out?") : qsTr("Remove server?")

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r2
        border.color: Theme.line
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: Theme.sp.s4

        Text {
            Layout.fillWidth: true
            text: removeServerDialog.signOut ? "Sign out?" : "Remove server?"
            elide: Text.ElideRight
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.lg
            font.weight: Theme.fontWeight.semibold
            color: Theme.fg0
        }
        Text {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            text: removeServerDialog.signOut
                  ? "You'll be signed out of " + removeServerDialog._name
                    + " and it will be removed from this device, along with "
                    + "its saved login. Your account on the server is "
                    + "untouched — you can add it again later."
                  : "This removes " + removeServerDialog._name
                    + " and its saved login from this device. Your account "
                    + "on the server is untouched — you can add it again later."
            wrapMode: Text.Wrap
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.sm
            color: Theme.fg2
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: Theme.sp.s3

            Button {
                text: "Cancel"
                flat: true
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Cancel")
                Accessible.description: qsTr("Keep the server")
                Accessible.onPressAction: removeServerDialog.close()
                contentItem: Text {
                    text: parent.text
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    color: Theme.fg1
                    horizontalAlignment: Text.AlignHCenter
                    Accessible.ignored: true
                }
                background: Rectangle {
                    color: parent.hovered ? Theme.bg2 : "transparent"
                    radius: Theme.r2
                }
                onClicked: removeServerDialog.close()
            }
            Button {
                text: removeServerDialog.signOut ? "Sign out" : "Remove"
                Accessible.role: Accessible.Button
                Accessible.name: removeServerDialog.signOut
                                 ? qsTr("Sign out") : qsTr("Remove server")
                Accessible.description: qsTr(
                    "Removes the server and its saved login from this device")
                Accessible.onPressAction: removeServerDialog.confirm()
                contentItem: Text {
                    text: parent.text
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    font.weight: Theme.fontWeight.semibold
                    color: Theme.onAccent
                    horizontalAlignment: Text.AlignHCenter
                    Accessible.ignored: true
                }
                background: Rectangle {
                    color: parent.hovered ? Qt.lighter(Theme.danger, 1.1) : Theme.danger
                    radius: Theme.r2
                }
                onClicked: removeServerDialog.confirm()
            }
        }
    }
}
