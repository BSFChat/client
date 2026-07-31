// Bound: the row delegate legitimately reads `root`'s properties, and without
// this every such reference is an unqualified lookup that qmllint flags and
// that resolves late at runtime.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import BSFChat

// The people currently in one voice channel, nested under that channel's row
// in the sidebar (Discord's arrangement, and SPEC §3.2: 16px indent, 22h rows,
// 16x16 avatar).
//
// Fed straight from the room list's per-room roster, which is derived from
// m.call.member state. That matters for two reasons: it covers voice channels
// the local user is NOT in — the whole point of the list, since "who's in
// there already" is what makes you decide to join — and the roster it renders
// is the same vector the count badge is sized from, so the two can never
// disagree.
//
// Lives in its own file because ChannelList.qml is already ~3k lines and the
// channel delegate is nested twelve levels deep by the time it gets here.
Column {
    id: root

    // Array of participant maps: { user_id, muted, deafened, cameraOn,
    // screenSharing, joined_at }. Empty array renders nothing.
    property var participants: []

    // Connection the roster belongs to. Used for display-name resolution and
    // for the local mic level that drives the self speaking ring; null is
    // tolerated so the delegate can't crash mid-teardown.
    property var connection: null

    // Only the local user gets a speaking indicator — the mesh doesn't signal
    // remote voice activity yet, and faking one would be a lie about who is
    // talking.
    readonly property string selfUserId: connection ? connection.userId : ""

    visible: participants && participants.length > 0
    leftPadding: 16
    bottomPadding: visible ? 4 : 0
    spacing: 2

    Repeater {
        model: root.participants

        delegate: Item {
            id: participantRow
            required property var modelData

            width: root.width - root.leftPadding
            height: 22

            readonly property string userId: modelData.user_id || ""
            readonly property bool isSelf: userId !== "" && userId === root.selfUserId
            readonly property bool muted: modelData.muted === true
            readonly property bool deafened: modelData.deafened === true
            readonly property bool sharing: modelData.screenSharing === true
            readonly property bool camera: modelData.cameraOn === true

            // Stamped by the connection when it builds the sidebar snapshot —
            // a lookup call here would not be a reactive dependency, so a name
            // that arrived after this row was built would never appear.
            readonly property string label:
                modelData.displayName || participantRow.userId

            readonly property real micLevel:
                isSelf && root.connection ? root.connection.micLevel : 0
            readonly property bool speaking: isSelf && !muted && micLevel > 0.05

            Accessible.role: Accessible.StaticText
            Accessible.name: label
                + (deafened ? ", deafened" : (muted ? ", muted" : ""))
                + (sharing ? ", sharing screen" : "")
                + (camera ? ", camera on" : "")

            RowLayout {
                anchors.fill: parent
                anchors.rightMargin: Theme.sp.s3
                spacing: Theme.sp.s3

                // 16x16 avatar inside an 18px box, so the speaking ring has
                // room to grow without nudging the row's layout.
                Item {
                    Layout.preferredWidth: 18
                    Layout.preferredHeight: 18
                    Layout.alignment: Qt.AlignVCenter

                    Rectangle {
                        anchors.centerIn: parent
                        width: 16; height: 16
                        radius: 8
                        color: Theme.senderColor(participantRow.userId)
                        opacity: participantRow.muted || participantRow.deafened ? 0.5 : 1.0
                        Text {
                            anchors.centerIn: parent
                            text: {
                                var n = participantRow.label;
                                var s = n.replace(/^[^a-zA-Z0-9]+/, "");
                                return (s.length > 0 ? s.charAt(0) : "?").toUpperCase();
                            }
                            font.family: Theme.fontSans
                            font.pixelSize: 9
                            font.weight: Theme.fontWeight.semibold
                            color: Theme.onAccent
                        }
                    }

                    Rectangle {
                        anchors.centerIn: parent
                        width: 18 + participantRow.micLevel * 6
                        height: width
                        radius: width / 2
                        color: "transparent"
                        border.color: Theme.online
                        border.width: 1.5
                        opacity: participantRow.speaking ? 0.8 : 0
                        visible: opacity > 0.01
                        Behavior on opacity { NumberAnimation { duration: 80 } }
                        Behavior on width { NumberAnimation { duration: 60 } }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    text: participantRow.label
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    font.weight: participantRow.isSelf
                        ? Theme.fontWeight.semibold
                        : Theme.fontWeight.regular
                    color: participantRow.muted || participantRow.deafened
                        ? Theme.fg3 : Theme.fg1
                    elide: Text.ElideRight
                }

                // Trailing state glyphs, in the order they matter: screen
                // share and camera are things the row's occupant is doing;
                // deafened supersedes muted because it implies it.
                Icon {
                    Layout.alignment: Qt.AlignVCenter
                    visible: participantRow.sharing
                    name: "screen-share"
                    size: 11
                    color: Theme.online
                }

                Icon {
                    Layout.alignment: Qt.AlignVCenter
                    visible: participantRow.camera
                    name: "video"
                    size: 11
                    color: Theme.online
                }

                Icon {
                    Layout.alignment: Qt.AlignVCenter
                    visible: participantRow.deafened || participantRow.muted
                    name: participantRow.deafened ? "headphones-off" : "mic-off"
                    size: 11
                    color: Theme.danger
                }
            }
        }
    }
}
