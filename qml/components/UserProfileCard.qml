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

    // ---- Per-server nickname ----
    // Empty means "no nickname set", matching the API: the endpoint omits the key
    // rather than returning "". This is fetched separately from the profile
    // because GET /profile returns the user's GLOBAL display name, so the profile
    // alone cannot tell us whether the name shown elsewhere is a nickname — and
    // without that we could not offer "Remove nickname" only when there is one.
    property string nickname: ""
    property bool editingNickname: false

    readonly property bool isSelf: serverManager.activeServer
        && userId === serverManager.activeServer.userId

    // The two flags are independent, exactly as the server checks them: renaming
    // yourself needs CHANGE_NICKNAME, renaming anyone else needs MANAGE_NICKNAMES.
    // Holding one does not imply the other. Both are server-scope questions.
    // Read-and-compared rather than merely touched, for the reason documented in
    // MessageInput: a bare property read gets dead-code-eliminated on QML's
    // AOT-compiled path and the binding then never re-evaluates.
    property int _permGen: serverManager.activeServer
        ? serverManager.activeServer.permissionsGeneration : 0

    readonly property bool mayEditNickname: {
        if (!serverManager.activeServer) return false;
        return _permGen >= 0 && (isSelf ? serverManager.activeServer.canChangeNickname()
                                        : serverManager.activeServer.canManageNicknames());
    }

    // The name this user actually renders as on this server.
    readonly property string effectiveName:
        nickname !== "" ? nickname : (profileDisplayName || userId)

    width: 320
    // Driven by content rather than a fixed 290, because the nickname row and its
    // editor appear conditionally and a hard-coded height would clip them. Floored
    // at 290 so the common card keeps the size it had, and derived from the
    // layout's implicit height so a longer nickname or an extra line cannot
    // overflow. The fillHeight spacer contributes nothing implicit, so this is the
    // natural content height.
    height: Math.max(290, cardContent.implicitHeight + 2 * Theme.sp.s7)
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
        // Reset per-user state before the fetches land, so reopening the card on a
        // different member never shows the previous member's nickname.
        nickname = "";
        editingNickname = false;
        if (userId !== "" && serverManager.activeServer) {
            serverManager.activeServer.fetchProfile(userId);
            serverManager.activeServer.fetchNickname(userId);
        }
    }

    // Sends whatever is in the editor. An empty value is a CLEAR, not a no-op, so
    // this is also what "Remove" calls. The editor stays open until the server
    // confirms via nicknameChanged — a rejection leaves it open with the typed
    // value so the user can correct it instead of losing what they wrote.
    function submitNickname() {
        if (!serverManager.activeServer || userId === "") return;
        serverManager.activeServer.setNickname(userId, nickField.text.trim());
    }

    Connections {
        target: serverManager.activeServer
        ignoreUnknownSignals: true
        function onProfileFetched(uid, displayName, avatarUrl) {
            if (uid === profileCard.userId) {
                profileCard.profileDisplayName = displayName || uid;
                profileCard.profileAvatarUrl = avatarUrl || "";
            }
        }
        function onNicknameFetched(uid, nick) {
            if (uid === profileCard.userId) profileCard.nickname = nick;
        }
        function onNicknameChanged(uid, nick) {
            if (uid !== profileCard.userId) return;
            // Only a confirmed write reaches here — a rejection arrives as
            // sendFeedback instead — so closing the editor on this signal cannot
            // leave the card showing a name the server refused.
            profileCard.nickname = nick;
            profileCard.editingNickname = false;
        }
    }

    contentItem: ColumnLayout {
        id: cardContent
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

        // Display name — Geist semibold, tight tracking. Shows the EFFECTIVE
        // name, i.e. the nickname when one is set, so the card agrees with the
        // member list and message authors rather than showing the global name
        // while every other surface shows the nickname.
        Text {
            text: profileCard.effectiveName
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.xl
            font.weight: Theme.fontWeight.semibold
            font.letterSpacing: Theme.trackTight.xl
            color: Theme.fg0
            Layout.fillWidth: true
            elide: Text.ElideRight
        }

        // The account's own name, shown only when a nickname is masking it.
        // A moderator looking at "Steve" needs to be able to tell that the
        // account behind it is named something else.
        Text {
            visible: profileCard.nickname !== ""
                && profileCard.profileDisplayName !== ""
                && profileCard.profileDisplayName !== profileCard.nickname
            text: qsTr("aka %1").arg(profileCard.profileDisplayName)
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.sm
            color: Theme.fg2
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

        // ---- Per-server nickname ----
        // Visible when there is either a nickname to show or the permission to set
        // one. Someone with neither sees nothing, rather than a disabled control
        // for a capability their role does not have.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.sp.s3
            visible: profileCard.nickname !== "" || profileCard.mayEditNickname

            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.line }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp.s3
                visible: !profileCard.editingNickname

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    Text {
                        text: qsTr("NICKNAME ON THIS SERVER")
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                        font.weight: Theme.fontWeight.semibold
                        font.letterSpacing: Theme.trackWidest.xs
                        color: Theme.fg3
                    }
                    Text {
                        Layout.fillWidth: true
                        text: profileCard.nickname !== "" ? profileCard.nickname
                                                          : qsTr("None")
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        color: profileCard.nickname !== "" ? Theme.fg0 : Theme.fg3
                        elide: Text.ElideRight
                    }
                }

                Button {
                    id: editNickBtn
                    visible: profileCard.mayEditNickname
                    Layout.preferredWidth: 44
                    Layout.preferredHeight: 32
                    contentItem: Icon {
                        anchors.centerIn: parent
                        name: "edit"
                        size: 14
                        color: editNickBtn.hovered ? Theme.fg0 : Theme.fg1
                    }
                    background: Rectangle {
                        color: editNickBtn.hovered ? Theme.bg3 : Theme.bg2
                        border.color: Theme.line
                        border.width: 1
                        radius: Theme.r2
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                    onClicked: {
                        nickField.text = profileCard.nickname;
                        profileCard.editingNickname = true;
                        nickField.forceActiveFocus();
                    }
                    ToolTip.visible: editNickBtn.hovered
                    ToolTip.text: profileCard.isSelf ? qsTr("Change your nickname")
                                                     : qsTr("Change nickname")
                    ToolTip.delay: 500
                }
            }

            // Editor. Saving an emptied field is how a nickname is removed, so
            // "Remove" and "clear the box then Save" are the same operation — the
            // explicit button just makes it discoverable.
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp.s3
                visible: profileCard.editingNickname

                TextField {
                    id: nickField
                    Layout.fillWidth: true
                    placeholderText: profileCard.profileDisplayName || profileCard.userId
                    // Matches kMaxNicknameCodepoints server-side. The server
                    // re-checks, and counts code points rather than UTF-16 units,
                    // so this only stops the obvious overrun early.
                    maximumLength: 32
                    color: Theme.fg0
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    background: Rectangle {
                        color: Theme.bg0
                        radius: Theme.r2
                        border.color: nickField.activeFocus ? Theme.accent : Theme.line
                        border.width: 1
                    }
                    leftPadding: Theme.sp.s4
                    rightPadding: Theme.sp.s4
                    topPadding: Theme.sp.s3
                    bottomPadding: Theme.sp.s3
                    onAccepted: profileCard.submitNickname()
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp.s3

                    Button {
                        id: saveNickBtn
                        Layout.fillWidth: true
                        Layout.preferredHeight: 34
                        contentItem: Text {
                            text: qsTr("Save")
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.sm
                            font.weight: Theme.fontWeight.semibold
                            color: Theme.onAccent
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            color: saveNickBtn.hovered ? Theme.accentDim : Theme.accent
                            radius: Theme.r2
                            Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                        }
                        onClicked: profileCard.submitNickname()
                    }

                    Button {
                        id: clearNickBtn
                        visible: profileCard.nickname !== ""
                        Layout.preferredHeight: 34
                        Layout.preferredWidth: 80
                        contentItem: Text {
                            text: qsTr("Remove")
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.sm
                            color: clearNickBtn.hovered ? Theme.danger : Theme.fg1
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            color: clearNickBtn.hovered ? Theme.bg3 : Theme.bg2
                            border.color: Theme.line
                            border.width: 1
                            radius: Theme.r2
                            Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                        }
                        onClicked: {
                            nickField.text = "";
                            profileCard.submitNickname();
                        }
                    }

                    Button {
                        id: cancelNickBtn
                        Layout.preferredHeight: 34
                        Layout.preferredWidth: 74
                        contentItem: Text {
                            text: qsTr("Cancel")
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.sm
                            color: cancelNickBtn.hovered ? Theme.fg0 : Theme.fg1
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            color: cancelNickBtn.hovered ? Theme.bg3 : Theme.bg2
                            border.color: Theme.line
                            border.width: 1
                            radius: Theme.r2
                            Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                        }
                        onClicked: profileCard.editingNickname = false
                    }
                }
            }
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
