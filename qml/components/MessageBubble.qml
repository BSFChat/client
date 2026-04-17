import QtQuick
import QtQuick.Layouts
import BSFChat

Item {
    id: bubble

    property string eventId
    property string sender
    property string senderDisplayName
    property string body
    property string formattedBody
    property real timestamp
    property string msgtype: "m.text"
    property string mediaUrl: ""
    property string mediaFileName: ""
    property real mediaFileSize: 0
    property bool isOwnMessage
    property bool showSender
    property bool edited: false

    signal senderClicked(string userId, string displayName)
    signal editRequested(string eventId, string currentBody)

    implicitHeight: contentLayout.implicitHeight + (showSender ? Theme.spacingLarge : 2)

    Rectangle {
        id: hoverBg
        anchors.fill: parent
        anchors.leftMargin: -Theme.spacingNormal
        anchors.rightMargin: -Theme.spacingNormal
        color: bubbleHover.containsMouse ? Qt.rgba(0, 0, 0, 0.06) : "transparent"
        radius: Theme.radiusSmall
    }

    MouseArea {
        id: bubbleHover
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
    }

    // Hover action bar — visible on mouse-over for the user's own messages.
    // Right-aligned pencil icon; clicking it emits editRequested upwards so
    // the MessageInput can enter edit mode for this event.
    Rectangle {
        id: hoverActions
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: -4
        anchors.rightMargin: Theme.spacingSmall
        width: 28; height: 24
        radius: Theme.radiusSmall
        color: Theme.bgDark
        border.color: Theme.bgLight
        border.width: 1
        visible: bubble.isOwnMessage && bubbleHover.containsMouse
                 && bubble.msgtype === "m.text"
        z: 1

        Text {
            anchors.centerIn: parent
            text: "\u270E" // ✎ pencil
            font.pixelSize: 14
            color: editMouse.containsMouse ? Theme.accent : Theme.textSecondary
        }
        MouseArea {
            id: editMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: bubble.editRequested(bubble.eventId, bubble.body)
        }
    }

    RowLayout {
        id: contentLayout
        anchors.fill: parent
        anchors.topMargin: showSender ? Theme.spacingNormal : 0
        spacing: Theme.spacingLarge

        // Avatar area (shown when showSender is true)
        Item {
            Layout.preferredWidth: 40
            Layout.preferredHeight: 40
            Layout.alignment: Qt.AlignTop
            visible: showSender

            Rectangle {
                width: 40
                height: 40
                radius: 20
                color: Theme.senderColor(bubble.sender)

                Text {
                    anchors.centerIn: parent
                    text: bubble.senderDisplayName.charAt(0).toUpperCase()
                    font.pixelSize: 16
                    font.bold: true
                    color: "white"
                }
            }
        }

        // Compact timestamp for grouped messages (visible on hover)
        Item {
            Layout.preferredWidth: 40
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: 2
            visible: !showSender

            Text {
                anchors.centerIn: parent
                text: {
                    var date = new Date(bubble.timestamp);
                    return date.toLocaleTimeString(Qt.locale(), "HH:mm");
                }
                font.pixelSize: 10
                color: Theme.textMuted
                visible: bubbleHover.containsMouse
            }
        }

        // Message content
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            // Sender name and timestamp
            RowLayout {
                visible: showSender
                spacing: Theme.spacingNormal

                Text {
                    text: bubble.senderDisplayName
                    font.pixelSize: Theme.fontSizeNormal
                    font.bold: true
                    color: Theme.senderColor(bubble.sender)

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            bubble.senderClicked(bubble.sender, bubble.senderDisplayName);
                        }
                    }
                }

                Text {
                    text: {
                        var date = new Date(bubble.timestamp);
                        var today = new Date();
                        var yesterday = new Date();
                        yesterday.setDate(yesterday.getDate() - 1);
                        var timeStr = date.toLocaleTimeString(Qt.locale(), "HH:mm");
                        if (date.toDateString() === today.toDateString()) {
                            return "Today at " + timeStr;
                        }
                        if (date.toDateString() === yesterday.toDateString()) {
                            return "Yesterday at " + timeStr;
                        }
                        return date.toLocaleDateString(Qt.locale(), "MM/dd/yyyy") + " " + timeStr;
                    }
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.textMuted
                }
            }

            // Inline image for m.image messages
            Loader {
                Layout.fillWidth: true
                Layout.preferredHeight: item ? item.implicitHeight : 0
                active: bubble.msgtype === "m.image" && bubble.mediaUrl !== ""
                sourceComponent: Component {
                    ColumnLayout {
                        spacing: Theme.spacingSmall

                        Rectangle {
                            Layout.preferredWidth: Math.min(400, mediaImage.sourceSize.width > 0 ? mediaImage.sourceSize.width : 400)
                            Layout.preferredHeight: {
                                if (mediaImage.sourceSize.width > 0 && mediaImage.sourceSize.height > 0) {
                                    var scale = Math.min(400 / mediaImage.sourceSize.width, 300 / mediaImage.sourceSize.height, 1.0);
                                    return mediaImage.sourceSize.height * scale;
                                }
                                return 200;
                            }
                            Layout.maximumWidth: 400
                            Layout.maximumHeight: 300
                            radius: Theme.radiusNormal
                            color: Theme.bgLight
                            clip: true

                            Image {
                                id: mediaImage
                                anchors.fill: parent
                                source: bubble.mediaUrl
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                                cache: true
                            }

                            // Loading placeholder
                            Rectangle {
                                anchors.fill: parent
                                color: Theme.bgLight
                                radius: Theme.radiusNormal
                                visible: mediaImage.status === Image.Loading

                                Text {
                                    anchors.centerIn: parent
                                    text: "Loading image..."
                                    font.pixelSize: Theme.fontSizeSmall
                                    color: Theme.textMuted
                                }

                                // Simple loading bar
                                Rectangle {
                                    anchors.bottom: parent.bottom
                                    anchors.left: parent.left
                                    height: 3
                                    width: parent.width * mediaImage.progress
                                    color: Theme.accent
                                    radius: 1
                                }
                            }

                            // Error state
                            Rectangle {
                                anchors.fill: parent
                                color: Theme.bgLight
                                radius: Theme.radiusNormal
                                visible: mediaImage.status === Image.Error

                                Text {
                                    anchors.centerIn: parent
                                    text: "Failed to load image"
                                    font.pixelSize: Theme.fontSizeSmall
                                    color: Theme.danger
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: Qt.openUrlExternally(bubble.mediaUrl)
                            }
                        }

                        // Filename and size below image
                        RowLayout {
                            spacing: Theme.spacingSmall
                            visible: bubble.mediaFileName !== ""

                            Text {
                                text: bubble.mediaFileName
                                font.pixelSize: Theme.fontSizeSmall
                                color: Theme.textMuted
                                elide: Text.ElideMiddle
                                Layout.maximumWidth: 300
                            }

                            Text {
                                text: bubble.mediaFileSize > 0 ? formatFileSize(bubble.mediaFileSize) : ""
                                font.pixelSize: Theme.fontSizeSmall
                                color: Theme.textMuted
                                visible: text !== ""
                            }
                        }
                    }
                }
            }

            // File attachment for m.file messages
            Loader {
                Layout.fillWidth: true
                Layout.preferredHeight: item ? item.implicitHeight : 0
                active: bubble.msgtype === "m.file" && bubble.mediaUrl !== ""
                sourceComponent: Component {
                    Rectangle {
                        implicitHeight: fileRow.implicitHeight + Theme.spacingLarge
                        implicitWidth: fileRow.implicitWidth + Theme.spacingLarge * 2
                        width: Math.min(350, implicitWidth)
                        color: Theme.bgLight
                        radius: Theme.radiusNormal

                        RowLayout {
                            id: fileRow
                            anchors.fill: parent
                            anchors.margins: Theme.spacingNormal
                            spacing: Theme.spacingNormal

                            // File icon
                            Rectangle {
                                Layout.preferredWidth: 36
                                Layout.preferredHeight: 36
                                radius: Theme.radiusSmall
                                color: Theme.accent

                                Text {
                                    anchors.centerIn: parent
                                    text: "\u2B07"
                                    font.pixelSize: 18
                                    color: "white"
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2

                                Text {
                                    text: bubble.mediaFileName
                                    font.pixelSize: Theme.fontSizeNormal
                                    color: Theme.accent
                                    elide: Text.ElideMiddle
                                    Layout.fillWidth: true

                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: Qt.openUrlExternally(bubble.mediaUrl)
                                    }
                                }

                                Text {
                                    text: bubble.mediaFileSize > 0 ? formatFileSize(bubble.mediaFileSize) : "File"
                                    font.pixelSize: Theme.fontSizeSmall
                                    color: Theme.textMuted
                                }
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: Qt.openUrlExternally(bubble.mediaUrl)
                            z: -1
                        }
                    }
                }
            }

            // Message body (only for text messages or as caption).
            // TextEdit instead of Text so the content is selectable and
            // copyable via the native shortcuts (⌘C on macOS, Ctrl+C elsewhere).
            // The " (edited)" suffix is appended right into the rich text
            // so it sits inline at the end of the message — same pattern
            // Discord uses.
            TextEdit {
                Layout.fillWidth: true
                visible: bubble.msgtype === "m.text" || (bubble.msgtype !== "m.image" && bubble.msgtype !== "m.file")
                text: {
                    var base = bubble.formattedBody !== "" ? bubble.formattedBody : bubble.body;
                    if (!bubble.edited) return base;
                    // RichText mode lets us style the badge; PlainText mode
                    // appends raw text. Promote to RichText when editing.
                    var suffix = '<span style="color:#8e9297;font-size:small"> (edited)</span>';
                    if (bubble.formattedBody !== "") return base + suffix;
                    // Escape plain body minimally so angle brackets don't
                    // get interpreted by RichText.
                    var escaped = base.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
                    return escaped + suffix;
                }
                textFormat: (bubble.formattedBody !== "" || bubble.edited)
                            ? TextEdit.RichText : TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                font.pixelSize: Theme.fontSizeNormal
                color: Theme.textPrimary
                readOnly: true
                selectByMouse: true
                selectedTextColor: "white"
                selectionColor: Theme.accent
                persistentSelection: false
                onLinkActivated: (link) => Qt.openUrlExternally(link)
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.NoButton
                    cursorShape: parent.hoveredLink ? Qt.PointingHandCursor : Qt.IBeamCursor
                }
            }
        }
    }

    function formatFileSize(bytes) {
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB";
        if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + " MB";
        return (bytes / (1024 * 1024 * 1024)).toFixed(1) + " GB";
    }
}
