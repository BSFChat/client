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
    Flickable {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.sp.s8
        contentHeight: grid.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Grid {
            id: grid
            width: parent.width
            columnSpacing: Theme.layout.participantGap
            rowSpacing: Theme.layout.participantGap
            // Pick as many columns as cleanly fit; at least 1 so a narrow
            // main column still shows a row of one.
            columns: Math.max(1,
                Math.floor((width + Theme.layout.participantGap)
                           / (Theme.layout.participantTileW + Theme.layout.participantGap)))

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

        // Empty state — visible only when the voice channel has no peers
        // yet. Resolves a brief "call UI with no tiles" gap.
        Text {
            anchors.centerIn: parent
            visible: {
                var s = serverManager.activeServer;
                return !s || !s.voiceMembers || s.voiceMembers.length === 0;
            }
            text: "Waiting for others to join…"
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.md
            color: Theme.fg3
        }
    }
}
