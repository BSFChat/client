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

        // Header: green dot + "Voice Connected". The dot pulses brighter
        // while the user is actively transmitting — linear on micLevel
        // (0..1, smoothed and log-compressed server-side) so it reacts to
        // real speech without flickering on room tone.
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            readonly property real micLevel: serverManager.activeServer
                ? serverManager.activeServer.micLevel : 0.0
            readonly property bool transmitting: micLevel > 0.1

            Rectangle {
                width: 8; height: 8; radius: 4
                // At rest: default success green. While transmitting:
                // brighter, fully saturated green with a subtle outer halo.
                color: parent.transmitting
                    ? Qt.lighter(Theme.success, 1.0 + parent.micLevel * 0.5)
                    : Theme.success
                opacity: parent.transmitting
                    ? 0.7 + parent.micLevel * 0.3
                    : 0.55
                Behavior on color { ColorAnimation { duration: 80 } }
                Behavior on opacity { NumberAnimation { duration: 80 } }

                // Subtle outer glow when speaking.
                Rectangle {
                    anchors.centerIn: parent
                    width: 8 + parent.parent.micLevel * 10
                    height: width
                    radius: width / 2
                    color: "transparent"
                    border.color: Theme.success
                    border.width: 1
                    opacity: parent.parent.transmitting ? 0.35 : 0.0
                    visible: opacity > 0.01
                    Behavior on opacity { NumberAnimation { duration: 80 } }
                }
            }

            Text {
                text: parent.transmitting ? "Transmitting" : "Voice Connected"
                font.pixelSize: Theme.fontSizeSmall
                font.bold: true
                color: parent.transmitting
                    ? Qt.lighter(Theme.success, 1.1)
                    : Theme.success
                Behavior on color { ColorAnimation { duration: 80 } }
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

        // Mic-silent warning — shown when the local device has been capturing
        // zero-level audio for ~3 seconds. Usually a device-selection or
        // permission problem; the user should check Client Settings → Audio.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? micSilentCol.implicitHeight + 10 : 0
            visible: serverManager.activeServer
                     && serverManager.activeServer.micSilent
            radius: Theme.radiusSmall
            color: Qt.rgba(1, 0.3, 0.3, 0.15)
            border.color: Theme.danger; border.width: 1

            ColumnLayout {
                id: micSilentCol
                anchors.fill: parent
                anchors.margins: 5
                spacing: 2
                Text {
                    text: "Your mic is silent"
                    color: Theme.danger
                    font.pixelSize: Theme.fontSizeSmall
                    font.bold: true
                }
                Text {
                    Layout.fillWidth: true
                    text: "Others can't hear you. Check your input device in Client Settings → Audio."
                    color: Theme.textMuted
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                }
            }
        }

        // Member list — each member shows a colored dot for their
        // peer-connection state so you know who's actually reachable.
        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 100)
            clip: true
            interactive: contentHeight > 100
            spacing: 2
            model: serverManager.activeServer ? serverManager.activeServer.voiceMembers : []

            delegate: Item {
                width: ListView.view ? ListView.view.width : 100
                height: 24

                readonly property string peerState: modelData.peerState || "new"
                readonly property color stateColor:
                    peerState === "connected"    ? Theme.success :
                    peerState === "connecting"   ? "#fee75c" :
                    peerState === "failed"       ? Theme.danger :
                    peerState === "disconnected" ? Theme.danger :
                    Theme.textMuted // "new"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 2
                    spacing: 6

                    // Peer-connection indicator dot
                    Rectangle {
                        width: 6; height: 6; radius: 3
                        color: parent.parent.stateColor
                        Layout.alignment: Qt.AlignVCenter
                    }

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
                        color: parent.parent.peerState === "connected"
                               ? Theme.textSecondary
                               : parent.parent.stateColor
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
