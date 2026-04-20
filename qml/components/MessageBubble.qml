import QtQuick
import QtQuick.Controls
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
    // True while this bubble is the target of a reply-jump or message-link
    // navigation. MessageView sets it for ~1.5s so the user gets a visible
    // "here it is" pulse after the ListView centers on this row.
    property bool highlighted: false
    // Reply metadata — populated from the MessageModel when this message
    // carries an m.in_reply_to relation. Empty replyToEventId means not a reply.
    property string replyToEventId: ""
    property string replyToSender: ""
    property string replyPreview: ""
    // Aggregated reactions for this message. Each entry is
    // {emoji, count, reacted, eventIds}. Bound from model.reactions.
    property var reactions: []

    signal senderClicked(string userId, string displayName)
    // Emitted when the user clicks a reaction chip (toggle) or picks an
    // emoji from the popup. MessageView forwards to ServerConnection.
    signal reactionToggled(string eventId, string emoji)
    signal editRequested(string eventId, string currentBody)
    // Reply action on the hover bar. Payload mirrors what MessageInput
    // needs to populate its "Replying to…" banner.
    signal replyRequested(string eventId, string body, string senderDisplayName)
    // Forward action on the hover bar. MessageView opens the picker.
    signal forwardRequested(string eventId, string body, string senderDisplayName)
    // Clicking the reply preamble asks the view to scroll to the target.
    signal jumpToEvent(string eventId)
    // Copy-link button on the hover bar. MessageView builds the link
    // (via ServerConnection.messageLink) and stuffs it into the clipboard.
    signal copyLinkRequested(string eventId)
    // Clicking a `#channel` link in the rendered message body. MessageView
    // resolves the name to a roomId on the active server and switches.
    signal channelLinkClicked(string name)
    // Clicking a `bsfchat://message/...` link — originates from forwarded
    // message preambles or copy-pasted links. MessageView delegates to
    // ServerManager.openMessageLink.
    signal messageLinkClicked(string link)
    // Clicking a `bsfchat://user/<userId>` link — inline @mention anchor.
    // MessageView opens the profile card for that user.
    signal userLinkClicked(string userId, string displayName)

    implicitHeight: contentLayout.implicitHeight + (showSender ? Theme.sp.s7 : 2)

    Rectangle {
        id: hoverBg
        anchors.fill: parent
        anchors.leftMargin: -Theme.sp.s3
        anchors.rightMargin: -Theme.sp.s3
        color: bubbleHover.containsMouse ? Qt.rgba(0, 0, 0, 0.06) : "transparent"
        radius: Theme.r1
    }

    // Pulse overlay — fires when MessageView.highlightedEventId matches
    // this bubble. Fades in/out via Behavior for a smooth flash.
    Rectangle {
        id: highlightPulse
        anchors.fill: parent
        anchors.leftMargin: -Theme.sp.s3
        anchors.rightMargin: -Theme.sp.s3
        radius: Theme.r1
        color: Theme.accent
        opacity: bubble.highlighted ? 0.18 : 0.0
        z: 0
        Behavior on opacity { NumberAnimation { duration: 300 } }
    }

    MouseArea {
        id: bubbleHover
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
    }

    // Hover action bar — visible on mouse-over. Contains reply/forward for
    // any text message and edit for own messages. Three compact icon
    // buttons sit in a row; each highlights its glyph on hover.
    Row {
        id: hoverActions
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: 0
        anchors.rightMargin: Theme.sp.s1
        spacing: 0
        // Visibility must stay true while the cursor is on any child
        // button — otherwise moving from the bubble onto a button toggles
        // bubbleHover.containsMouse off (child MouseAreas consume hover)
        // and the bar flickers. OR the per-button hover states in.
        visible: bubble.msgtype === "m.text" && (bubbleHover.containsMouse
                 || replyMouse.containsMouse
                 || forwardMouse.containsMouse
                 || copyLinkMouse.containsMouse
                 || reactMouse.containsMouse
                 || editMouse.containsMouse)
        z: 1

        // Reply
        Rectangle {
            width: 28; height: 24
            radius: Theme.r1
            color: replyMouse.containsMouse ? Theme.bg3 : Theme.bg1
            border.color: Theme.line
            border.width: 1
            Icon {
                anchors.centerIn: parent
                name: "reply"
                size: 14
                color: replyMouse.containsMouse ? Theme.accent : Theme.fg1
            }
            MouseArea {
                id: replyMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: bubble.replyRequested(bubble.eventId, bubble.body,
                                                 bubble.senderDisplayName)
            }
        }

        // Forward
        Rectangle {
            width: 28; height: 24
            radius: Theme.r1
            color: forwardMouse.containsMouse ? Theme.bg3 : Theme.bg1
            border.color: Theme.line
            border.width: 1
            Icon {
                anchors.centerIn: parent
                name: "forward"
                size: 14
                color: forwardMouse.containsMouse ? Theme.accent : Theme.fg1
            }
            MouseArea {
                id: forwardMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: bubble.forwardRequested(bubble.eventId, bubble.body,
                                                   bubble.senderDisplayName)
            }
        }

        // Copy-link — produces a bsfchat://message/... URL on the clipboard
        // so the user can paste it into another channel and let recipients
        // jump straight to the original.
        Rectangle {
            width: 28; height: 24
            radius: Theme.r1
            color: copyLinkMouse.containsMouse ? Theme.bg3 : Theme.bg1
            border.color: Theme.line
            border.width: 1
            Icon {
                anchors.centerIn: parent
                name: "link"
                size: 14
                color: copyLinkMouse.containsMouse ? Theme.accent : Theme.fg1
            }
            MouseArea {
                id: copyLinkMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: bubble.copyLinkRequested(bubble.eventId)
            }
        }

        // Quick-react — opens the EmojiPicker anchored to this button.
        // Selecting an emoji fires reactionToggled and closes the popup.
        Rectangle {
            id: reactButton
            width: 28; height: 24
            radius: Theme.r1
            color: reactMouse.containsMouse || reactPicker.visible
                   ? Theme.bg3 : Theme.bg1
            border.color: Theme.line
            border.width: 1
            Icon {
                anchors.centerIn: parent
                name: "smile-plus"
                size: 14
                color: reactMouse.containsMouse || reactPicker.visible
                       ? Theme.accent : Theme.fg1
            }
            MouseArea {
                id: reactMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (reactPicker.visible) reactPicker.close();
                    else reactPicker.open();
                }
            }

            EmojiPicker {
                id: reactPicker
                // Anchor above-right so the picker doesn't cover the bubble.
                y: -height - Theme.sp.s1
                x: -width + 28
                onEmojiSelected: function(emoji) {
                    bubble.reactionToggled(bubble.eventId, emoji);
                    reactPicker.close();
                }
            }
        }

        // Edit — only on your own messages.
        Rectangle {
            width: 28; height: 24
            radius: Theme.r1
            color: editMouse.containsMouse ? Theme.bg3 : Theme.bg1
            border.color: Theme.line
            border.width: 1
            visible: bubble.isOwnMessage
            Icon {
                anchors.centerIn: parent
                name: "edit"
                size: 14
                color: editMouse.containsMouse ? Theme.accent : Theme.fg1
            }
            MouseArea {
                id: editMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: bubble.editRequested(bubble.eventId, bubble.body)
            }
        }
    }

    RowLayout {
        id: contentLayout
        anchors.fill: parent
        anchors.topMargin: showSender ? Theme.sp.s3 : 0
        spacing: Theme.sp.s7

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
                    color: Theme.onAccent
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
                color: Theme.fg2
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
                spacing: Theme.sp.s3

                Text {
                    text: bubble.senderDisplayName
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackTight.md
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
                    font.family: Theme.fontMono
                    font.pixelSize: 11
                    color: Theme.fg3
                }
            }

            // Reply preamble — shown when this message is a reply. Accent
            // bar on the left, quoted sender + short preview. Clicking
            // asks the MessageView to scroll to the original.
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? replyPreambleRow.implicitHeight + 4 : 0
                visible: bubble.replyToEventId !== ""

                RowLayout {
                    id: replyPreambleRow
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    spacing: Theme.sp.s1

                    Rectangle {
                        Layout.preferredWidth: 3
                        Layout.fillHeight: true
                        Layout.minimumHeight: 16
                        color: Theme.accent
                        radius: 1.5
                    }

                    // Small curved reply-arrow to cue "this is a reply"
                    // without having to read the @ prefix first.
                    Icon {
                        name: "reply"
                        size: 12
                        color: Theme.accent
                    }

                    Text {
                        text: bubble.replyToSender !== ""
                              ? bubble.replyToSender : "unknown"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.accent
                    }

                    Text {
                        text: bubble.replyPreview !== ""
                              ? bubble.replyPreview
                              : "(message unavailable)"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                        color: Theme.fg3
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: bubble.jumpToEvent(bubble.replyToEventId)
                }
            }

            // Inline image for m.image messages
            Loader {
                Layout.fillWidth: true
                Layout.preferredHeight: item ? item.implicitHeight : 0
                active: bubble.msgtype === "m.image" && bubble.mediaUrl !== ""
                sourceComponent: Component {
                    ColumnLayout {
                        spacing: Theme.sp.s1

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
                            radius: Theme.r2
                            color: Theme.bg3
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
                                color: Theme.bg3
                                radius: Theme.r2
                                visible: mediaImage.status === Image.Loading

                                Text {
                                    anchors.centerIn: parent
                                    text: "Loading image..."
                                    font.pixelSize: Theme.fontSize.sm
                                    color: Theme.fg2
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
                                color: Theme.bg3
                                radius: Theme.r2
                                visible: mediaImage.status === Image.Error

                                Text {
                                    anchors.centerIn: parent
                                    text: "Failed to load image"
                                    font.pixelSize: Theme.fontSize.sm
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
                            spacing: Theme.sp.s1
                            visible: bubble.mediaFileName !== ""

                            Text {
                                text: bubble.mediaFileName
                                font.pixelSize: Theme.fontSize.sm
                                color: Theme.fg2
                                elide: Text.ElideMiddle
                                Layout.maximumWidth: 300
                            }

                            Text {
                                text: bubble.mediaFileSize > 0 ? formatFileSize(bubble.mediaFileSize) : ""
                                font.pixelSize: Theme.fontSize.sm
                                color: Theme.fg2
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
                        implicitHeight: fileRow.implicitHeight + Theme.sp.s7
                        implicitWidth: fileRow.implicitWidth + Theme.sp.s7 * 2
                        width: Math.min(350, implicitWidth)
                        color: Theme.bg3
                        radius: Theme.r2

                        RowLayout {
                            id: fileRow
                            anchors.fill: parent
                            anchors.margins: Theme.sp.s3
                            spacing: Theme.sp.s3

                            // File icon
                            Rectangle {
                                Layout.preferredWidth: 36
                                Layout.preferredHeight: 36
                                radius: Theme.r1
                                color: Theme.accent

                                Text {
                                    anchors.centerIn: parent
                                    text: "\u2B07"
                                    font.pixelSize: 18
                                    color: Theme.onAccent
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2

                                Text {
                                    text: bubble.mediaFileName
                                    font.pixelSize: Theme.fontSize.md
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
                                    font.pixelSize: Theme.fontSize.sm
                                    color: Theme.fg2
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
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.base
                color: Theme.fg1
                readOnly: true
                selectByMouse: true
                selectedTextColor: "white"
                selectionColor: Theme.accent
                persistentSelection: false
                onLinkActivated: (link) => {
                    // Intercept internal scheme; let anything else open in
                    // the system browser as before.
                    if (link.indexOf("bsfchat://channel/") === 0) {
                        bubble.channelLinkClicked(
                            link.substring("bsfchat://channel/".length));
                    } else if (link.indexOf("bsfchat://message/") === 0) {
                        bubble.messageLinkClicked(link);
                    } else if (link.indexOf("bsfchat://user/") === 0) {
                        // Strip the scheme prefix; the anchor's text was
                        // the display name. We don't have it here, so pass
                        // the user id for both and let the profile card
                        // resolve the name.
                        var uid = decodeURIComponent(
                            link.substring("bsfchat://user/".length));
                        bubble.userLinkClicked(uid, uid);
                    } else {
                        Qt.openUrlExternally(link);
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.NoButton
                    cursorShape: parent.hoveredLink ? Qt.PointingHandCursor : Qt.IBeamCursor
                }
            }

            // Reaction chips: one per unique emoji + a trailing "+" chip that
            // The first reaction is added exclusively from the hover bar's
            // smiley button; this row only appears once at least one
            // reaction exists, so a message with none stays visually quiet.
            Flow {
                id: reactionsFlow
                Layout.fillWidth: true
                Layout.topMargin: (bubble.reactions && bubble.reactions.length > 0) ? 4 : 0
                spacing: 4
                visible: bubble.reactions && bubble.reactions.length > 0

                Repeater {
                    model: bubble.reactions
                    delegate: Rectangle {
                        property var entry: modelData
                        height: 22
                        width: chipRow.implicitWidth + 14
                        // r5 per SPEC — pill-ish rounded tag.
                        radius: Theme.r5
                        color: entry && entry.reacted
                               ? Qt.rgba(Theme.accent.r, Theme.accent.g,
                                         Theme.accent.b, 0.22)
                               : Theme.bg3
                        border.width: 1
                        border.color: entry && entry.reacted
                                      ? Theme.accent
                                      : Theme.line

                        Row {
                            id: chipRow
                            anchors.centerIn: parent
                            spacing: 5
                            Text {
                                text: entry ? entry.emoji : ""
                                font.family: Theme.fontSans
                                font.pixelSize: 13
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: entry ? entry.count : 0
                                font.family: Theme.fontMono
                                font.pixelSize: 11
                                font.weight: entry && entry.reacted
                                             ? Theme.fontWeight.semibold
                                             : Theme.fontWeight.medium
                                color: entry && entry.reacted
                                       ? Theme.accent
                                       : Theme.fg2
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (!entry) return;
                                bubble.reactionToggled(bubble.eventId, entry.emoji);
                            }
                        }
                    }
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
