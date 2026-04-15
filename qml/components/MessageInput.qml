import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.platform as Platform
import BSFChat

Rectangle {
    id: inputRoot
    color: Theme.bgLight
    radius: Theme.radiusNormal
    height: Math.max(38, Math.min(inputArea.implicitHeight + 16, 200))

    property string roomName: ""
    property bool uploading: false

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.spacingNormal
        anchors.rightMargin: Theme.spacingNormal
        spacing: Theme.spacingSmall

        // Attachment button
        Rectangle {
            Layout.preferredWidth: 28
            Layout.preferredHeight: 28
            Layout.alignment: Qt.AlignVCenter
            radius: Theme.radiusSmall
            color: attachHover.containsMouse ? Theme.bgMedium : "transparent"
            opacity: inputRoot.uploading ? 0.4 : 1.0

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
                placeholderText: inputRoot.uploading ? "Uploading..." : "Message #" + inputRoot.roomName
                placeholderTextColor: Theme.textMuted
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeNormal
                wrapMode: TextEdit.Wrap
                background: null
                selectByMouse: true
                verticalAlignment: TextEdit.AlignVCenter
                topPadding: 8
                bottomPadding: 8
                enabled: !inputRoot.uploading

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
        if (serverManager.activeServer) {
            serverManager.activeServer.sendMessage(text);
            inputArea.text = "";
        }
    }
}
