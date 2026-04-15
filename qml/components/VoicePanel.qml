import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

Rectangle {
    id: root
    implicitHeight: voicePanelColumn.implicitHeight + Theme.spacingNormal * 2
    color: Qt.darker(Theme.bgDark, 1.15)

    Rectangle {
        anchors.top: parent.top
        width: parent.width
        height: 1
        color: Theme.bgDarkest
    }

    ColumnLayout {
        id: voicePanelColumn
        anchors.fill: parent
        anchors.margins: Theme.spacingNormal
        spacing: Theme.spacingSmall

        // Header: green dot + "Voice Connected"
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Rectangle {
                width: 8; height: 8; radius: 4
                color: Theme.success
            }

            Text {
                text: "Voice Connected"
                font.pixelSize: Theme.fontSizeSmall
                font.bold: true
                color: Theme.success
            }
        }

        // Channel name
        Text {
            text: {
                if (!serverManager.activeServer) return "";
                var rid = serverManager.activeServer.activeVoiceRoomId;
                if (!rid) return "";
                return serverManager.activeServer.roomListModel.roomDisplayName(rid);
            }
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.textMuted
            elide: Text.ElideRight
            Layout.fillWidth: true
        }

        // Member list
        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 80)
            clip: true
            interactive: contentHeight > 80
            spacing: 2
            model: serverManager.activeServer ? serverManager.activeServer.voiceMembers : []

            delegate: Item {
                width: ListView.view ? ListView.view.width : 100
                height: 24

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 2
                    spacing: 6

                    Rectangle {
                        width: 18; height: 18; radius: 9
                        color: Theme.accent

                        Text {
                            anchors.centerIn: parent
                            text: {
                                var uid = modelData.user_id || "";
                                return uid.length > 1 ? uid.charAt(1).toUpperCase() : "?";
                            }
                            font.pixelSize: 9
                            font.bold: true
                            color: "white"
                        }
                    }

                    Text {
                        text: modelData.user_id || "Unknown"
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.textSecondary
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }

                    // Mute indicator
                    Canvas {
                        visible: modelData.muted === true
                        width: 12; height: 12
                        onPaint: {
                            var ctx = getContext("2d");
                            ctx.clearRect(0, 0, width, height);
                            ctx.strokeStyle = Theme.danger.toString();
                            ctx.lineWidth = 1.2;
                            ctx.beginPath();
                            ctx.roundedRect(3.5, 0.5, 5, 7, 2.5, 2.5);
                            ctx.stroke();
                            ctx.beginPath();
                            ctx.moveTo(0, 0); ctx.lineTo(12, 12);
                            ctx.lineWidth = 1.5;
                            ctx.stroke();
                        }
                    }
                }
            }
        }

        // Controls row
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall

            // Mute button
            Rectangle {
                Layout.preferredWidth: Theme.iconButtonSize
                Layout.preferredHeight: Theme.iconButtonSize
                radius: Theme.radiusSmall
                color: serverManager.activeServer && serverManager.activeServer.voiceMuted
                       ? Theme.danger
                       : (muteArea.containsMouse ? Theme.bgLight : Theme.bgMedium)

                Canvas {
                    anchors.centerIn: parent
                    width: 16; height: 16
                    property bool isMuted: serverManager.activeServer ? serverManager.activeServer.voiceMuted : false
                    onIsMutedChanged: requestPaint()
                    onPaint: {
                        var ctx = getContext("2d");
                        ctx.clearRect(0, 0, width, height);
                        ctx.strokeStyle = "white";
                        ctx.fillStyle = "white";
                        ctx.lineWidth = 1.5;
                        // Mic body (rounded rect)
                        ctx.beginPath();
                        ctx.roundedRect(5, 1, 6, 9, 3, 3);
                        ctx.stroke();
                        // Mic stand (arc + line)
                        ctx.beginPath();
                        ctx.arc(8, 10, 5, Math.PI, 0, false);
                        ctx.stroke();
                        ctx.beginPath();
                        ctx.moveTo(8, 15); ctx.lineTo(8, 16);
                        ctx.stroke();
                        // Strikethrough when muted
                        if (isMuted) {
                            ctx.strokeStyle = "white";
                            ctx.lineWidth = 2;
                            ctx.beginPath();
                            ctx.moveTo(1, 1); ctx.lineTo(15, 15);
                            ctx.stroke();
                        }
                    }
                }

                MouseArea {
                    id: muteArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (serverManager.activeServer) serverManager.activeServer.toggleMute()
                }

                ToolTip.visible: muteArea.containsMouse
                ToolTip.text: serverManager.activeServer && serverManager.activeServer.voiceMuted ? "Unmute" : "Mute"
                ToolTip.delay: 500
            }

            // Deafen button
            Rectangle {
                Layout.preferredWidth: Theme.iconButtonSize
                Layout.preferredHeight: Theme.iconButtonSize
                radius: Theme.radiusSmall
                color: serverManager.activeServer && serverManager.activeServer.voiceDeafened
                       ? Theme.danger
                       : (deafenArea.containsMouse ? Theme.bgLight : Theme.bgMedium)

                Canvas {
                    anchors.centerIn: parent
                    width: 16; height: 16
                    property bool isDeafened: serverManager.activeServer ? serverManager.activeServer.voiceDeafened : false
                    onIsDeafenedChanged: requestPaint()
                    onPaint: {
                        var ctx = getContext("2d");
                        ctx.clearRect(0, 0, width, height);
                        ctx.strokeStyle = "white";
                        ctx.lineWidth = 1.5;
                        // Headband (arc)
                        ctx.beginPath();
                        ctx.arc(8, 7, 6, Math.PI, 0, false);
                        ctx.stroke();
                        // Left ear cup
                        ctx.fillStyle = "white";
                        ctx.beginPath();
                        ctx.roundedRect(1, 7, 4, 7, 1, 1);
                        ctx.fill();
                        // Right ear cup
                        ctx.beginPath();
                        ctx.roundedRect(11, 7, 4, 7, 1, 1);
                        ctx.fill();
                        // Strikethrough when deafened
                        if (isDeafened) {
                            ctx.strokeStyle = "white";
                            ctx.lineWidth = 2;
                            ctx.beginPath();
                            ctx.moveTo(1, 1); ctx.lineTo(15, 15);
                            ctx.stroke();
                        }
                    }
                }

                MouseArea {
                    id: deafenArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (serverManager.activeServer) serverManager.activeServer.toggleDeafen()
                }

                ToolTip.visible: deafenArea.containsMouse
                ToolTip.text: serverManager.activeServer && serverManager.activeServer.voiceDeafened ? "Undeafen" : "Deafen"
                ToolTip.delay: 500
            }

            Item { Layout.fillWidth: true }

            // Disconnect button
            Rectangle {
                Layout.preferredWidth: 70
                Layout.preferredHeight: Theme.iconButtonSize
                radius: Theme.radiusSmall
                color: disconnectArea.containsMouse ? Qt.lighter(Theme.danger, 1.1) : Theme.danger

                Text {
                    anchors.centerIn: parent
                    text: "Leave"
                    font.pixelSize: Theme.fontSizeSmall
                    font.bold: true
                    color: "white"
                }

                MouseArea {
                    id: disconnectArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (serverManager.activeServer) serverManager.activeServer.leaveVoiceChannel()
                }
            }
        }
    }
}
