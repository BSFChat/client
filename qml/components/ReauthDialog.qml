import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// Sign in again to a server that is already in the sidebar.
//
// Distinct from LoginDialog, which ADDS a server: the URL is fixed, there is
// no registration path, and cancelling must leave the server exactly where it
// is. That separation is the point — on 2026-09-19, when the homeserver's
// access_tokens table was purged, the only recovery available was to remove
// the server and add it back through LoginDialog, which is not a recovery
// path, it is a workaround.
//
// Only ever shown for a server that advertises m.login.password and no OIDC
// flow. An OIDC server signs in through the browser, so ServerConnection
// runs that directly and this dialog never opens.
Dialog {
    id: dialog
    title: "Sign in again"
    anchors.centerIn: parent
    width: Math.min(400, (parent ? parent.width : 400) - 32)
    height: Math.min(implicitHeight, (parent ? parent.height : 800) - 32)
    modal: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape

    // Which sidebar row we are signing back in to. Set from the signal
    // rather than by the caller: the dialog subscribes to ServerManager once
    // and does not need to be pre-bound to a connection before the flow that
    // needs it has started. Both entry points — the banner button and the
    // server rail's context menu — just call reauthenticateServer(index).
    property int serverIndex: -1
    readonly property var conn: serverIndex >= 0
        ? serverManager.connectionAt(serverIndex) : null

    property string errorMessage: ""
    property bool busy: false

    Connections {
        target: serverManager

        // Only ever emitted for a server with a password flow and no OIDC.
        function onReauthPasswordRequired(index, serverUrl, userId) {
            dialog.serverIndex = index;
            dialog.errorMessage = "";
            dialog.busy = false;
            usernameField.text = dialog.localpartOf(userId);
            passwordField.text = "";
            dialog.open();
            passwordField.forceActiveFocus();
        }
        function onReauthFailed(index, serverUrl, error) {
            if (index !== dialog.serverIndex) return;
            dialog.busy = false;
            // Only meaningful while the dialog is up; the OIDC path reports
            // its failures through the toast surface in main.qml.
            if (dialog.opened) dialog.errorMessage = error;
        }
    }

    Connections {
        target: dialog.conn
        ignoreUnknownSignals: true
        function onLoginSucceeded() {
            dialog.busy = false;
            dialog.close();
        }
    }

    // "@josh:chat.bsfchat.com" -> "josh". The password flow wants the
    // localpart; showing the full MXID and sending it verbatim is a
    // plausible way to make a correct password look wrong.
    function localpartOf(userId) {
        if (!userId) return "";
        var s = userId.charAt(0) === "@" ? userId.substring(1) : userId;
        var colon = s.indexOf(":");
        return colon >= 0 ? s.substring(0, colon) : s;
    }

    function submit() {
        if (!dialog.conn) return;
        if (passwordField.text.length === 0) return;
        dialog.busy = true;
        dialog.errorMessage = "";
        dialog.conn.reauthWithPassword(usernameField.text.trim(),
                                       passwordField.text);
    }

    Flickable {
        id: contentFlick
        anchors.fill: parent
        contentWidth: width
        contentHeight: form.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: form
            width: contentFlick.width
            spacing: Theme.sp.s3

            Text {
                Layout.fillWidth: true
                text: dialog.conn
                      ? "Your session on " + dialog.conn.serverUrl
                        + " has ended. Sign in again to reconnect."
                      : ""
                wrapMode: Text.Wrap
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
            }

            TextField {
                id: usernameField
                Layout.fillWidth: true
                placeholderText: "Username"
                enabled: !dialog.busy
                font.family: Theme.fontSans
                onAccepted: passwordField.forceActiveFocus()
            }

            TextField {
                id: passwordField
                Layout.fillWidth: true
                placeholderText: "Password"
                echoMode: TextInput.Password
                enabled: !dialog.busy
                font.family: Theme.fontSans
                onAccepted: dialog.submit()
            }

            Text {
                Layout.fillWidth: true
                visible: dialog.errorMessage.length > 0
                text: dialog.errorMessage
                wrapMode: Text.Wrap
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.danger
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp.s2

                Item { Layout.fillWidth: true }

                Button {
                    text: "Cancel"
                    enabled: !dialog.busy
                    onClicked: dialog.close()
                }
                Button {
                    text: dialog.busy ? "Signing in…" : "Sign in"
                    enabled: !dialog.busy && passwordField.text.length > 0
                    onClicked: dialog.submit()
                }
            }
        }
    }
}
