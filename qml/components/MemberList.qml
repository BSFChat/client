import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// Member list (SPEC §3 — "memberListW 220"). Quiet bg1 surface, widely-
// tracked "MEMBERS — N" label in fg3, 32×32 avatars with rounded-square
// shape to echo the ServerRail. Names inherit the user's highest-
// positioned hoisted role's colour so role hierarchy reads at a glance.
Rectangle {
    id: memberListRoot
    color: Theme.bg1
    implicitWidth: Theme.layout.memberListW

    // Re-evaluate role lookups whenever the server's permission state
    // ticks — role assignment, role edit, role create/delete all bump
    // permissionsGeneration. Child delegate bindings depend on this so
    // they refresh in lockstep with server-side changes.
    readonly property int _gen: serverManager.activeServer
        ? serverManager.activeServer.permissionsGeneration : 0

    // Presence dots re-evaluate on every `presenceChanged` tick (sender
    // activity observed, self-status set). The underlying presenceFor()
    // lookup is a plain QMap read; this property just drives the refresh.
    property int _presenceGen: 0
    Connections {
        target: serverManager.activeServer
        ignoreUnknownSignals: true
        function onPresenceChanged() { memberListRoot._presenceGen++; }
    }
    // Also tick every minute so the 5-minute online window decays
    // naturally — a user who went silent 4 minutes ago shouldn't still
    // show green forever.
    Timer { running: true; repeat: true; interval: 60 * 1000
        onTriggered: memberListRoot._presenceGen++ }

    // Resolve a user's highest-position hoisted role (Discord-style: the
    // most senior role with `hoist` flag determines the name colour +
    // dot). Returns null if the user has no applicable role or the
    // server doesn't expose roles.
    function _highestRoleFor(userId) {
        _gen;
        if (!serverManager.activeServer) return null;
        var ids = serverManager.activeServer.memberRoles(userId);
        if (!ids || ids.length === 0) return null;
        var roles = serverManager.activeServer.serverRoles;
        if (!roles) return null;
        var best = null;
        for (var i = 0; i < ids.length; ++i) {
            var id = ids[i];
            if (id === "everyone") continue;
            for (var j = 0; j < roles.length; ++j) {
                var r = roles[j];
                var rid = r.id || r.name;
                if (rid === id && (!best || (r.position || 0) > (best.position || 0))) {
                    best = r;
                }
            }
        }
        return best;
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Header. Member count derived from the active-room roster; falls
        // back to 0 while the model isn't bound.
        Item {
            id: headerItem
            Layout.fillWidth: true
            Layout.preferredHeight: 48

            // memberListView.count tracks model.rowCount reactively —
                // rowCount() as a direct call wouldn't re-evaluate when
                // rows are inserted/removed.
            readonly property int memberCount: memberListView.count

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.sp.s7
                anchors.rightMargin: Theme.sp.s7
                spacing: Theme.sp.s2

                Text {
                    text: "MEMBERS"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xs
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackWidest.xs
                    color: Theme.fg3
                    verticalAlignment: Text.AlignVCenter
                }
                Text {
                    text: "— " + headerItem.memberCount
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSize.xs
                    color: Theme.fg3
                    verticalAlignment: Text.AlignVCenter
                    visible: headerItem.memberCount > 0
                }
                Item { Layout.fillWidth: true }
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
            id: memberListView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            ScrollBar.vertical: ThemedScrollBar {}
            model: serverManager.activeServer ? serverManager.activeServer.memberListModel : null

            delegate: Item {
                id: memberDelegate
                width: ListView.view.width
                height: 40

                // Role + name colour lookup, bound to the outer `_gen` so
                // a role save ripples through to every row instantly.
                readonly property var _role: memberListRoot._highestRoleFor(model.userId)
                readonly property color _roleColor: _role && _role.color ? _role.color : Theme.fg1

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
                        Item {
                            width: Theme.avatar.md
                            height: Theme.avatar.md

                            Rectangle {
                                anchors.fill: parent
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

                            // Presence dot — online/idle/dnd/offline colour,
                            // offline rendered as a hollow ring so it reads
                            // as a meaningful absence rather than "no dot".
                            // Bumps memberListRoot._presenceGen when sync
                            // runs so QSettings-style lookups refresh.
                            Rectangle {
                                readonly property string _state: {
                                    memberListRoot._presenceGen;
                                    var s = serverManager.activeServer;
                                    return s ? s.presenceFor(model.userId) : "offline";
                                }
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.rightMargin: -2
                                anchors.bottomMargin: -2
                                width: 10
                                height: 10
                                radius: 5
                                color: {
                                    switch (_state) {
                                    case "online": return Theme.online;
                                    case "idle":   return Theme.warn;
                                    case "dnd":    return Theme.danger;
                                    default:       return Theme.bg1;
                                    }
                                }
                                border.width: _state === "offline" ? 1.5 : 2
                                border.color: _state === "offline" ? Theme.fg3 : Theme.bg1
                                visible: true
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0

                            Text {
                                // Bold on hover, tinted by highest role
                                // normally. Falls back to fg1 for roleless
                                // members (e.g. @everyone-only).
                                text: model.displayName
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.base
                                font.weight: Theme.fontWeight.medium
                                color: {
                                    if (memberMouse.containsMouse) return Theme.fg0;
                                    return memberDelegate._role ? memberDelegate._roleColor
                                                                : Theme.fg1;
                                }
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }

                            // Role tag under the name — only shown for
                            // hoisted roles so @everyone-only members
                            // don't get a blank subtext line.
                            Text {
                                text: memberDelegate._role && memberDelegate._role.hoist
                                    ? (memberDelegate._role.name || "") : ""
                                visible: text.length > 0
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.xs
                                font.weight: Theme.fontWeight.medium
                                color: memberDelegate._roleColor
                                opacity: 0.75
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }
                    }

                    MouseArea {
                        id: memberMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: (mouse) => {
                            if (mouse.button === Qt.RightButton) {
                                memberContextMenu.userId = model.userId;
                                memberContextMenu.displayName = model.displayName || model.userId;
                                memberContextMenu.popup();
                            } else {
                                memberProfileCard.userId = model.userId;
                                memberProfileCard.profileDisplayName = model.displayName;
                                memberProfileCard.open();
                            }
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

    // Shared role-assignment popup — triggered from the context menu.
    // Declared here AND in main.qml. Both points to the same user-facing
    // behaviour; the context menu uses this local one directly, while
    // the profile-card button uses the global one via Window.window.
    RoleAssignPopup {
        id: roleAssignPopup
        parent: Overlay.overlay
    }

    // Right-click member-context menu. Permission-gated: Manage Roles
    // only shown to users with MANAGE_ROLES; self is filtered out of
    // destructive actions.
    Menu {
        id: memberContextMenu
        property string userId: ""
        property string displayName: ""

        readonly property bool isSelf: serverManager.activeServer
            && userId === serverManager.activeServer.userId
        // Probe a real roomId for the permission check — Members list is
        // channel-scoped (its model is the active-room's roster), so
        // activeRoomId is always populated here.
        readonly property bool canManageRoles: serverManager.activeServer
            && serverManager.activeServer.canManageRoles(
                   serverManager.activeServer.activeRoomId)

        background: Rectangle {
            color: Theme.bg1
            radius: Theme.r2
            border.color: Theme.line
            border.width: 1
            implicitWidth: 220
        }

        component MemberCtxItem: MenuItem {
            id: mi
            // See MessageBubble.CtxItem: invisible Menu rows otherwise
            // leave a blank slot because Qt Controls Menu doesn't always
            // skip them in layout. Folding implicitHeight to 0 removes
            // the gap cleanly.
            implicitHeight: visible ? 34 : 0
            height: implicitHeight
            property string iconName: ""
            property color labelColor: Theme.fg0
            contentItem: RowLayout {
                spacing: Theme.sp.s3
                Icon {
                    name: mi.iconName; size: 14
                    color: !mi.enabled ? Theme.fg3
                         : mi.hovered ? mi.labelColor : Theme.fg2
                    Layout.leftMargin: Theme.sp.s3
                }
                Text {
                    text: mi.text
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    color: !mi.enabled ? Theme.fg3 : mi.labelColor
                    Layout.fillWidth: true
                    verticalAlignment: Text.AlignVCenter
                }
            }
            background: Rectangle {
                color: mi.hovered && mi.enabled ? Theme.bg2 : "transparent"
                radius: Theme.r1
                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
            }
        }

        MemberCtxItem {
            text: "View profile"
            iconName: "at"
            onTriggered: {
                memberProfileCard.userId = memberContextMenu.userId;
                memberProfileCard.profileDisplayName = memberContextMenu.displayName;
                memberProfileCard.open();
            }
        }
        MemberCtxItem {
            text: "Copy user ID"
            iconName: "copy"
            onTriggered: {
                if (serverManager) serverManager.copyToClipboard(memberContextMenu.userId);
            }
        }

        MenuSeparator {
            visible: memberContextMenu.canManageRoles
            contentItem: Rectangle {
                implicitWidth: 180
                implicitHeight: 1
                color: Theme.line
            }
        }
        MemberCtxItem {
            text: "Manage roles…"
            iconName: "shield"
            visible: memberContextMenu.canManageRoles
            onTriggered: roleAssignPopup.openFor(
                memberContextMenu.userId,
                memberContextMenu.displayName)
        }
    }
}
