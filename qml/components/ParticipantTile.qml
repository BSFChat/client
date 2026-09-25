import QtQuick
import QtQuick.Layouts
import BSFChat

// Single tile in VoiceRoom's participant grid (SPEC §3.3).
//
// Fixed-size by `Theme.layout.participantTile{W,H}`. bg2 surface with a
// subtle `line` border at rest; when the local mic detects voice above
// the silence floor the border swaps to `accent` and a speaking ring
// pulses AROUND THE AVATAR (not around the whole tile — an outer halo
// would clip against the containing Flickable).
//
// Layout is reserved so state transitions don't shift content: the
// status line below the name always takes its full height, and the
// avatar's outer ring sits OUTSIDE the tile's content box but inside
// the margins, so turning it on/off doesn't jostle the layout.
Rectangle {
    id: tile
    property var  member: ({})
    readonly property string userId:   member ? (member.user_id || "") : ""
    readonly property string dispName: member ? (member.displayName || userId) : ""
    readonly property string peerState: member ? (member.peerState || "new") : "new"
    // "direct" / "relayed" / "" — see ServerConnection::buildVoiceMembers and
    // src/voice/IpPrivacy.h. Empty means ICE has not selected a pair yet, which
    // is a real answer and not a missing one.
    readonly property string connectionPath: member ? (member.connectionPath || "") : ""
    readonly property bool   isSelf: serverManager.activeServer
                                     && userId === serverManager.activeServer.userId
    // Self = our mic level (we know authoritatively). Remote = smoothed
    // output from the Opus decoder, bumped via peerLevelChanged.
    property int _levelGen: 0
    Connections {
        target: serverManager.activeServer
        ignoreUnknownSignals: true
        function onPeerLevelChanged(uid) {
            if (uid === tile.userId) tile._levelGen++;
        }
    }
    readonly property real   level: {
        if (isSelf) {
            return serverManager.activeServer
                ? serverManager.activeServer.micLevel : 0;
        }
        _levelGen;
        return serverManager.activeServer
            ? serverManager.activeServer.peerLevel(userId) : 0;
    }
    readonly property bool   speaking: level > 0.04
    // Muted state. For ourselves, authoritative from the connection;
    // for remote peers, we trust the per-member `muted` flag broadcast
    // over the voice signalling channel.
    readonly property bool   muted:    isSelf
                                       ? (serverManager.activeServer
                                          && serverManager.activeServer.voiceMuted)
                                       : (member && member.muted === true)
    readonly property bool   deafened: isSelf
                                       ? (serverManager.activeServer
                                          && serverManager.activeServer.voiceDeafened)
                                       : (member && member.deafened === true)

    implicitWidth:  Theme.layout.participantTileW
    implicitHeight: Theme.layout.participantTileH
    radius:         Theme.layout.participantRadius
    color:          Theme.bg2
    // Keep border.width CONSTANT at 1 so the content box doesn't resize
    // with speaking state. Only the color changes — still legible, no
    // layout churn.
    border.color:   speaking ? Theme.accent : Theme.line
    border.width:   1
    clip:           true    // guard against any child overshoot

    Behavior on border.color { ColorAnimation { duration: Theme.motion.fastMs } }

    // NO VIDEO HERE. A camera feed used to render inside this 220×180
    // tile, which is what the owner was complaining about: turning your
    // webcam on put your face in a thumbnail in a grid of avatars. Every
    // live video surface — screen shares and cameras alike, local and
    // remote — now belongs to VoiceRoom's stage and its bottom strip
    // (VideoFeedTile.qml), and the moment any of them exists this grid
    // is not on screen at all (VoiceRoom.isSharing). So a tile here is
    // always an audio-only participant, and giving it a VideoOutput
    // would only mean a second sink attached to the same stream,
    // composited behind a hidden view.
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.sp.s5
        spacing: Theme.sp.s3

        // Top spacer.
        Item { Layout.fillHeight: true }

        // Avatar with speaking ring — both centered in a fixed-size Item
        // so the layout doesn't reflow when the ring comes on/off.
        Item {
            Layout.preferredWidth: Theme.avatar.xl + 16
            Layout.preferredHeight: Theme.avatar.xl + 16
            Layout.alignment: Qt.AlignHCenter

            // Speaking ring — lives INSIDE the reserved 96×96 box so it
            // never sticks into the tile's edges. Border width stays small
            // so the tile border doesn't need to grow to contain it.
            Rectangle {
                anchors.centerIn: parent
                readonly property int ringSize: Theme.avatar.xl + 10
                width: ringSize
                height: ringSize
                radius: ringSize / 2
                color: "transparent"
                border.color: Theme.accent
                border.width: tile.speaking ? 2 + tile.level * 3 : 0
                opacity: tile.speaking ? 1.0 : 0.0
                Behavior on border.width { NumberAnimation { duration: 80 } }
                Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }
            }

            // Avatar tile itself, centered. Rounded square, matching every
            // other avatar in the tree — r3 at this size for the same reason
            // UserSettings.qml uses it on its 64px tile.
            Rectangle {
                anchors.centerIn: parent
                width: Theme.avatar.xl
                height: Theme.avatar.xl
                radius: Theme.r3
                color: Theme.senderColor(tile.userId)

                Text {
                    anchors.centerIn: parent
                    text: {
                        var n = tile.dispName;
                        var stripped = n.replace(/^[^a-zA-Z0-9]+/, "");
                        return (stripped.length > 0
                                ? stripped.charAt(0)
                                : "?").toUpperCase();
                    }
                    font.family: Theme.fontSans
                    font.pixelSize: 28
                    font.weight: Theme.fontWeight.semibold
                    color: Theme.onAccent
                }

                // Muted indicator — bottom-right of the avatar. Same 22px
                // treatment whether it's self (authoritative) or remote
                // (trusted via signalling). Deafened supersedes muted
                // since a deafened user is implicitly muted too.
                Rectangle {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.rightMargin: -2
                    anchors.bottomMargin: -2
                    width: 22; height: 22
                    radius: 11
                    color: Theme.danger
                    border.color: Theme.bg2
                    border.width: 2
                    visible: tile.muted || tile.deafened
                    Icon {
                        anchors.centerIn: parent
                        name: tile.deafened ? "headphones-off" : "mic-off"
                        size: 12
                        color: Theme.onAccent
                    }
                }

                // Peer-connection dot — top-right of the avatar. Only
                // visible while the connection is anything other than
                // "connected" so the steady state stays clean. Colour
                // mirrors the VoiceDock / old VoicePanel convention:
                // green=online, yellow=handshaking, red=broken.
                Rectangle {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.rightMargin: -2
                    anchors.topMargin: -2
                    width: 14; height: 14
                    radius: 7
                    border.color: Theme.bg2
                    border.width: 2
                    color: {
                        switch (tile.peerState) {
                            case "connecting":   return Theme.warn;
                            case "new":          return Theme.warn;
                            case "failed":       return Theme.danger;
                            case "disconnected": return Theme.danger;
                            default:             return Theme.online;
                        }
                    }
                    visible: !tile.isSelf && tile.peerState !== "connected"
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                }
            }
        }

        // Name — Geist semibold, fg0.
        Text {
            Layout.alignment: Qt.AlignHCenter
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: tile.dispName
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.md
            font.weight: Theme.fontWeight.semibold
            color: Theme.fg0
            elide: Text.ElideRight
        }

        // Status line — reserves a fixed height so appearing/disappearing
        // text doesn't shift the avatar or name. Uses opacity, not
        // visibility, for the same reason.
        Item {
            Layout.alignment: Qt.AlignHCenter
            Layout.fillWidth: true
            Layout.preferredHeight: 16

            Text {
                anchors.centerIn: parent
                horizontalAlignment: Text.AlignHCenter
                text: {
                    switch (tile.peerState) {
                    case "connected":    return tile.speaking ? "Speaking" : "";
                    case "connecting":   return "Connecting…";
                    case "new":          return "Joining…";
                    case "failed":       return "Connection failed";
                    case "disconnected": return "Disconnected";
                    default:             return tile.peerState;
                    }
                }
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: tile.peerState === "failed" ? Theme.danger
                     : tile.speaking ? Theme.accent
                     : Theme.fg2
                elide: Text.ElideRight
                opacity: text.length > 0 ? 1.0 : 0.0
                Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }
            }
        }

        // Connection path — "direct" or "relayed", from the ICE candidate pair
        // this connection actually selected. Diagnostics, so it follows the
        // same switch as the video overlay rather than being on all the time.
        //
        // Empty (and so invisible) while ICE has not settled: C++ leaves the
        // key absent rather than guessing, and guessing is exactly what would
        // make this misleading — "direct" shown for a connection that is about
        // to come up relayed reads as a leak that is not happening.
        Item {
            visible: appSettings.showVideoDiagnostics && !tile.isSelf
                     && tile.connectionPath.length > 0
            Layout.alignment: Qt.AlignHCenter
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 14 : 0

            Text {
                anchors.centerIn: parent
                horizontalAlignment: Text.AlignHCenter
                text: tile.connectionPath
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSize.xs
                color: Theme.fg2
                elide: Text.ElideRight
            }
        }

        // Bottom spacer.
        Item { Layout.fillHeight: true }
    }
}
