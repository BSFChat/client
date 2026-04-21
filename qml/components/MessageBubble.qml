import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
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
    // Left-click on an inline image asks MessageView to open the
    // in-app ImageViewer (zoomable / pannable lightbox). Middle-click
    // bypasses this and opens in the OS browser instead.
    signal imageOpenRequested(string url, string filename, real size)
    // Right-click context-menu action: redact (delete) the message.
    // Allowed for the sender or anyone with MANAGE_MESSAGES.
    signal deleteRequested(string eventId)

    // URLs to unfurl — extracted once per body change. Capped at 2 so
    // a spam-linked message doesn't explode into preview cards.
    readonly property var _previewUrls: {
        // Only unfurl plain-text messages. Attachments (image/file/
        // video) already carry their own preview chrome; a URL buried
        // in an image caption isn't worth fetching.
        if (bubble.msgtype !== "m.text") return [];
        if (!bubble.body) return [];
        // Match http(s) URLs anywhere in the body. Stops at whitespace
        // or closing punctuation commonly seen at the end of a prose
        // sentence (`.`, `)`, `]`, `!`, `?`, `,`, `"`, `'`).
        var re = /\bhttps?:\/\/[^\s<>"'()\[\]]+[^\s<>"'()\[\].,!?]/gi;
        var matches = bubble.body.match(re);
        if (!matches) return [];
        // Dedupe + cap.
        var seen = {};
        var out = [];
        for (var i = 0; i < matches.length && out.length < 2; i++) {
            var u = matches[i];
            if (seen[u]) continue;
            seen[u] = true;
            out.push(u);
        }
        return out;
    }

    implicitHeight: contentLayout.implicitHeight + (showSender ? Theme.sp.s7 : 2)

    // HoverHandler tracks hover across the whole bubble regardless of
    // child event consumption — the MouseArea below goes false the moment
    // the cursor lands on the TextEdit (which grabs mouse events for
    // selectByMouse). Using `bubbleHovered` as the single source of truth
    // for hover-sensitive UI keeps the action bar stable.
    HoverHandler {
        id: bubbleHoverHandler
    }
    readonly property bool bubbleHovered: bubbleHoverHandler.hovered

    // Right-click anywhere on the bubble pops the context menu. Using a
    // TapHandler instead of a MouseArea so it cooperates with the
    // TextEdit body's selectByMouse — children still handle their own
    // left-drag text selection, and we only grab the right button here.
    TapHandler {
        acceptedButtons: Qt.RightButton
        // popup() with no args uses the OS cursor position, which is
        // what we want for a right-click context menu. popup(pos)
        // expects coords in the Menu's parent item's frame, and mixing
        // that with scene-root coords from a TapHandler eventPoint put
        // the menu half a screen away from the actual click.
        onTapped: contextMenu.popup()
    }

    // True when we can show the "Delete" menu item. Either we sent the
    // message, or we have MANAGE_MESSAGES in the active room.
    readonly property bool canDelete: isOwnMessage
        || (serverManager.activeServer
            && serverManager.activeServer.canManageMessages(
                   serverManager.activeServer.activeRoomId))

    // Right-click context menu. Declared at the bubble root so the
    // position passed to popup() is in application coords and works
    // regardless of where in the bubble the user clicked.
    Menu {
        id: contextMenu

        background: Rectangle {
            color: Theme.bg1
            radius: Theme.r2
            border.color: Theme.line
            border.width: 1
            implicitWidth: 200
        }

        component CtxItem: MenuItem {
            id: ci
            // Collapse vertical footprint when hidden. Qt Controls Menu
            // doesn't always skip invisible MenuItems in its layout, so
            // an `Edit` item on someone else's message would leave a
            // blank 34px slot in the popup. Gating implicitHeight on
            // visibility is the canonical way to fold it cleanly.
            implicitHeight: visible ? 34 : 0
            height: implicitHeight
            property string iconName: ""
            property color labelColor: Theme.fg0
            property string shortcut: ""
            contentItem: RowLayout {
                spacing: Theme.sp.s3
                Icon {
                    name: ci.iconName
                    size: 14
                    color: !ci.enabled ? Theme.fg3
                         : ci.hovered ? ci.labelColor : Theme.fg2
                    Layout.leftMargin: Theme.sp.s3
                }
                Text {
                    text: ci.text
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    color: !ci.enabled ? Theme.fg3 : ci.labelColor
                    Layout.fillWidth: true
                    verticalAlignment: Text.AlignVCenter
                }
                Text {
                    text: ci.shortcut
                    visible: ci.shortcut.length > 0
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSize.xs
                    color: Theme.fg3
                    Layout.rightMargin: Theme.sp.s3
                }
            }
            background: Rectangle {
                color: ci.hovered && ci.enabled ? Theme.bg2 : "transparent"
                radius: Theme.r1
                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
            }
        }

        CtxItem {
            text: "Reply"
            iconName: "reply"
            onTriggered: bubble.replyRequested(bubble.eventId, bubble.body,
                                               bubble.senderDisplayName)
        }
        CtxItem {
            text: "Forward"
            iconName: "forward"
            onTriggered: bubble.forwardRequested(bubble.eventId, bubble.body,
                                                 bubble.senderDisplayName)
        }
        CtxItem {
            text: "Edit"
            iconName: "edit"
            visible: bubble.isOwnMessage
            onTriggered: bubble.editRequested(bubble.eventId, bubble.body)
        }

        MenuSeparator {
            contentItem: Rectangle {
                implicitWidth: 160
                implicitHeight: 1
                color: Theme.line
            }
        }

        CtxItem {
            text: "Copy text"
            iconName: "copy"
            onTriggered: {
                if (serverManager) serverManager.copyToClipboard(bubble.body);
            }
        }
        CtxItem {
            text: "Copy message link"
            iconName: "link"
            onTriggered: bubble.copyLinkRequested(bubble.eventId)
        }

        CtxItem {
            text: {
                var s = serverManager.activeServer;
                if (s && s.isEventPinned(s.activeRoomId, bubble.eventId))
                    return "Unpin message";
                return "Pin message";
            }
            iconName: "pin"
            enabled: {
                var s = serverManager.activeServer;
                if (!s) return false;
                if (s.permissionsGeneration < 0) return false;
                return s.canManageChannel(s.activeRoomId);
            }
            onTriggered: {
                var s = serverManager.activeServer;
                if (s) s.togglePinnedEvent(s.activeRoomId, bubble.eventId);
            }
        }

        MenuSeparator {
            visible: bubble.canDelete
            contentItem: Rectangle {
                implicitWidth: 160
                implicitHeight: 1
                color: Theme.line
            }
        }

        CtxItem {
            text: "Delete message"
            iconName: "x"
            labelColor: Theme.danger
            visible: bubble.canDelete
            onTriggered: bubble.deleteRequested(bubble.eventId)
        }
    }

    // User-scope context menu — popped by right-click on the sender
    // avatar or sender name. Distinct from the message-scope menu
    // (which is fired by right-click on the bubble body) so "reply"/
    // "delete" actions don't appear when the user meant to reach out
    // to the person. Mirrors the MemberList right-click vocabulary.
    Menu {
        id: userContextMenu
        readonly property bool isSelfUser: serverManager.activeServer
            && bubble.sender === serverManager.activeServer.userId
        readonly property bool canManageRoles: serverManager.activeServer
            && serverManager.activeServer.canManageRoles(
                   serverManager.activeServer.activeRoomId)

        background: Rectangle {
            color: Theme.bg1
            radius: Theme.r2
            border.color: Theme.line
            border.width: 1
            implicitWidth: 220
        }

        CtxItem {
            text: "View profile"
            iconName: "at"
            onTriggered: bubble.senderClicked(bubble.sender,
                                              bubble.senderDisplayName)
        }
        CtxItem {
            text: "Copy user ID"
            iconName: "copy"
            onTriggered: {
                if (serverManager) serverManager.copyToClipboard(bubble.sender);
            }
        }

        MenuSeparator {
            visible: userContextMenu.canManageRoles
            contentItem: Rectangle {
                implicitWidth: 180
                implicitHeight: 1
                color: Theme.line
            }
        }
        CtxItem {
            text: "Manage roles…"
            iconName: "shield"
            visible: userContextMenu.canManageRoles
            onTriggered: Window.window.openRoleAssignment(
                bubble.sender, bubble.senderDisplayName)
        }
    }

    Rectangle {
        id: hoverBg
        anchors.fill: parent
        anchors.leftMargin: -Theme.sp.s3
        anchors.rightMargin: -Theme.sp.s3
        color: bubble.bubbleHovered ? Qt.rgba(0, 0, 0, 0.06) : "transparent"
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
        // `bubbleHovered` (HoverHandler) stays true across the whole
        // bubble — including while the cursor is on the selectable
        // TextEdit, which would swallow hover from a plain MouseArea.
        // The per-button MouseAreas OR'd in keep the bar visible while
        // the cursor has travelled off the bubble onto a button itself.
        visible: bubble.msgtype === "m.text" && (bubble.bubbleHovered
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
                id: senderAvatar
                width: 40
                height: 40
                radius: 20
                color: Theme.senderColor(bubble.sender)
                scale: senderAvatarMouse.containsMouse ? 1.06 : 1.0
                Behavior on scale {
                    NumberAnimation { duration: Theme.motion.fastMs
                                      easing.type: Easing.BezierSpline
                                      easing.bezierCurve: Theme.motion.bezier }
                }

                Text {
                    anchors.centerIn: parent
                    // Strip leading non-alphanumerics (@, _, etc.) before
                    // picking the initial — otherwise "@josh" renders as
                    // "@" and the swatch looks like a typo.
                    text: {
                        var n = bubble.senderDisplayName || "?";
                        var s = n.replace(/^[^a-zA-Z0-9]+/, "");
                        return (s.length > 0 ? s.charAt(0) : "?").toUpperCase();
                    }
                    font.family: Theme.fontSans
                    font.pixelSize: 16
                    font.weight: Theme.fontWeight.semibold
                    color: Theme.onAccent
                }

                // Avatar click target. Left-click opens the profile card
                // (same as clicking the sender name next to it). Right-
                // click opens the user context menu. Matches MemberList.
                MouseArea {
                    id: senderAvatarMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: (m) => {
                        if (m.button === Qt.RightButton) {
                            userContextMenu.popup();
                        } else {
                            bubble.senderClicked(bubble.sender,
                                                 bubble.senderDisplayName);
                        }
                    }
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
                visible: bubble.bubbleHovered
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

                    // Same left/right routing as the avatar, so the name
                    // and the avatar feel like one user-target. The
                    // outer right-click TapHandler on the bubble root
                    // wins anywhere else and shows the message menu —
                    // this MouseArea steals right-clicks only over the
                    // sender name's actual glyph bounds.
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: (m) => {
                            if (m.button === Qt.RightButton) {
                                userContextMenu.popup();
                            } else {
                                bubble.senderClicked(bubble.sender,
                                                     bubble.senderDisplayName);
                            }
                        }
                    }
                }

                Text {
                    id: senderTimestamp
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

                    // Hover tooltip with the full absolute date + time
                    // (with seconds). "Today at 14:41" is great for
                    // scanning; a hover tooltip gives users the exact
                    // moment when they need it — referencing a specific
                    // message, cross-checking logs, etc.
                    HoverHandler { id: timestampHover }
                    ToolTip.visible: timestampHover.hovered
                    ToolTip.delay: 500
                    ToolTip.text: {
                        var d = new Date(bubble.timestamp);
                        return d.toLocaleString(Qt.locale(),
                            "dddd, MMMM d, yyyy 'at' h:mm:ss AP");
                    }
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

            // Inline video for m.video messages. Uses QtMultimedia
            // (already linked). Paused by default — Discord-style;
            // click the big play overlay to start. Transport bar
            // appears once playing, fades out while the cursor isn't
            // on the card so a muted video-in-background doesn't
            // clutter the message row.
            Loader {
                Layout.fillWidth: true
                Layout.preferredHeight: item ? item.implicitHeight : 0
                active: bubble.msgtype === "m.video" && bubble.mediaUrl !== ""
                sourceComponent: VideoPlayerCard {
                    source: bubble.mediaUrl
                    fileName: bubble.mediaFileName
                    fileSize: bubble.mediaFileSize
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

                            // Loading placeholder — subtle bg2 on the
                            // card, "Loading…" copy, accent progress bar
                            // along the bottom edge tracking real bytes
                            // (mediaImage.progress is 0..1).
                            Rectangle {
                                anchors.fill: parent
                                color: Theme.bg2
                                radius: Theme.r2
                                visible: mediaImage.status === Image.Loading

                                ColumnLayout {
                                    anchors.centerIn: parent
                                    spacing: Theme.sp.s2
                                    Icon {
                                        Layout.alignment: Qt.AlignHCenter
                                        name: "paperclip"
                                        size: 20
                                        color: Theme.fg3
                                        opacity: 0.6
                                    }
                                    Text {
                                        Layout.alignment: Qt.AlignHCenter
                                        text: "Loading…"
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.sm
                                        color: Theme.fg2
                                    }
                                }

                                Rectangle {
                                    anchors.bottom: parent.bottom
                                    anchors.left: parent.left
                                    height: 3
                                    width: parent.width * mediaImage.progress
                                    color: Theme.accent
                                    radius: 1
                                    Behavior on width {
                                        NumberAnimation { duration: 120 }
                                    }
                                }
                            }

                            // Error state — danger icon + explanatory copy
                            // + retry-in-browser hint. Matches the
                            // empty-state vocabulary used in the Bans /
                            // Members tabs for consistency.
                            Rectangle {
                                anchors.fill: parent
                                color: Theme.bg2
                                radius: Theme.r2
                                visible: mediaImage.status === Image.Error

                                ColumnLayout {
                                    anchors.centerIn: parent
                                    spacing: Theme.sp.s2
                                    Icon {
                                        Layout.alignment: Qt.AlignHCenter
                                        name: "eye"
                                        size: 22
                                        color: Theme.danger
                                    }
                                    Text {
                                        Layout.alignment: Qt.AlignHCenter
                                        text: "Couldn't load image"
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.sm
                                        font.weight: Theme.fontWeight.semibold
                                        color: Theme.fg0
                                    }
                                    Text {
                                        Layout.alignment: Qt.AlignHCenter
                                        text: "Middle-click to try in your browser"
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.xs
                                        color: Theme.fg3
                                    }
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                                onClicked: (mouse) => {
                                    // Left-click opens the in-app viewer
                                    // (zoom + pan). Middle-click is the
                                    // escape hatch to the OS browser.
                                    if (mouse.button === Qt.MiddleButton) {
                                        Qt.openUrlExternally(bubble.mediaUrl);
                                    } else {
                                        bubble.imageOpenRequested(
                                            bubble.mediaUrl,
                                            bubble.mediaFileName,
                                            bubble.mediaFileSize);
                                    }
                                }
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
                        id: fileCard
                        implicitHeight: fileRow.implicitHeight + Theme.sp.s7
                        implicitWidth: fileRow.implicitWidth + Theme.sp.s7 * 2
                        width: Math.min(360, implicitWidth)
                        // Subtle container — bg2 card with a line border,
                        // lifts to bg3 on hover. Keeps the file affordance
                        // distinct from the message body without competing
                        // with the rest of the chat for attention.
                        color: fileCardMouse.containsMouse ? Theme.bg3 : Theme.bg2
                        radius: Theme.r2
                        border.color: fileCardMouse.containsMouse ? Theme.fg3 : Theme.line
                        border.width: 1
                        Behavior on color       { ColorAnimation { duration: Theme.motion.fastMs } }
                        Behavior on border.color { ColorAnimation { duration: Theme.motion.fastMs } }

                        RowLayout {
                            id: fileRow
                            anchors.fill: parent
                            anchors.margins: Theme.sp.s4
                            spacing: Theme.sp.s4

                            // Icon tile — subtle accent-tinted swatch with
                            // a paperclip glyph, lifts to fully-accent on
                            // card hover to cue "click to open".
                            Rectangle {
                                Layout.preferredWidth: 40
                                Layout.preferredHeight: 40
                                radius: Theme.r2
                                color: fileCardMouse.containsMouse
                                    ? Theme.accent
                                    : Qt.rgba(Theme.accent.r, Theme.accent.g,
                                              Theme.accent.b, 0.16)
                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                Icon {
                                    anchors.centerIn: parent
                                    name: "paperclip"
                                    size: 18
                                    color: fileCardMouse.containsMouse
                                        ? Theme.onAccent : Theme.accent
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    text: bubble.mediaFileName
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.fontSize.md
                                    font.weight: Theme.fontWeight.semibold
                                    color: Theme.fg0
                                    elide: Text.ElideMiddle
                                    Layout.fillWidth: true
                                }
                                Text {
                                    text: bubble.mediaFileSize > 0
                                        ? formatFileSize(bubble.mediaFileSize)
                                          + " · click to open"
                                        : "File · click to open"
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.fontSize.sm
                                    color: Theme.fg3
                                }
                            }
                        }

                        MouseArea {
                            id: fileCardMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: Qt.openUrlExternally(bubble.mediaUrl)
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
                visible: bubble.msgtype === "m.text"
                      || (bubble.msgtype !== "m.image"
                          && bubble.msgtype !== "m.video"
                          && bubble.msgtype !== "m.audio"
                          && bubble.msgtype !== "m.file")
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

            // Link previews — OpenGraph-style unfurl cards for any
            // URLs in the body. Cap at 2 so a message full of links
            // doesn't produce a wall of previews. Each LinkPreview
            // self-hides if the target page has no usable metadata.
            Repeater {
                model: bubble._previewUrls
                delegate: LinkPreview {
                    Layout.topMargin: 4
                    url: modelData
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
                        id: chip
                        property var entry: modelData
                        property bool reacted: entry && entry.reacted === true
                        height: 22
                        width: chipRow.implicitWidth + 14
                        // r5 per SPEC — pill-ish rounded tag.
                        radius: Theme.r5
                        // Three-state fill: reacted (accent-tinted), hover
                        // (slightly brighter neutral), rest (bg3).
                        color: {
                            if (chip.reacted) {
                                return chipMouse.containsMouse
                                    ? Qt.rgba(Theme.accent.r, Theme.accent.g,
                                              Theme.accent.b, 0.32)
                                    : Qt.rgba(Theme.accent.r, Theme.accent.g,
                                              Theme.accent.b, 0.22);
                            }
                            return chipMouse.containsMouse ? Theme.bg4 : Theme.bg3;
                        }
                        border.width: 1
                        border.color: chip.reacted ? Theme.accent
                                                   : chipMouse.containsMouse
                                                     ? Theme.fg3
                                                     : Theme.line
                        Behavior on color       { ColorAnimation { duration: Theme.motion.fastMs } }
                        Behavior on border.color { ColorAnimation { duration: Theme.motion.fastMs } }

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
                                font.weight: chip.reacted
                                             ? Theme.fontWeight.semibold
                                             : Theme.fontWeight.medium
                                color: chip.reacted ? Theme.fg0 : Theme.fg2
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }

                        MouseArea {
                            id: chipMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (!entry) return;
                                bubble.reactionToggled(bubble.eventId, entry.emoji);
                            }
                        }

                        // Tooltip lists who reacted. Resolves each userId
                        // through the shared sender display-name map so
                        // the strings match what people see everywhere
                        // else. Our own reaction surfaces as "you" for
                        // extra self-recognition.
                        ToolTip.visible: chipMouse.containsMouse
                                        && chip.width > 0
                        ToolTip.delay: 500
                        ToolTip.text: {
                            if (!entry) return "";
                            var ids = entry.userIds || [];
                            if (ids.length === 0) return entry.emoji;
                            var names = [];
                            var ownId = serverManager.activeServer
                                        ? serverManager.activeServer.userId : "";
                            for (var i = 0; i < Math.min(ids.length, 6); ++i) {
                                if (ids[i] === ownId) {
                                    names.push("you");
                                } else if (serverManager.activeServer
                                           && serverManager.activeServer.memberListModel) {
                                    var n = serverManager.activeServer
                                        .memberListModel.displayNameForUser(ids[i]);
                                    names.push(n && n.length > 0 ? n : ids[i]);
                                } else {
                                    names.push(ids[i]);
                                }
                            }
                            var more = ids.length - names.length;
                            var list = names.join(", ");
                            if (more > 0) list += " and " + more + " more";
                            return list + " reacted with " + entry.emoji;
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
