import QtQuick
import QtQuick.Layouts
import BSFChat

import "../js/VoiceLevels.js" as VoiceLevels

// One person in VoiceRoom's bottom member strip — the compact 56-wide
// avatar chip with a speaking ring, a mute/deafen glyph and a name.
//
// Lifted out of the ListView delegate it used to be inline in VoiceRoom.qml
// for two reasons, and the second is the important one:
//
//   * VoiceRoom.qml is 1,200 lines and the delegate was eighty of them.
//
//   * THE RING NEVER LIT. The delegate read `modelData.speaking === true`
//     off a row of `activeServer.voiceMembers`, and nothing writes a
//     `speaking` key — not ServerConnection::buildVoiceMembers(), which
//     stamps displayName / screenSharing / cameraOn / peerState /
//     connectionPath and nothing else, and not the protocol, which has no
//     such field anywhere. So the expression was `undefined === true`, i.e.
//     false, for everybody, always. Nobody noticed because a ring that
//     never appears looks exactly like a room where nobody is talking.
//
//     Adding the key server-side would not have fixed it either:
//     `voiceMembers` is republished by a 5-second poll, and a speaking
//     indicator quantised to 5 seconds is worse than none.
//
//     The live level was already there, on the same connection, and
//     ParticipantTile.qml has always read it. So this component does what
//     that one does, and VoiceRoom instantiates it rather than restating
//     it — which is the reason the divergence was possible at all.
//
// A chip is NOT a smaller ParticipantTile: the strip is what is on screen
// while somebody is sharing their screen, so it is deliberately austere —
// no peer-state dot, no connection path, no status line. Only the three
// things you need at a glance about a voice member whose face is not on
// the stage: who, are they audible, are they talking.
Item {
    id: chip

    // One row of activeServer.voiceMembers. Defaulted to an empty object so
    // every binding below reads as "unknown member" rather than throwing
    // during delegate teardown, when modelData briefly goes undefined.
    property var member: ({})

    readonly property string userId: chip.member ? (chip.member.user_id || "") : ""
    readonly property string dispName:
        chip.member ? (chip.member.displayName || chip.userId || "?") : "?"

    // Muted / deafened stay on the row. Unlike the level, these ARE
    // published — buildVoiceMembers() passes the server's flags straight
    // through — and unlike the level they change at human speed, so the
    // poll that carries them is fast enough.
    readonly property bool muted:    chip.member ? chip.member.muted === true : false
    readonly property bool deafened: chip.member ? chip.member.deafened === true : false

    // The connection, read once. `serverManager` is a context property, so
    // every reference to it is an unqualified lookup that qmllint flags and
    // that resolves late at runtime; naming it here means one such lookup in
    // the file instead of six, and one place that knows where a level comes
    // from. It is a binding, not a stored value — activeServer changes when
    // the user switches server and everything below has to follow.
    readonly property var conn: serverManager.activeServer

    readonly property bool isSelf:
        chip.userId.length > 0
        && chip.conn
        && chip.userId === chip.conn.userId

    // ── The live level ────────────────────────────────────────────
    //
    // Same shape as ParticipantTile.qml, and it has to be: peerLevel() is
    // an INVOKABLE, not a property, so calling it creates no binding
    // dependency and a ring bound to it alone would freeze on whatever the
    // first call returned. peerLevelChanged(userId) is the notification,
    // and _levelGen is the dependency — a counter the binding reads so that
    // bumping it re-evaluates the call.
    //
    // micLevel, for ourselves, is a real Q_PROPERTY and needs none of this.
    property int _levelGen: 0
    Connections {
        target: chip.conn
        // The strip outlives connections: activeServer is null between
        // servers, and a null target with a named handler is a warning per
        // delegate per switch without this.
        ignoreUnknownSignals: true
        function onPeerLevelChanged(uid) {
            if (uid === chip.userId) chip._levelGen++;
        }
    }

    readonly property real level: {
        if (!chip.conn) return 0;
        if (chip.isSelf) return chip.conn.micLevel;
        chip._levelGen;   // dependency, see above — not a no-op
        return chip.conn.peerLevel(chip.userId);
    }

    // Mute is passed IN rather than checked here — VoiceLevels owns the whole
    // predicate, including why deafened is not part of it.
    readonly property bool speaking:
        VoiceLevels.speaking(chip.level, chip.isSelf, chip.muted)

    implicitWidth: 56

    // The whole chip reads as one thing, in the order a screen reader user
    // needs it: who, then what is unusual about them. Same construction as
    // ParticipantTile.qml, with one deliberate omission.
    //
    // `speaking` IS NOT IN THE NAME. The ring is driven by a raw level with no
    // hold on it, so it flickers off in the gaps between words — fine for a
    // ring the eye integrates, intolerable for a name a screen reader
    // re-announces every time it changes. Releasing speech needs a hold longer
    // than an inter-word gap, which is exactly what MemberListModel's
    // kSpeakingHoldMs exists for; until that state is reachable from here the
    // honest thing is to leave it out rather than ship a stutter.
    // ParticipantTile.qml does include it, and has the same problem.
    Accessible.role: Accessible.StaticText
    Accessible.name: {
        var s = [];
        s.push(chip.isSelf ? qsTr("%1, you").arg(chip.dispName) : chip.dispName);
        if (chip.deafened) s.push(qsTr("deafened"));
        else if (chip.muted) s.push(qsTr("muted"));
        return s.join(qsTr(", "));
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 2

        // Speaking ring + avatar tile.
        Item {
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44
            Layout.alignment: Qt.AlignHCenter

            Rectangle {
                objectName: "speakingRing"
                anchors.centerIn: parent
                width: parent.width + 6
                height: parent.height + 6
                radius: width / 2
                color: "transparent"
                border.width: 2
                border.color: Theme.online
                opacity: chip.speaking ? 0.9 : 0
                visible: opacity > 0.01
                // Decoration; the state is in the chip's accessible name.
                Accessible.ignored: true
                Behavior on opacity { NumberAnimation { duration: 120 } }
            }

            Rectangle {
                anchors.fill: parent
                radius: Theme.r2
                color: Theme.senderColor(chip.userId)
                Text {
                    anchors.centerIn: parent
                    text: (chip.dispName.replace(/^[^a-zA-Z0-9]+/, "")
                          .charAt(0) || "?").toUpperCase()
                    Accessible.ignored: true
                    font.family: Theme.fontSans
                    font.pixelSize: 16
                    font.weight: Theme.fontWeight.semibold
                    color: Theme.onAccent
                }
            }

            // Status glyph in the bottom-right.
            Item {
                objectName: "statusGlyph"
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: -2
                width: 14; height: 14
                visible: chip.muted || chip.deafened
                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color: Theme.danger
                    border.color: Theme.bg1
                    border.width: 1.5
                }
                Icon {
                    anchors.centerIn: parent
                    name: chip.deafened ? "headphones-off" : "mic-off"
                    size: 8
                    color: "white"
                }
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
            horizontalAlignment: Text.AlignHCenter
            text: chip.dispName
            Accessible.ignored: true
            font.family: Theme.fontSans
            font.pixelSize: 10
            color: Theme.fg2
            elide: Text.ElideRight
        }
    }
}
