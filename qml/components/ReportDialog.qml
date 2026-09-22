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
    anchors.centerIn: Overlay.overlay
    width: Math.min(460, (parent ? parent.width : 460) - 32)
    height: Math.min(implicitHeight, (parent ? parent.height : 800) - 32)
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
        reasonField.text = "";
        // Ticked by default only when it would change something. Pre-ticking
        // it for somebody already blocked would make the checkbox read as
        // "keep blocking", which is not what unticking it would do.
        alsoBlockCheck.checked = !reportDialog.alreadyBlocked;
    }
    onOpened: reasonField.forceActiveFocus()
    onClosed: {
        reportDialog.requestId = "";
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
            // Kept inside the dialog rather than raised as a toast that
            // outlives it: the rate-limit refusal is the common one and it
            // says WHEN to try again, which is only useful next to the button
            // it is talking about.
            reportDialog.errorMessage = message;
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
                    color: Qt.rgba(Theme.warn.r, Theme.warn.g, Theme.warn.b, 0.15)
                    Icon {
                        anchors.centerIn: parent
                        name: "bolt"
                        size: 16
                        color: Theme.warn
                    }
                }
                Text {
                    Layout.fillWidth: true
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
                text: "This goes to the administrators of this server. They "
                    + "can read the message and decide what to do. Nothing "
                    + "changes right away, and "
                    + reportDialog.targetName
                    + " is not told that you reported them."
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
                        text: reportDialog.targetName
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.accent
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
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
                visible: reasonField.text.length > 700
                text: reasonField.text.length > 1024
                    ? "That is too long to send — shorten it a little."
                    : reasonField.text.length + " / 1024"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                color: reasonField.text.length > 1024 ? Theme.danger : Theme.fg3
                horizontalAlignment: Text.AlignRight
            }

            // ── The control that has an effect now ──
            ThemedCheckBox {
                id: alsoBlockCheck
                Layout.fillWidth: true
                visible: !reportDialog.alreadyBlocked
                enabled: !reportDialog.busy
                text: "Also block " + reportDialog.targetName
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
                text: reportDialog.errorMessage
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
                    enabled: !reportDialog.busy && reasonField.text.length <= 1024
                    implicitHeight: Theme.controlHeight.md
                    contentItem: Text {
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
