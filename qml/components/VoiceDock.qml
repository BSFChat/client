import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import BSFChat

// VoiceDock (SPEC §3.5) — persistent 64h call-controls bar anchored below
// the main content. Visible only while in a voice channel.
//
// Layout is a single RowLayout with a `Layout.fillWidth: true` spacer on
// both sides of the center cluster, which keeps it visually centered
// regardless of the other clusters' content width and — crucially — never
// overflows when the main column is narrow.
Rectangle {
    id: dock
    color: Theme.bg1
    implicitHeight: visible ? Theme.layout.voiceDockH : 0
    visible: serverManager.activeServer
             && serverManager.activeServer.inVoiceChannel

    // Top-edge divider.
    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 1
        color: Theme.line
    }

    // 40×40 dock button with a tintable SVG icon. Toggled state tints the
    // glyph red (mute/deafen engaged). `danger` variant is used for the
    // disconnect action — solid-fill red with an onAccent glyph.
    component DockButton: Rectangle {
        id: btn
        property string icon: ""
        property bool   toggled: false
        property bool   danger:  false
        property string tooltip: ""
        property bool   enabled2: true
        signal clicked()

        implicitWidth: 40
        implicitHeight: 40
        radius: Theme.r2
        color: danger     ? Theme.danger
             : toggled    ? Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.18)
             : hover.containsMouse && enabled2 ? Theme.bg3
             : Theme.bg2
        opacity: enabled2 ? 1.0 : 0.45
        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

        Icon {
            anchors.centerIn: parent
            name: btn.icon
            size: 18
            color: btn.danger  ? "white"
                 : btn.toggled ? Theme.danger
                 : Theme.fg1
        }

        MouseArea {
            id: hover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: btn.enabled2 ? Qt.PointingHandCursor : Qt.ArrowCursor
            enabled: btn.enabled2
            onClicked: btn.clicked()
        }

        ToolTip.visible: hover.containsMouse && tooltip.length > 0
        ToolTip.text: tooltip
        ToolTip.delay: 500
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.sp.s7
        anchors.rightMargin: Theme.sp.s7
        spacing: Theme.sp.s5

        // ─── Left cluster: self identity + connection line ──────────
        // Click-to-focus: tapping anywhere in this cluster swaps the main
        // area back to the VoiceRoom view (handy when the user wandered
        // into a text channel mid-call and wants to see the participant
        // grid again). r1 hover tint gives visual confirmation it's live.
        Rectangle {
            id: leftCluster
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: leftClusterRow.implicitWidth + Theme.sp.s3 * 2
            implicitHeight: leftClusterRow.implicitHeight + Theme.sp.s2 * 2
            radius: Theme.r1
            color: leftClusterHover.containsMouse
                   && !serverManager.activeServer.viewingVoiceRoom
                   ? Theme.bg2 : "transparent"
            Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

            MouseArea {
                id: leftClusterHover
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: !serverManager.activeServer.viewingVoiceRoom
                             ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: if (serverManager.activeServer)
                               serverManager.activeServer.showVoiceRoom()
            }

            RowLayout {
                id: leftClusterRow
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: Theme.sp.s3
                spacing: Theme.sp.s3

                Rectangle {
                    width: Theme.avatar.md; height: Theme.avatar.md
                    // Rounded-square to match every other avatar in the
                    // app (ServerRail / MemberList / UserSettings / profile
                    // card). We were still on the circular legacy here.
                    radius: Theme.r2
                    color: Theme.senderColor(serverManager.activeServer
                        ? (serverManager.activeServer.userId || "") : "")
                    Text {
                        anchors.centerIn: parent
                        text: {
                            if (!serverManager.activeServer) return "?";
                            var n = serverManager.activeServer.displayName
                                 || serverManager.activeServer.userId;
                            var stripped = n.replace(/^[^a-zA-Z0-9]+/, "");
                            return (stripped.length > 0
                                    ? stripped.charAt(0) : "?").toUpperCase();
                        }
                        font.family: Theme.fontSans
                        font.pixelSize: 14
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.onAccent
                    }
                }

                ColumnLayout {
                    spacing: 0
                    Layout.maximumWidth: 180

                    Text {
                        text: serverManager.activeServer
                              ? serverManager.activeServer.displayName : ""
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.base
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.fg0
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Text {
                        text: {
                            var s = serverManager.activeServer;
                            if (!s || !s.activeVoiceRoomId) return "";
                            var name = s.roomListModel
                                       ? s.roomListModel.roomDisplayName(s.activeVoiceRoomId)
                                       : s.activeVoiceRoomId;
                            return "Connected to #" + name;
                        }
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                        color: Theme.fg2
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }
            }
        }

        // Flexible spacer — pushes the center cluster toward true horizontal
        // centering as long as the member list (to our right) stays fixed.
        Item { Layout.fillWidth: true }

        // ─── Center cluster ─────────────────────────────────────────
        RowLayout {
            spacing: Theme.sp.s3
            Layout.alignment: Qt.AlignVCenter

            DockButton {
                icon: serverManager.activeServer
                      && serverManager.activeServer.voiceMuted ? "mic-off" : "mic"
                toggled: serverManager.activeServer
                         && serverManager.activeServer.voiceMuted
                tooltip: toggled ? "Unmute" : "Mute"
                onClicked: if (serverManager.activeServer)
                               serverManager.activeServer.toggleMute()
            }

            DockButton {
                icon: serverManager.activeServer
                      && serverManager.activeServer.voiceDeafened
                      ? "headphones-off" : "headphones"
                toggled: serverManager.activeServer
                         && serverManager.activeServer.voiceDeafened
                tooltip: toggled ? "Undeafen" : "Deafen"
                onClicked: if (serverManager.activeServer)
                               serverManager.activeServer.toggleDeafen()
            }

            DockButton {
                icon: "screen-share"
                tooltip: screenShare.active
                    ? "Stop sharing screen"
                    : "Share screen or window…"
                toggled: screenShare.active
                onClicked: {
                    if (screenShare.active) screenShare.stop();
                    else screenShare.showPicker();
                }
                // Surface capture errors as a toast (silent QScreenCapture
                // failures would otherwise just look like "the button did
                // nothing"). No action buttons — we don't want a click to
                // re-trigger any system-settings navigation.
                Connections {
                    target: screenShare
                    function onLastErrorChanged() {
                        var err = screenShare.lastError;
                        if (err && err.length > 0
                            && Window.window && Window.window.toastError) {
                            Window.window.toastError(err);
                        }
                    }
                }
            }

            DockButton {
                icon: "video"
                tooltip: "Video (coming soon)"
                enabled2: false
            }

            // Spacer before destructive action.
            Item { implicitWidth: Theme.sp.s3; implicitHeight: 1 }

            DockButton {
                icon: "phone-off"
                danger: true
                tooltip: "Disconnect"
                onClicked: if (serverManager.activeServer)
                               serverManager.activeServer.leaveVoiceChannel()
            }
        }

        Item { Layout.fillWidth: true }

        // ─── Right cluster: mic level meter ─────────────────────────
        RowLayout {
            spacing: Theme.sp.s1
            Layout.alignment: Qt.AlignVCenter

            Repeater {
                model: 6
                delegate: Rectangle {
                    required property int index
                    width: 3
                    height: 10 + index * 2
                    radius: 1.5
                    readonly property real level:
                        serverManager.activeServer
                        ? serverManager.activeServer.micLevel : 0
                    readonly property real threshold: (index + 1) / 7
                    color: {
                        if (serverManager.activeServer
                            && serverManager.activeServer.micSilent) return Theme.danger;
                        return level >= threshold ? Theme.accent : Theme.bg3;
                    }
                    Behavior on color { ColorAnimation { duration: 60 } }
                }
            }
        }
    }
}
