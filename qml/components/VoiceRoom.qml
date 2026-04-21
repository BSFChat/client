import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import BSFChat

// VoiceRoom (SPEC §3.3) — the "hero" main-content view when the user is in
// a voice channel. Header + participant grid. Each tile carries an avatar,
// name, peer-state status line, and (for self) a speaking-ring glow driven
// by the outgoing mic level.
//
// When not in a voice channel this component isn't shown; main.qml swaps
// between MessageView and VoiceRoom based on activeServer.inVoiceChannel.
Rectangle {
    id: room
    color: Theme.bg0

    // Header (SPEC §3.3, 56h) — channel name, member count, crypto badge,
    // latency chip. Invite/more buttons are placeholders until the feature
    // set around voice grows.
    Rectangle {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 56
        color: Theme.bg0

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.sp.s8
            anchors.rightMargin: Theme.sp.s8
            spacing: Theme.sp.s5

            Icon {
                name: "volume"
                size: 20
                color: Theme.accent
                Layout.alignment: Qt.AlignVCenter
            }

            Text {
                text: {
                    var s = serverManager.activeServer;
                    if (!s || !s.activeVoiceRoomId) return "";
                    return s.roomListModel
                           ? s.roomListModel.roomDisplayName(s.activeVoiceRoomId)
                           : s.activeVoiceRoomId;
                }
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xl
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackTight.xl
                color: Theme.fg0
                Layout.alignment: Qt.AlignVCenter
            }

            Text {
                text: {
                    var s = serverManager.activeServer;
                    if (!s || !s.voiceMembers) return "";
                    return s.voiceMembers.length + " in call";
                }
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
                Layout.alignment: Qt.AlignVCenter
            }

            Item { Layout.fillWidth: true }

            // Transport badge. We're not running SRTP — audio Opus frames
            // ride an SCTP data channel over libdatachannel's DTLS 1.2
            // handshake. So the truthful label is DTLS · SCTP (encryption
            // terminates at the peer, not end-to-end). Upgrade to SRTP /
            // MLS etc. later means changing the text here too.
            Rectangle {
                implicitWidth: cryptoText.implicitWidth + Theme.sp.s4
                implicitHeight: 22
                radius: Theme.r1
                color: Theme.accentGlow
                Layout.alignment: Qt.AlignVCenter

                Text {
                    id: cryptoText
                    anchors.centerIn: parent
                    text: "DTLS \u00B7 SCTP"
                    font.family: Theme.fontMono
                    font.pixelSize: 11
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackWide.sm
                    color: Theme.accent
                }
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.line
        }
    }

    // Participant grid — fills the rest of the main area under the header.
    // Auto-columns based on tile+gap widths; tiles wrap to the next row
    // when the main column narrows.
    ScrollView {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        ScrollBar.vertical: ThemedScrollBar {}
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        Item {
            // Width binds to the ScrollView's viewport; height grows with
            // content. Padding via an inner margin item keeps tiles off
            // the scrollbar without needing to reach into the ScrollBar.
            width: parent.width
            implicitHeight: gridWrapper.implicitHeight + Theme.sp.s8 * 2

            Item {
                id: gridWrapper
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: Theme.sp.s8
                anchors.rightMargin: Theme.sp.s8
                anchors.topMargin: Theme.sp.s8
                anchors.top: parent.top
                implicitHeight: grid.implicitHeight

                Grid {
                    id: grid
                    width: parent.width
                    columnSpacing: Theme.layout.participantGap
                    rowSpacing: Theme.layout.participantGap
                    // Pick as many columns as cleanly fit; at least 1 so a
                    // narrow main column still shows a row of one.
                    columns: Math.max(1,
                        Math.floor((width + Theme.layout.participantGap)
                                   / (Theme.layout.participantTileW
                                      + Theme.layout.participantGap)))

                    Repeater {
                        model: serverManager.activeServer
                               ? serverManager.activeServer.voiceMembers
                               : []
                        delegate: ParticipantTile {
                            required property var modelData
                            member: modelData
                        }
                    }
                }
            }
        }
    }

    // Empty state — shown only when the voice channel has no peers yet.
    // Centered icon + headline + subtext, same vocabulary as the ban /
    // member empty-states in ServerSettings so the app reads consistently.
    ColumnLayout {
        anchors.centerIn: parent
        anchors.verticalCenterOffset: 28  // lift slightly above geometric center
        spacing: Theme.sp.s4
        width: 360
        visible: {
            var s = serverManager.activeServer;
            return !s || !s.voiceMembers || s.voiceMembers.length === 0;
        }

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 72
            Layout.preferredHeight: 72
            radius: Theme.r3
            color: Theme.bg1
            border.color: Theme.line
            border.width: 1

            Icon {
                anchors.centerIn: parent
                name: "volume"
                size: 28
                color: Theme.accent
            }

            // Subtle pulse ring so the empty-state doesn't feel static —
            // reads as "the call is live, waiting."
            Rectangle {
                anchors.centerIn: parent
                width: parent.width
                height: parent.height
                radius: parent.radius
                color: "transparent"
                border.color: Theme.accent
                border.width: 2
                opacity: 0
                SequentialAnimation on opacity {
                    loops: Animation.Infinite
                    running: parent.parent.visible
                    NumberAnimation { to: 0.35; duration: 800; easing.type: Easing.OutQuad }
                    NumberAnimation { to: 0;    duration: 900; easing.type: Easing.InQuad  }
                    PauseAnimation { duration: 300 }
                }
                SequentialAnimation on scale {
                    loops: Animation.Infinite
                    running: parent.parent.visible
                    NumberAnimation { to: 1.25; duration: 1700; easing.type: Easing.OutQuad }
                    PropertyAction   { value: 1.0 }
                }
            }
        }

        Text {
            Layout.alignment: Qt.AlignHCenter
            text: "Waiting for others"
            color: Theme.fg0
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.lg
            font.weight: Theme.fontWeight.semibold
            font.letterSpacing: Theme.trackTight.lg
        }

        Text {
            Layout.alignment: Qt.AlignHCenter
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: "You're in the channel. Others who join will show up here, and the controls below stay available."
            color: Theme.fg2
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.sm
            wrapMode: Text.WordWrap
        }
    }
}
