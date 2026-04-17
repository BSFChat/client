import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.platform as Platform
import BSFChat

// MessageInput is an implicitHeight-driven Rectangle so it can grow the
// vertical banner strip (when editing) without forcing the parent to
// re-layout around a fixed height.
Rectangle {
    id: inputRoot
    color: Theme.bgLight
    radius: Theme.radiusNormal
    implicitHeight: editingHeader.visible
        ? editingHeader.height + inputCore.implicitHeight + 8
        : inputCore.implicitHeight
    height: implicitHeight

    property string roomName: ""
    property string activeRoomId: serverManager.activeServer ? serverManager.activeServer.activeRoomId : ""
    property bool uploading: false

    // Editing state. When `editingEventId` is non-empty, pressing Enter
    // sends an edit event referencing that event_id rather than a new
    // m.room.message. Called from MessageView → MessageBubble.
    property string editingEventId: ""
    property string editingOriginalBody: ""
    function beginEditing(eventId, currentBody) {
        editingEventId = eventId;
        editingOriginalBody = currentBody;
        inputArea.text = currentBody;
        inputArea.forceActiveFocus();
        inputArea.cursorPosition = currentBody.length;
    }
    function cancelEditing() {
        editingEventId = "";
        editingOriginalBody = "";
        inputArea.text = "";
    }

    // Permission-derived UX state. Using permissionsGeneration as a real
    // dependency (integer, read and compared) makes these bindings reactive
    // across QML's AOT-compiled path; the bare `serverRoles` touch I tried
    // earlier got dead-code-eliminated.
    property int _permGen: serverManager.activeServer ? serverManager.activeServer.permissionsGeneration : 0
    property bool canSend: {
        if (!serverManager.activeServer) return true;
        return _permGen >= 0 && serverManager.activeServer.canSend(activeRoomId);
    }
    property bool canAttach: {
        if (!serverManager.activeServer) return true;
        return _permGen >= 0 && serverManager.activeServer.canAttach(activeRoomId);
    }
    property int slowmodeSeconds: {
        if (!serverManager.activeServer) return 0;
        return _permGen >= 0 ? serverManager.activeServer.channelSlowmode(activeRoomId) : 0;
    }
    // Client-side slowmode tracker. Server is still authoritative.
    property double lastSentAt: 0
    property int _slowmodeTick: 0 // bumped by the timer to force re-eval
    readonly property int slowmodeRemaining: {
        _slowmodeTick; // dependency
        if (slowmodeSeconds <= 0 || lastSentAt === 0) return 0;
        var elapsed = (Date.now() - lastSentAt) / 1000;
        var left = slowmodeSeconds - elapsed;
        return left > 0 ? Math.ceil(left) : 0;
    }

    Timer {
        running: inputRoot.slowmodeSeconds > 0 && inputRoot.lastSentAt > 0
        interval: 500
        repeat: true
        onTriggered: inputRoot._slowmodeTick++
    }

    // Banner shown above the composer while editing an existing message.
    // Occupies the top of the implicit height when visible.
    Rectangle {
        id: editingHeader
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: visible ? 28 : 0
        color: Theme.bgDarkest
        radius: Theme.radiusNormal
        visible: inputRoot.editingEventId !== ""
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.spacingNormal
            anchors.rightMargin: Theme.spacingNormal
            spacing: Theme.spacingSmall
            Text {
                text: "Editing message"
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
                font.bold: true
            }
            Text {
                text: "— press Esc to cancel"
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
                Layout.fillWidth: true
            }
            Text {
                text: "\u2715" // ✕
                color: cancelMouse.containsMouse ? Theme.textPrimary : Theme.textMuted
                font.pixelSize: 14
                MouseArea {
                    id: cancelMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: inputRoot.cancelEditing()
                }
            }
        }
    }

    RowLayout {
        id: inputCore
        anchors.top: editingHeader.visible ? editingHeader.bottom : parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.topMargin: editingHeader.visible ? 4 : 0
        anchors.leftMargin: Theme.spacingNormal
        anchors.rightMargin: Theme.spacingNormal
        spacing: Theme.spacingSmall

        // Attachment button — hidden if user lacks ATTACH_FILES.
        Rectangle {
            Layout.preferredWidth: 28
            Layout.preferredHeight: 28
            Layout.alignment: Qt.AlignVCenter
            radius: Theme.radiusSmall
            color: attachHover.containsMouse ? Theme.bgMedium : "transparent"
            opacity: inputRoot.uploading ? 0.4 : 1.0
            visible: inputRoot.canAttach

            Text {
                anchors.centerIn: parent
                text: "+"
                font.pixelSize: 18
                font.bold: true
                color: Theme.textMuted
            }

            MouseArea {
                id: attachHover
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                enabled: !inputRoot.uploading
                onClicked: fileDialog.open()
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            TextArea {
                id: inputArea
                placeholderText: {
                    if (!inputRoot.canSend) return "You don't have permission to send here";
                    if (inputRoot.slowmodeRemaining > 0)
                        return "Slowmode — " + inputRoot.slowmodeRemaining + "s";
                    if (inputRoot.uploading) return "Uploading...";
                    return "Message #" + inputRoot.roomName;
                }
                placeholderTextColor: Theme.textMuted
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeNormal
                wrapMode: TextEdit.Wrap
                background: null
                selectByMouse: true
                verticalAlignment: TextEdit.AlignVCenter
                topPadding: 8
                bottomPadding: 8
                enabled: !inputRoot.uploading && inputRoot.canSend && inputRoot.slowmodeRemaining === 0

                onTextChanged: {
                    if (serverManager.activeServer && text.trim().length > 0) {
                        serverManager.activeServer.sendTypingNotification();
                    }
                }

                Keys.onReturnPressed: (event) => {
                    if (event.modifiers & Qt.ShiftModifier) {
                        event.accepted = false; // Allow newline
                    } else {
                        sendCurrentMessage();
                        event.accepted = true;
                    }
                }
                Keys.onEscapePressed: (event) => {
                    if (inputRoot.editingEventId !== "") {
                        inputRoot.cancelEditing();
                        event.accepted = true;
                    }
                }
            }
        }

        // Emoji button
        Rectangle {
            Layout.preferredWidth: 28
            Layout.preferredHeight: 28
            Layout.alignment: Qt.AlignVCenter
            radius: Theme.radiusSmall
            color: emojiHover.containsMouse || emojiPopup.visible ? Theme.bgMedium : "transparent"

            Text {
                anchors.centerIn: parent
                text: "\u{1F642}"
                font.pixelSize: 18
                opacity: emojiHover.containsMouse || emojiPopup.visible ? 1.0 : 0.7
            }

            MouseArea {
                id: emojiHover
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (emojiPopup.visible) {
                        emojiPopup.close();
                    } else {
                        emojiPopup.open();
                    }
                }
            }

            EmojiPicker {
                id: emojiPopup
                y: -height - Theme.spacingSmall
                x: -width + 28

                onEmojiSelected: function(emoji) {
                    inputArea.insert(inputArea.cursorPosition, emoji);
                    emojiPopup.close();
                    inputArea.forceActiveFocus();
                }
            }
        }

        // Upload progress indicator
        Rectangle {
            Layout.preferredWidth: 28
            Layout.preferredHeight: 28
            Layout.alignment: Qt.AlignVCenter
            radius: 14
            color: Theme.accent
            visible: inputRoot.uploading

            // Simple spinning indicator
            Text {
                anchors.centerIn: parent
                text: "\u21BB"
                font.pixelSize: 16
                color: "white"

                RotationAnimation on rotation {
                    from: 0
                    to: 360
                    duration: 1000
                    loops: Animation.Infinite
                    running: inputRoot.uploading
                }
            }
        }

        // Send button
        Rectangle {
            Layout.preferredWidth: 28
            Layout.preferredHeight: 28
            Layout.alignment: Qt.AlignVCenter
            radius: Theme.radiusSmall
            color: inputArea.text.trim().length > 0 ? Theme.accent : "transparent"
            visible: inputArea.text.trim().length > 0 && !inputRoot.uploading

            Text {
                anchors.centerIn: parent
                text: "\u279C"
                font.pixelSize: 16
                color: "white"
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: sendCurrentMessage()
            }
        }
    }

    Platform.FileDialog {
        id: fileDialog
        title: "Select a file to upload"
        nameFilters: ["All files (*)"]
        onAccepted: {
            if (serverManager.activeServer) {
                serverManager.activeServer.sendMediaMessage(fileDialog.file.toString());
                inputRoot.uploading = true;
            }
        }
    }

    // Listen for upload completion to reset uploading state
    Connections {
        target: serverManager.activeServer ? serverManager.activeServer : null

        function onMediaSendCompleted() {
            inputRoot.uploading = false;
        }

        function onMediaSendFailed(error) {
            inputRoot.uploading = false;
            console.warn("Media upload failed:", error);
        }
    }

    function sendCurrentMessage() {
        var text = inputArea.text.trim();
        if (text.length === 0) return;
        if (!serverManager.activeServer) return;

        if (inputRoot.editingEventId !== "") {
            // Edit path — server still gates on sender match. Don't apply
            // slowmode / canSend checks to edits; they're intentionally
            // allowed during cooldown (you're refining, not flooding).
            if (text === inputRoot.editingOriginalBody) {
                inputRoot.cancelEditing();
                return;
            }
            serverManager.activeServer.editMessage(inputRoot.editingEventId, text);
            inputRoot.cancelEditing();
            return;
        }

        if (!inputRoot.canSend || inputRoot.slowmodeRemaining > 0) return;
        serverManager.activeServer.sendMessage(text);
        inputArea.text = "";
        inputRoot.lastSentAt = Date.now();
    }
}
