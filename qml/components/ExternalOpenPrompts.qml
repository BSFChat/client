import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import BSFChat

// The "are you sure?" step in front of everything that leaves the app with
// somebody else's input: a link whose text names a different site than it
// goes to (LinkOpener::confirmationRequested), and an attachment that is not
// plain image/video/audio (ServerConnection::mediaOpenConfirmationRequested).
// Refusals — a link with a scheme we never open — are toasted here too.
//
// One instance per window shell (main.qml, MobileMain.qml). Everything shown
// is sender-controlled, so every Text is PlainText.
Item {
    id: prompts

    // main.qml already toasts mediaOpenFailed; the mobile shell does not.
    property bool toastMediaFailures: false
    // Where the prompt is centred. The mobile shell passes its inset-aware
    // popup surface.
    property Item popupParent: Overlay.overlay

    // The shared attached ToolTip (one instance per engine) shows member,
    // server and reaction names, and the Basic style's Text in it is AutoText
    // — so a name of `<img src=…>` would render. Made plain once, here, since
    // both shells mount this component.
    Component.onCompleted: {
        var tip = ToolTip.toolTip;
        if (tip && tip.contentItem && tip.contentItem.textFormat !== undefined)
            tip.contentItem.textFormat = Text.PlainText;
    }

    function _toast(text, kind) {
        var w = Window.window;
        if (w && typeof w.toast === "function") w.toast(text, kind);
    }

    function _ask(title, body, confirmLabel, action) {
        confirm.title = title;
        confirm.body = body;
        confirm.confirmLabel = confirmLabel;
        confirm.action = action;
        confirm.open();
    }

    Connections {
        target: typeof linkOpener !== "undefined" ? linkOpener : null
        ignoreUnknownSignals: true
        // `host` is ASCII (punycode) and `shownUrl` fully percent-encoded,
        // so a lookalike domain cannot pass for the real one here (C2-7).
        function onConfirmationRequested(href, host, shownUrl) {
            if (String(shownUrl).toLowerCase().indexOf("mailto:") === 0) {
                prompts._ask(qsTr("Write an email?"),
                             qsTr("This opens your mail app to write to %1.\n\n%2")
                                 .arg(host).arg(shownUrl),
                             qsTr("Open mail app"),
                             function() { linkOpener.openConfirmed(href); });
                return;
            }
            prompts._ask(qsTr("Open this link?"),
                         qsTr("This link goes to %1, not the address its text shows.\n\n%2")
                             .arg(host).arg(shownUrl),
                         qsTr("Open link"),
                         function() { linkOpener.openConfirmed(href); });
        }
        function onRefused(href, reason) {
            prompts._toast(qsTr("Link not opened: %1").arg(reason), "warn");
        }
        function onOpenFailed(href) {
            prompts._toast(qsTr("Couldn't open the link in your browser"), "error");
        }
    }

    Connections {
        target: serverManager ? serverManager.activeServer : null
        ignoreUnknownSignals: true
        // `fileName` arrives with bidi/control characters stripped, and the
        // type it will really open as is shown on its own line, so a name
        // like "invoice<U+202E>fdp.exe" cannot dress itself up (C2-7).
        function onMediaOpenConfirmationRequested(mxcUri, fileName, fileType, eventId) {
            var server = serverManager.activeServer;
            prompts._ask(qsTr("Open this file?"),
                         qsTr("“%1” will be opened with your system's default app. "
                              + "Only open files from people you trust.\n\nOpens as: %2")
                             .arg(fileName !== "" ? fileName : qsTr("Unnamed file"))
                             .arg(fileType !== "" ? fileType
                                                  : qsTr("no file type (your system will ask)")),
                         qsTr("Open file"),
                         function() {
                             if (server) server.openMediaExternally(mxcUri, true, eventId);
                         });
        }
        function onMediaOpenFailed(error) {
            if (prompts.toastMediaFailures)
                prompts._toast(qsTr("Couldn't open that file: %1").arg(error), "error");
        }
    }

    Popup {
        id: confirm
        parent: prompts.popupParent
        anchors.centerIn: parent
        width: Math.min(420, (parent ? parent.width : 420) - 32)
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: Theme.sp.s7

        property string title: ""
        property string body: ""
        property string confirmLabel: ""
        property var action: null

        onClosed: action = null

        background: Rectangle {
            color: Theme.bg1
            radius: Theme.r3
            border.color: Theme.line
            border.width: 1
        }

        contentItem: ColumnLayout {
            spacing: Theme.sp.s4
            Text {
                Layout.fillWidth: true
                text: confirm.title
                textFormat: Text.PlainText
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xl
                font.weight: Theme.fontWeight.semibold
                color: Theme.fg0
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                text: confirm.body
                textFormat: Text.PlainText
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
                wrapMode: Text.WrapAnywhere
                maximumLineCount: 12
                elide: Text.ElideRight
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.sp.s1
                spacing: Theme.sp.s3
                Item { Layout.fillWidth: true }
                Button {
                    id: cancelBtn
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Cancel")
                    Accessible.onPressAction: confirm.close()
                    contentItem: Text {
                        text: qsTr("Cancel")
                        textFormat: Text.PlainText
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        color: Theme.fg1
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        Accessible.ignored: true
                    }
                    background: Rectangle {
                        color: cancelBtn.hovered ? Theme.bg3 : "transparent"
                        border.color: Theme.line
                        border.width: 1
                        radius: Theme.r2
                        implicitWidth: 100
                        implicitHeight: 36
                    }
                    onClicked: confirm.close()
                }
                Button {
                    id: okBtn
                    Accessible.role: Accessible.Button
                    Accessible.name: confirm.confirmLabel
                    Accessible.description: confirm.body
                    Accessible.onPressAction: okBtn.clicked()
                    contentItem: Text {
                        text: confirm.confirmLabel
                        textFormat: Text.PlainText
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.onAccent
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        Accessible.ignored: true
                    }
                    background: Rectangle {
                        color: okBtn.hovered ? Qt.lighter(Theme.accent, 1.1) : Theme.accent
                        radius: Theme.r2
                        implicitWidth: 120
                        implicitHeight: 36
                    }
                    onClicked: {
                        var act = confirm.action;
                        confirm.close();
                        if (act) act();
                    }
                }
            }
        }
    }
}
