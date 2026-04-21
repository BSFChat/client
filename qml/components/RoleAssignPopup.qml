import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// Standalone "assign roles to a member" modal. Opens via
// openFor(userId, displayName). Renders one row per server role with a
// checkbox (skipping @everyone, which is implicit), plus a Save pill.
//
// Shared by MemberList and any other call site that wants to manage a
// specific user's roles without digging into Server Settings →
// Members tab. Writes land through ServerConnection.setMemberRoles,
// which handles optimistic updates + rollback on rejection.
Popup {
    id: popup

    property string userId: ""
    property string displayName: ""

    // Mutable set of assigned role ids — keyed by id, truthy value means
    // "assigned". Rebuilt on open from serverManager.activeServer and
    // mutated by checkbox toggles / row clicks.
    property var assignedSet: ({})

    function openFor(uid, name) {
        userId = uid;
        displayName = name || uid;
        _rebuildAssignedSet();
        open();
    }

    function _rebuildAssignedSet() {
        var m = {};
        if (serverManager.activeServer && userId) {
            var list = serverManager.activeServer.memberRoles(userId);
            for (var i = 0; i < list.length; i++) m[list[i]] = true;
        }
        assignedSet = m;
    }

    anchors.centerIn: Overlay.overlay
    width: Math.min(parent ? parent.width * 0.85 : 460, 460)
    height: Math.min(parent ? parent.height * 0.85 : 520, 520)
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 0

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1
    }

    contentItem: ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Header: avatar + display name + mxid + close X.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 72
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.sp.s7
                anchors.rightMargin: Theme.sp.s5
                spacing: Theme.sp.s4

                Rectangle {
                    Layout.preferredWidth: Theme.avatar.md
                    Layout.preferredHeight: Theme.avatar.md
                    radius: Theme.r2
                    color: Theme.senderColor(popup.userId)
                    Text {
                        anchors.centerIn: parent
                        text: {
                            var n = popup.displayName || popup.userId || "?";
                            var s = n.replace(/^[^a-zA-Z0-9]+/, "");
                            return (s.length > 0 ? s.charAt(0) : "?").toUpperCase();
                        }
                        font.family: Theme.fontSans
                        font.pixelSize: 14
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.onAccent
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Text {
                        text: "Manage roles"
                        color: Theme.fg3
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                        font.weight: Theme.fontWeight.semibold
                        font.letterSpacing: Theme.trackWidest.xs
                    }
                    Text {
                        text: popup.displayName
                        color: Theme.fg0
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.lg
                        font.weight: Theme.fontWeight.semibold
                        font.letterSpacing: Theme.trackTight.lg
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Text {
                        text: popup.userId
                        color: Theme.fg3
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSize.xs
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    Layout.alignment: Qt.AlignTop
                    Layout.topMargin: 2
                    radius: Theme.r1
                    color: closeXMouse.containsMouse ? Theme.bg3 : "transparent"
                    Icon {
                        anchors.centerIn: parent
                        name: "x"; size: 14
                        color: closeXMouse.containsMouse ? Theme.fg0 : Theme.fg2
                    }
                    MouseArea {
                        id: closeXMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: popup.close()
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

        // Scrollable list of roles. Filter out @everyone — it's the
        // implicit baseline and can't be un-assigned.
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ScrollBar.vertical: ThemedScrollBar {}
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: popup.width
                spacing: 2

                Repeater {
                    model: serverManager.activeServer
                        ? serverManager.activeServer.serverRoles : []
                    delegate: Rectangle {
                        readonly property var role: modelData
                        readonly property string roleId: role.id || role.name
                        readonly property bool isChecked:
                            popup.assignedSet[roleId] === true
                        visible: roleId !== "everyone"
                        Layout.fillWidth: true
                        Layout.leftMargin: Theme.sp.s5
                        Layout.rightMargin: Theme.sp.s5
                        implicitHeight: 40
                        radius: Theme.r1
                        color: roleHover.containsMouse ? Theme.bg2 : "transparent"
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.sp.s3
                            anchors.rightMargin: Theme.sp.s3
                            spacing: Theme.sp.s3

                            ThemedCheckBox {
                                id: roleCheckbox
                                checked: parent.parent.isChecked
                            }
                            Rectangle {
                                Layout.alignment: Qt.AlignVCenter
                                width: 12; height: 12; radius: 6
                                color: parent.parent.role.color || Theme.accent
                                border.color: Theme.bg0
                                border.width: 1
                            }
                            Text {
                                Layout.alignment: Qt.AlignVCenter
                                Layout.fillWidth: true
                                text: parent.parent.role.name || parent.parent.roleId
                                color: Theme.fg0
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.md
                                font.weight: Theme.fontWeight.medium
                                elide: Text.ElideRight
                            }
                            // Hoist marker — tiny pip indicating the role
                            // is hoisted (displayed separately / colours
                            // names). Informational only.
                            Text {
                                visible: parent.parent.role.hoist === true
                                text: "HOISTED"
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.xs
                                font.weight: Theme.fontWeight.semibold
                                font.letterSpacing: Theme.trackWidest.xs
                                color: parent.parent.role.color || Theme.accent
                                opacity: 0.8
                            }
                        }

                        MouseArea {
                            id: roleHover
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            // Click anywhere on the row toggles the role.
                            // Mirrors the Members-tab behaviour so muscle
                            // memory carries across.
                            onClicked: {
                                var m = {};
                                for (var k in popup.assignedSet) m[k] = popup.assignedSet[k];
                                var rid = parent.roleId;
                                if (m[rid]) delete m[rid];
                                else m[rid] = true;
                                popup.assignedSet = m;
                            }
                        }
                    }
                }

                Item { Layout.preferredHeight: Theme.sp.s3 }
            }
        }

        // Footer — Save commits, changes are diffed against the original
        // serverManager.memberRoles() so a no-op save is disabled.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 64
            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 1
                color: Theme.line
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.sp.s7
                anchors.rightMargin: Theme.sp.s7

                Text {
                    Layout.fillWidth: true
                    text: {
                        if (!serverManager.activeServer) return "";
                        var current = serverManager.activeServer.memberRoles(popup.userId) || [];
                        var assigned = [];
                        for (var k in popup.assignedSet) {
                            if (popup.assignedSet[k]) assigned.push(k);
                        }
                        // Cheap diff — count differences both ways.
                        var changed = 0;
                        var cs = {};
                        for (var i = 0; i < current.length; i++) cs[current[i]] = true;
                        for (var j = 0; j < assigned.length; j++) {
                            if (!cs[assigned[j]]) changed++;
                        }
                        for (var k2 in cs) if (popup.assignedSet[k2] !== true) changed++;
                        return changed === 0 ? "No changes"
                            : changed === 1 ? "1 change pending"
                                            : (changed + " changes pending");
                    }
                    color: Theme.fg3
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                }

                Button {
                    id: saveBtn
                    contentItem: Text {
                        text: "Save"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.onAccent
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: saveBtn.hovered ? Theme.accentDim : Theme.accent
                        radius: Theme.r2
                        implicitWidth: 120
                        implicitHeight: 36
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                    onClicked: {
                        if (!serverManager.activeServer) return;
                        var ids = [];
                        for (var k in popup.assignedSet) {
                            if (popup.assignedSet[k]) ids.push(k);
                        }
                        serverManager.activeServer.setMemberRoles(popup.userId, ids);
                        popup.close();
                    }
                }
            }
        }
    }
}
