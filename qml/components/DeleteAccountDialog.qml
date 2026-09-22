import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import BSFChat

// Delete this account, permanently. Apple App Store guideline 5.1.1(v).
//
// ── The confirmation is the whole dialog ─────────────────────────────────
//
// POST /account/deactivate is irreversible and, for an account that signs in
// through the identity provider, the FIRST request is the deletion — the
// server holds no password to challenge such an account with and admits it on
// the bearer token alone (server/src/api/AuthHandler.cpp). So the password
// prompt is a second gate that only some accounts ever see, and it must never
// be mistaken for the confirmation. The confirmation is here, before anything
// is sent: the user types the name of their account.
//
// Typing is not theatre. This control sits one click from "Log out" in a
// settings popup, it cannot be undone, and there is no trash can to fish it
// back out of. A button that can be hit by accident is the wrong shape for it.
//
// ── What the copy has to say, because the server actually does this ──────
//
// The account, its sessions, its profile and its memberships go. THE MESSAGES
// DO NOT. They are other people's conversations — a channel that silently
// loses half its history because somebody left is worse for everyone who
// stayed, and the server's deactivate_user is written that way deliberately.
// Saying so here is not a disclaimer, it is the one fact a person deciding
// this needs and will not guess.
Popup {
    id: deleteDialog

    readonly property var conn: serverManager.activeServer

    // What the user must type. The localpart, not the full mxid: "@amy:h" has
    // punctuation that is easy to get wrong and proves nothing extra.
    readonly property string confirmWord: {
        if (!deleteDialog.conn) return "";
        var id = deleteDialog.conn.userId;
        if (id.length > 1 && id.charAt(0) === "@") {
            var colon = id.indexOf(":");
            if (colon > 1) return id.substring(1, colon);
        }
        return id;
    }
    readonly property bool confirmed:
        confirmField.text.trim() === deleteDialog.confirmWord
        && deleteDialog.confirmWord.length > 0

    // True from the moment a request that could delete the account goes out.
    // It is what lets onConnChanged tell "the account was deleted" apart from
    // "the user switched servers with this dialog open" — a deletion is the
    // only one of the two that takes the connection away while this is set,
    // because a refusal leaves it running.
    property bool _deleting: false
    // Captured up front: by the time the deletion lands there may be no
    // connection left to read a URL from.
    property string _serverUrl: ""
    property bool _finished: false

    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    width: Math.min(460, (parent ? parent.width : 460) - 32)
    height: Math.min(implicitHeight, (parent ? parent.height : 800) - 32)
    modal: true
    // No CloseOnPressOutside: a stray click outside a half-typed
    // confirmation should not be how this dialog goes away, because the user
    // has to find it again and retype. Escape and Cancel are the ways out.
    closePolicy: Popup.CloseOnEscape
    padding: Theme.sp.s7

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0.0; to: 1.0; duration: 150; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: 0.95; to: 1.0; duration: 150; easing.type: Easing.OutCubic }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1.0; to: 0.0; duration: 100; easing.type: Easing.InCubic }
        NumberAnimation { property: "scale"; from: 1.0; to: 0.95; duration: 100; easing.type: Easing.InCubic }
    }

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1
    }

    onAboutToShow: {
        confirmField.text = "";
        passwordField.text = "";
        deleteDialog._deleting = false;
        deleteDialog._finished = false;
        deleteDialog._serverUrl = deleteDialog.conn ? deleteDialog.conn.serverUrl : "";
        // Abandons any challenge left over from a dialog that was closed
        // mid-flight, so this attempt starts from the first request rather
        // than inheriting a half-finished UIA session.
        if (deleteDialog.conn) deleteDialog.conn.cancelAccountDeletion();
    }
    onOpened: confirmField.forceActiveFocus()
    onClosed: {
        passwordField.text = "";
        confirmField.text = "";
        if (deleteDialog.conn) deleteDialog.conn.cancelAccountDeletion();
    }

    Connections {
        target: deleteDialog.conn
        ignoreUnknownSignals: true

        function onAccountDeletionPasswordRequired() {
            passwordField.forceActiveFocus();
        }

        function onAccountDeactivated(serverUrl) {
            deleteDialog._finish(serverUrl);
        }
    }

    // The second half of that, and NOT redundant with it.
    //
    // accountDeactivated is also what ServerManager listens for, and its slot
    // is connected first — so by the time this file's handler would run, the
    // roster has already removed the entry and changed activeServer, which is
    // the binding `conn` is made of and therefore the Connections target
    // above. Qt is under no obligation to deliver a signal to a target that
    // moved mid-emission, and the observable result would be a modal left on
    // screen over a sidebar that has already rearranged itself.
    //
    // So the disappearance of the connection is treated as the completion in
    // its own right. It also covers the ordinary case of the user switching
    // servers with this open, where there is nothing to announce.
    onConnChanged: {
        if (!deleteDialog.opened) return;
        deleteDialog._finish(deleteDialog._serverUrl);
    }

    // Idempotent: whichever of the two paths arrives first does the work.
    function _finish(serverUrl) {
        if (deleteDialog._finished) return;
        deleteDialog._finished = true;
        var announce = deleteDialog._deleting;
        deleteDialog.close();
        if (!announce) return;
        var w = contentFlick.hostWindow;
        if (w && w.toastInfo) {
            w.toastInfo(serverUrl.length > 0
                ? "Your account on " + serverUrl + " has been deleted."
                : "Your account has been deleted.");
        }
    }

    contentItem: Flickable {
        id: contentFlick
        // The Popup takes its implicitHeight from this, and a Flickable's own
        // is 0 — without this line the dialog clamps to nothing and opens
        // invisible. Tracking the form means the dialog is exactly as tall as
        // its content until the window is too short for it, and scrolls from
        // there.
        implicitHeight: form.implicitHeight
        // The ApplicationWindow, for the toast helpers. Resolved HERE, on an
        // Item: `Window` is an Item attached property, so reading it off the
        // Popup (which is not an Item) or off a Connections block (U-M6)
        // silently yields undefined and swallows the toast.
        readonly property var hostWindow: contentFlick.Window.window
        contentWidth: width
        contentHeight: form.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThemedScrollBar {}

        ColumnLayout {
            id: form
            width: contentFlick.width
            spacing: Theme.sp.s4

            // ── Header ──
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp.s3
                Rectangle {
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: Theme.controlHeight.sm
                    radius: Theme.r2
                    color: Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.15)
                    Icon {
                        anchors.centerIn: parent
                        name: "x"
                        size: 16
                        color: Theme.danger
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: "Delete your account"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xl
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackTight.xl
                    color: Theme.fg0
                    wrapMode: Text.WordWrap
                }
            }

            Text {
                Layout.fillWidth: true
                text: "This deletes your account on "
                    + (deleteDialog.conn ? deleteDialog.conn.serverUrl : "this server")
                    + ". It cannot be undone, and support cannot bring it back."
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
                wrapMode: Text.WordWrap
            }

            // ── What goes ──
            Text {
                Layout.fillWidth: true
                text: "What is deleted"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWidest.xs
                color: Theme.fg3
            }
            Text {
                Layout.fillWidth: true
                text: "• Your sign-in, on this and every other device\n"
                    + "• Your display name, nickname and avatar\n"
                    + "• Your membership of every channel here\n"
                    + "• Your blocked list and your settings on this server"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg1
                wrapMode: Text.WordWrap
            }

            // ── What stays. The part people do not expect. ──
            InfoBanner {
                icon: "eye"
                tint: Theme.warn
                text: "Messages you have sent stay in their channels. They are "
                    + "part of other people's conversations and deleting them "
                    + "would take half of those conversations with them. Delete "
                    + "any you want gone before you delete the account — after "
                    + "this you will not be able to."
            }

            // ── The confirmation ──
            Text {
                Layout.fillWidth: true
                text: "Type " + deleteDialog.confirmWord + " to confirm"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWidest.xs
                color: Theme.fg3
                wrapMode: Text.WordWrap
            }
            TextField {
                id: confirmField
                Layout.fillWidth: true
                enabled: deleteDialog.conn !== null
                    && !deleteDialog.conn.accountDeletionBusy
                placeholderText: deleteDialog.confirmWord
                placeholderTextColor: Theme.fg3
                color: Theme.fg0
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSize.md
                leftPadding: Theme.sp.s4
                rightPadding: Theme.sp.s4
                topPadding: Theme.sp.s3
                bottomPadding: Theme.sp.s3
                background: Rectangle {
                    color: Theme.bg0
                    radius: Theme.r2
                    border.color: confirmField.activeFocus ? Theme.accent : Theme.line
                    border.width: 1
                }
            }

            // ── The password stage, when the server asks for one ──
            //
            // Only after the first request has been refused with a UIA
            // challenge. An account signed in through the identity provider
            // has no password here and never sees this.
            Text {
                Layout.fillWidth: true
                visible: deleteDialog.conn !== null
                    && deleteDialog.conn.accountDeletionNeedsPassword
                text: "YOUR PASSWORD"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWidest.xs
                color: Theme.fg3
            }
            TextField {
                id: passwordField
                Layout.fillWidth: true
                visible: deleteDialog.conn !== null
                    && deleteDialog.conn.accountDeletionNeedsPassword
                enabled: deleteDialog.conn !== null
                    && !deleteDialog.conn.accountDeletionBusy
                placeholderText: "Password"
                placeholderTextColor: Theme.fg3
                echoMode: TextInput.Password
                color: Theme.fg0
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.md
                leftPadding: Theme.sp.s4
                rightPadding: Theme.sp.s4
                topPadding: Theme.sp.s3
                bottomPadding: Theme.sp.s3
                background: Rectangle {
                    color: Theme.bg0
                    radius: Theme.r2
                    border.color: passwordField.activeFocus ? Theme.accent : Theme.line
                    border.width: 1
                }
                onAccepted: deleteDialog.submit()
            }

            Text {
                Layout.fillWidth: true
                visible: deleteDialog.conn !== null
                    && deleteDialog.conn.accountDeletionError.length > 0
                text: deleteDialog.conn ? deleteDialog.conn.accountDeletionError : ""
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.danger
                wrapMode: Text.WordWrap
            }

            // ── Footer ──
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.sp.s2
                spacing: Theme.sp.s3
                Item { Layout.fillWidth: true }

                Button {
                    id: deleteCancelBtn
                    enabled: deleteDialog.conn === null
                        || !deleteDialog.conn.accountDeletionBusy
                    implicitHeight: Theme.controlHeight.md
                    contentItem: Text {
                        text: "Cancel"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.medium
                        color: Theme.fg1
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        implicitWidth: 100
                        radius: Theme.r2
                        color: deleteCancelBtn.hovered ? Theme.bg3 : "transparent"
                        border.color: Theme.line
                        border.width: 1
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                    onClicked: deleteDialog.close()
                }

                Button {
                    id: deleteConfirmBtn
                    enabled: deleteDialog.confirmed
                        && deleteDialog.conn !== null
                        && !deleteDialog.conn.accountDeletionBusy
                        && (!deleteDialog.conn.accountDeletionNeedsPassword
                            || passwordField.text.length > 0)
                    implicitHeight: Theme.controlHeight.md
                    contentItem: Text {
                        text: {
                            if (!deleteDialog.conn) return "Delete account";
                            if (deleteDialog.conn.accountDeletionBusy) return "Deleting…";
                            if (deleteDialog.conn.accountDeletionNeedsPassword)
                                return "Confirm and delete";
                            return "Delete account";
                        }
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.semibold
                        color: deleteConfirmBtn.enabled ? Theme.onAccent : Theme.fg3
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        implicitWidth: 168
                        radius: Theme.r2
                        color: !deleteConfirmBtn.enabled ? Theme.bg3
                             : deleteConfirmBtn.hovered ? Qt.darker(Theme.danger, 1.15)
                             : Theme.danger
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                    onClicked: deleteDialog.submit()
                }
            }
        }
    }

    // One button, two stages. Which request goes out is the CONNECTION's
    // decision, not this dialog's — it is the only thing that knows whether
    // the server has issued a challenge, and duplicating that here is how the
    // two would drift into sending a password to a server that had not asked.
    function submit() {
        if (!deleteDialog.conn) return;
        if (!deleteDialog.confirmed) return;
        if (deleteDialog.conn.accountDeletionNeedsPassword) {
            if (passwordField.text.length === 0) return;
            deleteDialog._deleting = true;
            deleteDialog.conn.submitAccountDeletionPassword(passwordField.text);
            return;
        }
        // Set before the call, not after: for an account with no password on
        // this server the first request IS the deletion, and it can complete
        // — connection and all — before this function returns.
        deleteDialog._deleting = true;
        deleteDialog.conn.beginAccountDeletion();
    }
}
