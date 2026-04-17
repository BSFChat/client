import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

Rectangle {
    color: Theme.bgMedium

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Sync error banner
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 28 : 0
            color: {
                if (!serverManager.activeServer) return "transparent";
                if (serverManager.activeServer.connectionStatus === 2) return Theme.warning;
                if (serverManager.activeServer.connectionStatus === 0) return Theme.danger;
                return "transparent";
            }
            visible: serverManager.activeServer !== null && serverManager.activeServer.connectionStatus !== 1

            Text {
                anchors.centerIn: parent
                text: {
                    if (!serverManager.activeServer) return "";
                    if (serverManager.activeServer.connectionStatus === 2) return "Reconnecting...";
                    if (serverManager.activeServer.connectionStatus === 0) return "Disconnected";
                    return "";
                }
                font.pixelSize: Theme.fontSizeSmall
                font.bold: true
                color: {
                    if (!serverManager.activeServer) return Theme.textPrimary;
                    if (serverManager.activeServer.connectionStatus === 2) return "#1e1f22";
                    return "white";
                }
            }

            Behavior on Layout.preferredHeight {
                NumberAnimation { duration: 200 }
            }
        }

        // Header bar
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            color: Theme.bgMedium

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacingLarge
                anchors.rightMargin: Theme.spacingLarge
                spacing: Theme.spacingNormal

                Text {
                    text: "#"
                    font.pixelSize: 20
                    color: Theme.textMuted
                    visible: serverManager.activeServer !== null && serverManager.activeServer.activeRoomId !== ""
                }

                Text {
                    text: serverManager.activeServer ? serverManager.activeServer.activeRoomName : ""
                    font.pixelSize: Theme.fontSizeLarge
                    font.bold: true
                    color: Theme.textPrimary
                }

                // Topic separator
                Rectangle {
                    width: 1
                    height: 20
                    color: Theme.bgLight
                    visible: topicText.text !== ""
                }

                Text {
                    id: topicText
                    text: serverManager.activeServer ? serverManager.activeServer.activeRoomTopic : ""
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.textMuted
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    visible: text !== ""
                }

                Item { Layout.fillWidth: true; visible: topicText.text === "" }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.bgDarkest
            }
        }

        // Messages list
        ListView {
            id: messageListView
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: Theme.spacingLarge
            Layout.rightMargin: Theme.spacingLarge
            clip: true
            verticalLayoutDirection: ListView.TopToBottom
            spacing: 2

            model: serverManager.activeServer ? serverManager.activeServer.messageModel : null

            // Track whether the user is near the bottom so we know whether
            // to auto-scroll on new messages. Imperative (not a binding) to
            // avoid the transient-false glitch where contentHeight grows
            // before contentY catches up during an append.
            property bool atBottom: true
            property bool initialLoad: true

            onModelChanged: {
                initialLoad = true;
                atBottom = true;
            }

            onContentYChanged: {
                if (!initialLoad) {
                    atBottom = (contentY + height >= contentHeight - 200);
                }
            }

            onContentHeightChanged: {
                // Also recompute here so "contentHeight grew while I was
                // at the end but contentY didn't move" still counts.
                if (!initialLoad) {
                    atBottom = (contentY + height >= contentHeight - 200);
                }
                if (initialLoad || atBottom) {
                    Qt.callLater(positionViewAtEnd);
                }
            }

            onCountChanged: {
                if (initialLoad || atBottom) {
                    Qt.callLater(positionViewAtEnd);
                    if (initialLoad) {
                        scrollTimer.restart();
                    }
                }
            }

            // After initial load, keep forcing scroll for a couple seconds
            // to handle async image loading
            Timer {
                id: scrollTimer
                interval: 2000
                onTriggered: {
                    messageListView.positionViewAtEnd();
                    messageListView.initialLoad = false;
                }
            }

            // Date separator helper
            function formatDateSeparator(timestamp) {
                var date = new Date(timestamp);
                var today = new Date();
                var yesterday = new Date();
                yesterday.setDate(yesterday.getDate() - 1);
                if (date.toDateString() === today.toDateString()) return "Today";
                if (date.toDateString() === yesterday.toDateString()) return "Yesterday";
                return date.toLocaleDateString(Qt.locale(), "MMMM d, yyyy");
            }

            delegate: Column {
                width: messageListView.width
                spacing: 0

                // Date separator
                Item {
                    width: parent.width
                    height: visible ? 40 : 0
                    visible: model.showDateSeparator

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: 1
                        color: Theme.bgLight
                    }

                    Rectangle {
                        anchors.centerIn: parent
                        width: dateSepText.implicitWidth + 24
                        height: 22
                        color: Theme.bgMedium
                        radius: Theme.radiusLarge

                        Text {
                            id: dateSepText
                            anchors.centerIn: parent
                            text: messageListView.formatDateSeparator(model.timestamp)
                            font.pixelSize: Theme.fontSizeSmall
                            font.bold: true
                            color: Theme.textMuted
                        }
                    }
                }

                MessageBubble {
                    width: parent.width
                    eventId: model.eventId
                    sender: model.sender
                    senderDisplayName: model.senderDisplayName
                    body: model.body
                    formattedBody: model.formattedBody
                    timestamp: model.timestamp
                    msgtype: model.msgtype
                    mediaUrl: model.mediaUrl || ""
                    mediaFileName: model.mediaFileName || ""
                    mediaFileSize: model.mediaFileSize || 0
                    isOwnMessage: model.isOwnMessage
                    showSender: model.showSender
                    edited: model.edited || false

                    onSenderClicked: (userId, displayName) => {
                        messageProfileCard.userId = userId;
                        messageProfileCard.profileDisplayName = displayName;
                        messageProfileCard.open();
                    }
                    onEditRequested: (targetId, currentBody) => {
                        messageInput.beginEditing(targetId, currentBody);
                    }
                }
            }

            // Scroll-to-bottom button
            Rectangle {
                id: scrollToBottomBtn
                anchors.bottom: parent.bottom
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottomMargin: 8
                width: 36
                height: 36
                radius: 18
                color: Theme.bgLight
                visible: !messageListView.atBottom && messageListView.count > 0
                opacity: 0.9

                Text {
                    anchors.centerIn: parent
                    text: "\u2193"
                    font.pixelSize: 18
                    color: Theme.textPrimary
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: messageListView.positionViewAtEnd()
                }
            }

            // Empty state
            Text {
                anchors.centerIn: parent
                visible: !serverManager.activeServer || serverManager.activeServer.activeRoomId === "" || messageListView.count === 0
                text: {
                    if (!serverManager.activeServer)
                        return "Select a server to start chatting"
                    if (serverManager.activeServer.activeRoomId === "")
                        return "Select a channel to start chatting"
                    return "No messages yet. Say hello!"
                }
                font.pixelSize: Theme.fontSizeLarge
                color: Theme.textMuted
            }
        }

        // Typing indicator
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacingLarge
            Layout.preferredHeight: visible ? 20 : 0
            visible: serverManager.activeServer && serverManager.activeServer.typingDisplay !== ""
            text: serverManager.activeServer ? serverManager.activeServer.typingDisplay : ""
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.textMuted

            Behavior on Layout.preferredHeight {
                NumberAnimation { duration: 100 }
            }
        }

        // Message input
        MessageInput {
            id: messageInput
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacingLarge
            Layout.rightMargin: Theme.spacingLarge
            Layout.topMargin: Theme.spacingNormal
            Layout.bottomMargin: Theme.spacingLarge
            Layout.minimumHeight: 44
            visible: serverManager.activeServer !== null && serverManager.activeServer.activeRoomId !== ""
            roomName: serverManager.activeServer ? serverManager.activeServer.activeRoomName : ""
        }
    }

    // Profile card for clicking on sender names
    UserProfileCard {
        id: messageProfileCard
        parent: Overlay.overlay
    }
}
