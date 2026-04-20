import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// Member list (SPEC §3 — "memberListW 220"). Quiet bg1 surface, widely-
// tracked "MEMBERS" label in fg3, 32×32 avatars with rounded-square shape
// to echo the ServerRail, display name in fg1. Will later grow presence
// dots on avatars when the m.presence feature lands.
Rectangle {
    color: Theme.bg1
    implicitWidth: Theme.layout.memberListW

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Header
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 48

            Text {
                anchors.fill: parent
                anchors.leftMargin: Theme.sp.s7
                verticalAlignment: Text.AlignVCenter
                text: "MEMBERS"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWidest.xs
                color: Theme.fg3
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.line
            }
        }

        // Member list
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            ScrollBar.vertical: ThemedScrollBar {}
            model: serverManager.activeServer ? serverManager.activeServer.memberListModel : null

            delegate: Item {
                width: ListView.view.width
                height: 40

                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.sp.s2
                    anchors.rightMargin: Theme.sp.s2
                    radius: Theme.r1
                    color: memberMouse.containsMouse ? Theme.bg3 : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.sp.s4
                        anchors.rightMargin: Theme.sp.s4
                        spacing: Theme.sp.s4

                        // Avatar — rounded-square so it rhymes with the
                        // ServerRail tiles rather than Discord's round pill.
                        Rectangle {
                            width: Theme.avatar.md
                            height: Theme.avatar.md
                            radius: Theme.r2
                            color: Theme.senderColor(model.userId)

                            Text {
                                anchors.centerIn: parent
                                text: {
                                    var n = model.displayName || "?";
                                    var stripped = n.replace(/^[^a-zA-Z0-9]+/, "");
                                    return (stripped.length > 0
                                            ? stripped.charAt(0)
                                            : "?").toUpperCase();
                                }
                                font.family: Theme.fontSans
                                font.pixelSize: 13
                                font.weight: Theme.fontWeight.semibold
                                color: Theme.onAccent
                            }
                        }

                        Text {
                            text: model.displayName
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.base
                            font.weight: Theme.fontWeight.medium
                            color: memberMouse.containsMouse ? Theme.fg0 : Theme.fg1
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }

                    MouseArea {
                        id: memberMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            memberProfileCard.userId = model.userId;
                            memberProfileCard.profileDisplayName = model.displayName;
                            memberProfileCard.open();
                        }
                    }
                }
            }
        }
    }

    // Profile card for clicking on member names
    UserProfileCard {
        id: memberProfileCard
        parent: Overlay.overlay
    }
}
