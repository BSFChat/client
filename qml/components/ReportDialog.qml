import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import BSFChat

// "Report this to the server administrators."
//
// One dialog for both report endpoints — a message and an account — because
// they are the same act with different evidence attached, and a user who has
// learned one should not have to learn the other.
//
// ── What this dialog may and may not say ─────────────────────────────────
//
// Server-side a report is ONE ROW and one audit record
// (server/src/api/ReportHandler.h). It does not redact the message, mute or
// ban the sender, hide anything from anybody, or notify the reported account.
// Every effect happens later, by hand, by a person reading the queue.
//
// So the copy below promises exactly that and nothing more. It is tempting to
// write "we'll take care of it" over a button, and it would be a lie on a
// self-hosted server where the administrator might be asleep, on holiday, or
// the reported person's friend. What this dialog does instead is name the
// control that DOES have an immediate effect — the block — and offer it in the
// same breath, ticked by default when the reporter is not already blocking
// them. That pairing is the design: block to stop it, report to have it dealt
// with.
Popup {
    id: reportDialog

    // "message" or "user". Chooses the endpoint, the title and whether the
    // quoted preview is shown.
    property string kind: "user"
    property string targetUserId: ""
    property string targetDisplayName: ""
    // Only meaningful for kind === "message".
    property string roomId: ""
    property string eventId: ""
    property string preview: ""

    property bool busy: false
    property string errorMessage: ""
    // The token for the request in flight. A reply that does not carry it
    // belongs to an attempt this dialog no longer owns — it was closed and
    // reopened for somebody else — and applying it would tell the user their
    // NEW report had been filed when it was the old one.
    property string requestId: ""
    property int _seq: 0
    // "Also block" for a report still in flight when the dialog was closed
    // (Esc while Sending…). The block used to be dropped with the dialog.
    property bool _blockOnFiled: false
    property string _blockTarget: ""
    // What the server counts: UTF-8 bytes, not UTF-16 units.
    readonly property int reasonBytes: serverManager.activeServer
        ? serverManager.activeServer.messageByteLength(reasonField.text)
        : reasonField.text.length
    readonly property int maxReasonBytes: 1024

    readonly property string targetName:
        targetDisplayName.length > 0 ? targetDisplayName : targetUserId

    readonly property bool alreadyBlocked: {
        var s = serverManager.activeServer;
        if (!s || reportDialog.targetUserId === "") return false;
        // Reading `users` is the subscription — see MessageBubble.senderBlocked.
        var subscribe = s.blockedUsersModel.users;
        return s.isUserBlocked(reportDialog.targetUserId);
    }

    function openFor(reportKind, userId, displayName, room, event, bodyPreview) {
        reportDialog.kind = reportKind;
        reportDialog.targetUserId = userId;
        reportDialog.targetDisplayName = displayName;
        reportDialog.roomId = room;
        reportDialog.eventId = event;
        // Capped here rather than in the delegate: the quote is context for
        // the reporter, not the payload — the server takes its own snapshot of
        // the event, so a long message costs nothing by being cut here.
        reportDialog.preview = bodyPreview.length > 240
            ? bodyPreview.substring(0, 240) + "…"
            : bodyPreview;
        reportDialog.open();
    }

    parent: Overlay.overlay
    // Centred in the part of the window the keyboard leaves free, and inside
    // the safe area: on a phone the reason field used to open under the
    // keyboard. iOS only: Android's adjustResize already shrinks the window
    // (and with it the overlay) for the keyboard, so subtracting it there
    // would count it twice.
    readonly property real _kbHeight: Qt.platform.os === "ios" && Qt.inputMethod.visible
        ? Math.max(0, Qt.inputMethod.keyboardRectangle.height) : 0
    readonly property real _safeTop: _overlayProbe.SafeArea.margins.top
    readonly property real _freeHeight: (parent ? parent.height : 800) - _kbHeight - _safeTop
    width: Math.min(460, (parent ? parent.width : 460) - 32)
    height: Math.min(implicitHeight, _freeHeight - 32)
    x: parent ? (parent.width - width) / 2 : 0
    y: _safeTop + Math.max(16, (_freeHeight - height) / 2)
    Item { id: _overlayProbe; parent: Overlay.overlay; anchors.fill: parent; enabled: false }
    modal: true
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
        reportDialog.busy = false;
        reportDialog.errorMessage = "";
        reportDialog.requestId = "";
        reportDialog._blockOnFiled = false;
        reasonField.text = "";
        // Ticked by default only when it would change something. Pre-ticking
        // it for somebody already blocked would make the checkbox read as
        // "keep blocking", which is not what unticking it would do.
        alsoBlockCheck.checked = !reportDialog.alreadyBlocked;
    }
    onOpened: reasonField.forceActiveFocus()
    onClosed: {
        // Closed mid-send: keep listening for this request so a ticked
        // "Also block" still happens once the report is accepted.
        if (reportDialog.busy && alsoBlockCheck.checked && !reportDialog.alreadyBlocked) {
            reportDialog._blockOnFiled = true;
            reportDialog._blockTarget = reportDialog.targetUserId;
        } else {
            reportDialog.requestId = "";
        }
        reasonField.text = "";
    }

    function submit() {
        var s = serverManager.activeServer;
        if (!s || reportDialog.busy) return;
        reportDialog._seq += 1;
        reportDialog.requestId = "report-" + Date.now() + "-" + reportDialog._seq;
        reportDialog.busy = true;
        reportDialog.errorMessage = "";
        if (reportDialog.kind === "message")
            s.reportMessage(reportDialog.requestId, reportDialog.roomId,
                            reportDialog.eventId, reasonField.text);
        else
            s.reportUser(reportDialog.requestId, reportDialog.targetUserId,
                         reasonField.text);
    }

    // The block is applied as soon as the report is accepted, not before: a
    // report the server refused (rate limit, a message that has since been
    // redacted) leaves the dialog open so it can be retried, and a block
    // already applied underneath it would be an effect from a button the user
    // is still looking at.
    Connections {
        target: serverManager.activeServer
        ignoreUnknownSignals: true

        function onReportFiled(id) {
            if (id !== reportDialog.requestId) return;
            reportDialog.busy = false;
            var s = serverManager.activeServer;
            if (reportDialog._blockOnFiled) {
                reportDialog._blockOnFiled = false;
                reportDialog.requestId = "";
                if (s && !s.isUserBlocked(reportDialog._blockTarget))
                    s.blockUser(reportDialog._blockTarget);
                var hw = contentFlick.hostWindow;
                if (hw && hw.toastSuccess)
                    hw.toastSuccess(qsTr("Report sent to the server administrators."));
                return;
            }
            if (alsoBlockCheck.checked && s && !reportDialog.alreadyBlocked)
                s.blockUser(reportDialog.targetUserId);
            var w = contentFlick.hostWindow;
            if (w && w.toastSuccess)
                w.toastSuccess("Report sent to the server administrators.");
            reportDialog.close();
        }

        function onReportRejected(id, message) {
            if (id !== reportDialog.requestId) return;
            reportDialog.busy = false;
            if (reportDialog._blockOnFiled) {
                // The dialog is gone; say it where the user can see it.
                reportDialog._blockOnFiled = false;
                reportDialog.requestId = "";
                var hw = contentFlick.hostWindow;
                if (hw && hw.toastError) hw.toastError(message);
                return;
            }
            // Kept inside the dialog rather than raised as a toast that
            // outlives it: the rate-limit refusal is the common one and it
            // says WHEN to try again, which is only useful next to the button
            // it is talking about.
            reportDialog.errorMessage = message;
        }
    }


    contentItem: Flickable {
        // On the contentItem: a Popup is not an Item, so Accessible on the
        // root reached no screen reader and logged a warning (audit 3).
        Accessible.role: Accessible.Dialog
        Accessible.name: reportDialog.kind === "message"
            ? qsTr("Report this message")
            : qsTr("Report %1").arg(reportDialog.targetName)
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
                    color: Qt.rgba(Theme.warn.r, Theme.warn.g, Theme.warn.b, 0.15)
                    // Decorative tint behind the title glyph (§6).
                    Accessible.ignored: true
                    Icon {
                        anchors.centerIn: parent
                        name: "bolt"
                        size: 16
                        color: Theme.warn
                    }
                }
                Text {
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    text: reportDialog.kind === "message"
                        ? "Report this message"
                        : "Report " + reportDialog.targetName
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xl
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackTight.xl
                    color: Theme.fg0
                    wrapMode: Text.WordWrap
                }
            }

            // ── What actually happens ──
            Text {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                text: (reportDialog.kind === "message"
                        ? qsTr("This goes to the administrators of this server. They can read the message and decide what to do.")
                        : qsTr("This goes to the administrators of this server. They can look at this account and decide what to do."))
                    + " " + qsTr("Nothing changes right away, and %1 is not told that you reported them.")
                              .arg(reportDialog.targetName)
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
                wrapMode: Text.WordWrap
            }

            // ── What is being reported ──
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: quoteCol.implicitHeight + Theme.sp.s4 * 2
                visible: reportDialog.kind === "message"
                    && reportDialog.preview.length > 0
                color: Theme.bg2
                radius: Theme.r2
                border.color: Theme.line
                border.width: 1
                ColumnLayout {
                    id: quoteCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.sp.s4
                    anchors.rightMargin: Theme.sp.s4
                    spacing: 2
                    Text {
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                        text: reportDialog.targetName
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.accent
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                        text: reportDialog.preview
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                        color: Theme.fg1
                        wrapMode: Text.WordWrap
                        maximumLineCount: 4
                        elide: Text.ElideRight
                    }
                }
            }

            // ── Reason ──
            Text {
                text: "WHAT IS WRONG WITH IT? (OPTIONAL)"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWidest.xs
                color: Theme.fg3
            }
            TextArea {
                id: reasonField
                Layout.fillWidth: true
                Layout.preferredHeight: 84
                enabled: !reportDialog.busy
                placeholderText: reportDialog.kind === "message"
                    ? "Harassment, a scam, something illegal…"
                    : "Impersonation, a pattern of harassment, their profile…"
                placeholderTextColor: Theme.fg3
                color: Theme.fg0
                wrapMode: TextEdit.Wrap
                Accessible.role: Accessible.EditableText
                Accessible.name: qsTr("What is wrong with it?")
                Accessible.description: qsTr("Optional. The administrators read this alongside the report.")
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.md
                leftPadding: Theme.sp.s4
                rightPadding: Theme.sp.s4
                topPadding: Theme.sp.s3
                bottomPadding: Theme.sp.s3
                background: Rectangle {
                    color: Theme.bg0
                    radius: Theme.r2
                    border.color: reasonField.activeFocus ? Theme.accent : Theme.line
                    border.width: 1
                }
            }
            // Only once it matters. The server's ceiling is far above any
            // sentence a person types (server input_limits::kMaxReasonBytes),
            // so a permanent counter would be clutter over a limit nobody
            // meets — but a reason silently refused after the dialog closed
            // would be worse.
            Text {
                Layout.fillWidth: true
                visible: reportDialog.reasonBytes > 700
                textFormat: Text.PlainText
                text: reportDialog.reasonBytes > reportDialog.maxReasonBytes
                    ? qsTr("That is too long to send — shorten it a little.")
                    : qsTr("%1 / %2 bytes").arg(reportDialog.reasonBytes).arg(reportDialog.maxReasonBytes)
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                color: reportDialog.reasonBytes > reportDialog.maxReasonBytes ? Theme.danger : Theme.fg3
                horizontalAlignment: Text.AlignRight
            }

            // ── The control that has an effect now ──
            ThemedCheckBox {
                id: alsoBlockCheck
                Layout.fillWidth: true
                visible: !reportDialog.alreadyBlocked
                enabled: !reportDialog.busy
                text: "Also block " + reportDialog.targetName
                // ThemedCheckBox already declares the CheckBox role and the
                // checkable/checked pair (§5); only the name is per-call-site.
                Accessible.name: qsTr("Also block %1").arg(reportDialog.targetName)
            }
            Text {
                Layout.fillWidth: true
                visible: alsoBlockCheck.visible
                text: "Blocking takes effect immediately and privately — you "
                    + "stop seeing their messages, mentions and notifications. "
                    + "They are not told."
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                color: Theme.fg3
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                visible: reportDialog.alreadyBlocked
                textFormat: Text.PlainText
                text: "You are already blocking " + reportDialog.targetName + "."
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                color: Theme.fg3
                wrapMode: Text.WordWrap
            }

            // ── Refusal ──
            Text {
                Layout.fillWidth: true
                visible: reportDialog.errorMessage.length > 0
                textFormat: Text.PlainText
                text: reportDialog.errorMessage
                // The rate-limit refusal says WHEN to try again, which is no
                // use to somebody who never hears it (§8).
                Accessible.role: Accessible.AlertMessage
                Accessible.name: reportDialog.errorMessage
                onVisibleChanged: if (visible)
                                      Accessible.announce(Accessible.name,
                                                          Accessible.Assertive)
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
                    id: reportCancelBtn
                    enabled: !reportDialog.busy
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Cancel")
                    Accessible.onPressAction: reportDialog.close()
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
                        color: reportCancelBtn.hovered ? Theme.bg3 : "transparent"
                        border.color: Theme.line
                        border.width: 1
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                    onClicked: reportDialog.close()
                }

                Button {
                    id: reportSendBtn
                    enabled: !reportDialog.busy
                             && reportDialog.reasonBytes <= reportDialog.maxReasonBytes
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Send report")
                    Accessible.description: qsTr("Sends this to the administrators of this server")
                    Accessible.onPressAction: if (reportSendBtn.enabled)
                                                  reportSendBtn.clicked()
                    implicitHeight: Theme.controlHeight.md
                    contentItem: Text {
                        textFormat: Text.PlainText
                        text: reportDialog.busy ? "Sending…" : "Send report"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.semibold
                        color: reportSendBtn.enabled ? Theme.onAccent : Theme.fg3
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        implicitWidth: 132
                        radius: Theme.r2
                        color: !reportSendBtn.enabled ? Theme.bg3
                             : reportSendBtn.hovered ? Theme.accentDim : Theme.accent
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                    onClicked: reportDialog.submit()
                }
            }
        }
    }
}
