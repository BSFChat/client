import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import BSFChat

Popup {
    id: profileCard

    property string userId: ""
    property string profileDisplayName: ""
    property string profileAvatarUrl: ""
    property string serverName: ""

    width: 320
    height: 290
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    x: (parent.width - width) / 2
    y: (parent.height - height) / 2

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0.0; to: 1.0; duration: 150; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: 0.95; to: 1.0; duration: 150; easing.type: Easing.OutCubic }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1.0; to: 0.0; duration: 100; easing.type: Easing.InCubic }
        NumberAnimation { property: "scale"; from: 1.0; to: 0.95; duration: 100; easing.type: Easing.InCubic }
    }

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1
    }

    onAboutToShow: {
        if (userId !== "" && serverManager.activeServer) {
            serverManager.activeServer.fetchProfile(userId);
        }
    }

    Connections {
        target: serverManager.activeServer
        function onProfileFetched(uid, displayName, avatarUrl) {
            if (uid === profileCard.userId) {
                profileCard.profileDisplayName = displayName || uid;
                profileCard.profileAvatarUrl = avatarUrl || "";
            }
        }
    }

    contentItem: ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.sp.s7
        spacing: Theme.sp.s7

        // Banner — tinted accent strip (derived from the user's sender
        // colour) with the avatar overlapping the bottom edge. Avatar is
        // a rounded-square to match the ServerRail / MemberList / Settings
        // treatment instead of a circle.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 72
            radius: Theme.r2
            color: Theme.senderColor(profileCard.userId)

            Rectangle {
                id: avatarTile
                width: 72
                height: 72
                radius: Theme.r3
                color: Theme.bg1
                border.color: Theme.bg1
                border.width: 4
                anchors.bottom: parent.bottom
                anchors.bottomMargin: -36
                anchors.left: parent.left
                anchors.leftMargin: Theme.sp.s7
                clip: true

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 4
                    radius: Theme.r2
                    color: Theme.senderColor(profileCard.userId)
                    clip: true

                    Image {
                        anchors.fill: parent
                        source: {
                            if (profileCard.profileAvatarUrl !== "" && serverManager.activeServer) {
                                return serverManager.activeServer.resolveMediaUrl(profileCard.profileAvatarUrl);
                            }
                            return "";
                        }
                        visible: source !== ""
                        fillMode: Image.PreserveAspectCrop
                        layer.enabled: true
                    }

                    Text {
                        anchors.centerIn: parent
                        text: {
                            var n = profileCard.profileDisplayName
                                 || profileCard.userId || "?";
                            var s = n.replace(/^[^a-zA-Z0-9]+/, "");
                            return (s.length > 0 ? s.charAt(0) : "?").toUpperCase();
                        }
                        font.family: Theme.fontSans
                        font.pixelSize: 28
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.onAccent
                        visible: profileCard.profileAvatarUrl === ""
                    }
                }
            }
        }

        // Spacer for avatar overlap (half of avatar height).
        Item {
            Layout.preferredHeight: 36
        }

        // Display name — Geist semibold, tight tracking.
        Text {
            text: profileCard.profileDisplayName || profileCard.userId
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.xl
            font.weight: Theme.fontWeight.semibold
            font.letterSpacing: Theme.trackTight.xl
            color: Theme.fg0
            Layout.fillWidth: true
            elide: Text.ElideRight
        }

        // User ID — mono, fg3 (quieter than display name).
        Text {
            text: profileCard.userId
            font.family: Theme.fontMono
            font.pixelSize: Theme.fontSize.sm
            color: Theme.fg3
            Layout.fillWidth: true
            elide: Text.ElideRight
        }

        // Server host, in mono for consistency with the mxid.
        Text {
            text: serverManager.activeServer ? serverManager.activeServer.serverUrl : ""
            font.family: Theme.fontMono
            font.pixelSize: Theme.fontSize.xs
            color: Theme.fg3
            Layout.fillWidth: true
            elide: Text.ElideRight
            visible: text !== ""
        }

        Item { Layout.fillHeight: true }

        // Action row — Send message (accent, placeholder until DMs) +
        // Manage roles (ghost, gated on MANAGE_ROLES). Both suppressed
        // when the card is showing your own profile, since you can't
        // DM yourself and shouldn't edit your own roles from here.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.sp.s3
            visible: serverManager.activeServer !== null
                && profileCard.userId !== serverManager.activeServer.userId

            readonly property bool canManageRoles: serverManager.activeServer
                && serverManager.activeServer.canManageRoles(
                       serverManager.activeServer.activeRoomId)

            Button {
                id: messageBtn
                Layout.fillWidth: true
                Layout.preferredHeight: 40
                contentItem: Text {
                    text: "Send message"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    font.weight: Theme.fontWeight.semibold
                    color: Theme.onAccent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: messageBtn.hovered ? Theme.accentDim : Theme.accent
                    radius: Theme.r2
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                }
                onClicked: {
                    // TODO: Open DM with user
                    profileCard.close();
                }
            }

            Button {
                id: manageRolesBtn
                visible: parent.canManageRoles
                Layout.preferredWidth: 44
                Layout.preferredHeight: 40
                contentItem: Icon {
                    anchors.centerIn: parent
                    name: "shield"
                    size: 16
                    color: manageRolesBtn.hovered ? Theme.fg0 : Theme.fg1
                }
                background: Rectangle {
                    color: manageRolesBtn.hovered ? Theme.bg3 : Theme.bg2
                    border.color: Theme.line
                    border.width: 1
                    radius: Theme.r2
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                }
                onClicked: {
                    var uid = profileCard.userId;
                    var dn = profileCard.profileDisplayName;
                    profileCard.close();
                    Window.window.openRoleAssignment(uid, dn);
                }
                ToolTip.visible: manageRolesBtn.hovered
                ToolTip.text: "Manage roles"
                ToolTip.delay: 500
            }
        }
    }
}
