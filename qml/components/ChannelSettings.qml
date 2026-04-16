import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// Channel-level settings: slowmode, privacy shortcut, per-role allow/deny
// overrides. Sectioned layout with a ScrollView so it degrades on short
// screens, plus a reusable TriToggle for Allow / Neutral / Deny.
Popup {
    id: channelSettings
    anchors.centerIn: Overlay.overlay
    width: Math.min(parent ? parent.width * 0.9 : 780, 780)
    height: Math.min(parent ? parent.height * 0.88 : 720, 720)
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    property string roomId: ""
    property string roomName: ""

    // Flags that actually make sense per-channel. Members / role-admin flags
    // (MANAGE_ROLES, MANAGE_SERVER, ADMINISTRATOR) are intentionally omitted —
    // those are server-wide concepts.
    readonly property var channelFlags: [
        {key: "view",    label: "View channel",    flag: 0x0001,
         hint: "Whether members with this role can see this channel at all."},
        {key: "send",    label: "Send messages",   flag: 0x0002, hint: ""},
        {key: "attach",  label: "Attach files",    flag: 0x0004, hint: ""},
        {key: "embed",   label: "Embed links",     flag: 0x0008, hint: ""},
        {key: "manmsg",  label: "Manage messages", flag: 0x0010,
         hint: "Delete anyone's message. Also bypasses slowmode."}
    ]

    // Depend on permissionsGeneration so override state updates immediately
    // after we write one.
    readonly property int _gen: serverManager.activeServer
        ? serverManager.activeServer.permissionsGeneration : 0

    readonly property var allOverrides: {
        if (!serverManager.activeServer) return [];
        _gen; // dependency
        return serverManager.activeServer.channelOverrides(roomId);
    }

    function overrideFor(targetKey) {
        for (var i = 0; i < allOverrides.length; i++) {
            if (allOverrides[i].target === targetKey) {
                return {
                    allow: Number(allOverrides[i].allowFlags),
                    deny:  Number(allOverrides[i].denyFlags)
                };
            }
        }
        return {allow: 0, deny: 0};
    }

    background: Rectangle {
        color: Theme.bgDark
        radius: Theme.radiusNormal
        border.color: Theme.bgLight
        border.width: 1
    }

    // ----- Reusable subcomponents -----

    component SectionHeader: Item {
        property alias text: label.text
        Layout.fillWidth: true
        Layout.preferredHeight: 32
        Text {
            id: label
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            font.pixelSize: Theme.fontSizeSmall
            font.bold: true
            color: Theme.textMuted
            font.letterSpacing: 0.5
        }
        Rectangle {
            anchors.left: label.right
            anchors.leftMargin: Theme.spacingNormal
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            height: 1
            color: Theme.bgLight
            opacity: 0.5
        }
    }

    // Row with a title/description on the left and an arbitrary control slot
    // (default property) on the right. Used for the slowmode and private
    // channel rows.
    component SettingRow: RowLayout {
        property string title: ""
        property string description: ""
        default property alias rightControl: rightContainer.children
        Layout.fillWidth: true
        spacing: Theme.spacingLarge

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Text {
                text: title
                font.pixelSize: Theme.fontSizeNormal
                font.bold: true
                color: Theme.textPrimary
            }
            Text {
                visible: description.length > 0
                Layout.fillWidth: true
                text: description
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textMuted
                wrapMode: Text.WordWrap
            }
        }
        Item {
            id: rightContainer
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: childrenRect.width
            implicitHeight: childrenRect.height
        }
    }

    // 3-segment Allow / Neutral / Deny control. `state` is -1 deny, 0 neutral,
    // 1 allow. Emits stateChangeRequested(newState).
    component TriToggle: Row {
        id: tri
        property int state: 0
        signal stateChangeRequested(int newState)

        spacing: 4
        Repeater {
            model: [
                {label: "Allow",   value:  1, active: "#57f287"},
                {label: "Neutral", value:  0, active: "#768390"},
                {label: "Deny",    value: -1, active: "#ed4245"}
            ]
            delegate: Rectangle {
                width: 72
                height: 28
                radius: 4
                readonly property bool isSelected: tri.state === modelData.value
                color: isSelected ? modelData.active : Theme.bgMedium
                border.color: isSelected ? Qt.lighter(modelData.active, 1.2) : "transparent"
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: modelData.label
                    color: parent.isSelected ? "white" : Theme.textMuted
                    font.pixelSize: Theme.fontSizeSmall
                    font.bold: parent.isSelected
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: tri.stateChangeRequested(modelData.value)
                }
            }
        }
    }

    // ----- Layout -----

    contentItem: ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Header bar
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            color: "transparent"
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacingLarge
                anchors.rightMargin: Theme.spacingLarge
                Text {
                    Layout.fillWidth: true
                    text: channelSettings.roomName
                        ? ("Channel settings — #" + channelSettings.roomName)
                        : "Channel settings"
                    font.pixelSize: 18
                    font.bold: true
                    color: Theme.textPrimary
                    elide: Text.ElideRight
                }
                Text {
                    text: "✕"
                    font.pixelSize: 20
                    color: closeXMouse.containsMouse ? Theme.textPrimary : Theme.textMuted
                    Layout.alignment: Qt.AlignVCenter
                    MouseArea {
                        id: closeXMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: channelSettings.close()
                    }
                }
            }
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.bgLight
            }
        }

        // Scrollable body
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: channelSettings.width - Theme.spacingLarge * 2
                x: Theme.spacingLarge
                y: Theme.spacingLarge
                spacing: Theme.spacingLarge * 1.25

                // ====== OVERVIEW ======
                SectionHeader { text: "OVERVIEW" }

                SettingRow {
                    title: "Slowmode"
                    description: "Members must wait this long between messages. Users with Manage messages bypass."
                    ComboBox {
                        id: slowmodeCombo
                        implicitWidth: 160
                        model: [
                            {label: "Off",        seconds: 0},
                            {label: "5 seconds",  seconds: 5},
                            {label: "10 seconds", seconds: 10},
                            {label: "30 seconds", seconds: 30},
                            {label: "1 minute",   seconds: 60},
                            {label: "5 minutes",  seconds: 300},
                            {label: "10 minutes", seconds: 600},
                            {label: "1 hour",     seconds: 3600}
                        ]
                        textRole: "label"
                        valueRole: "seconds"

                        Component.onCompleted: syncIndex()
                        Connections {
                            target: channelSettings
                            function on_GenChanged() { slowmodeCombo.syncIndex(); }
                        }
                        function syncIndex() {
                            if (!serverManager.activeServer) return;
                            var cur = serverManager.activeServer.channelSlowmode(
                                channelSettings.roomId);
                            for (var i = 0; i < model.length; i++) {
                                if (model[i].seconds === cur) {
                                    currentIndex = i;
                                    return;
                                }
                            }
                            currentIndex = 0;
                        }
                        onActivated: {
                            if (!serverManager.activeServer) return;
                            serverManager.activeServer.setChannelSlowmode(
                                channelSettings.roomId, model[currentIndex].seconds);
                        }
                    }
                }

                SettingRow {
                    id: privateRow
                    title: "Private channel"
                    description: "When on, @everyone is denied View Channel here. Only roles with an explicit Allow override can see it."

                    readonly property var _evOverride: channelSettings.overrideFor("role:everyone")
                    readonly property bool isPrivate: (_evOverride.deny & 0x1) !== 0

                    Switch {
                        id: privateSwitch
                        checked: privateRow.isPrivate
                        onToggled: {
                            if (!serverManager.activeServer) return;
                            var ov = privateRow._evOverride;
                            var deny = ov.deny;
                            if (checked) deny |= 0x1; else deny &= ~0x1;
                            serverManager.activeServer.setChannelOverride(
                                channelSettings.roomId, "role:everyone", ov.allow, deny);
                        }
                    }
                }

                // ====== PERMISSIONS ======
                SectionHeader { text: "ROLE OVERRIDES" }

                Text {
                    Layout.fillWidth: true
                    text: "Pick a role, then tune its permissions in this channel. Allow grants, Deny revokes, Neutral inherits from the role's server-wide permissions."
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.textMuted
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacingNormal
                    Text {
                        text: "Role"
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSizeSmall
                    }
                    ComboBox {
                        id: roleCombo
                        Layout.fillWidth: true
                        model: serverManager.activeServer
                            ? serverManager.activeServer.serverRoles : []
                        textRole: "name"
                        currentIndex: 0
                    }
                }

                // Permission grid
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Repeater {
                        model: channelSettings.channelFlags
                        delegate: Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: permRow.implicitHeight + 16
                            radius: Theme.radiusSmall
                            color: Theme.bgMedium

                            readonly property var flagInfo: modelData
                            // Use activeServer + currentIndex + permissionsGeneration as deps
                            readonly property int _gen: channelSettings._gen
                            readonly property var selectedRole:
                                roleCombo.currentIndex >= 0 && roleCombo.model.length > 0
                                    ? roleCombo.model[roleCombo.currentIndex] : null
                            readonly property string targetKey: selectedRole
                                ? ("role:" + (selectedRole.id || selectedRole.name)) : ""
                            readonly property var current: channelSettings.overrideFor(targetKey)
                            readonly property int triState:
                                (current.allow & flagInfo.flag) !== 0 ?  1 :
                                (current.deny  & flagInfo.flag) !== 0 ? -1 : 0

                            function apply(newState) {
                                if (!serverManager.activeServer) return;
                                if (!targetKey) return;
                                var allow = current.allow & ~flagInfo.flag;
                                var deny  = current.deny  & ~flagInfo.flag;
                                if (newState === 1)  allow |= flagInfo.flag;
                                if (newState === -1) deny  |= flagInfo.flag;
                                serverManager.activeServer.setChannelOverride(
                                    channelSettings.roomId, targetKey, allow, deny);
                            }

                            RowLayout {
                                id: permRow
                                anchors.fill: parent
                                anchors.leftMargin: Theme.spacingNormal
                                anchors.rightMargin: Theme.spacingNormal
                                spacing: Theme.spacingNormal

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Text {
                                        text: flagInfo.label
                                        color: Theme.textPrimary
                                        font.pixelSize: Theme.fontSizeNormal
                                    }
                                    Text {
                                        visible: flagInfo.hint.length > 0
                                        Layout.fillWidth: true
                                        text: flagInfo.hint
                                        color: Theme.textMuted
                                        font.pixelSize: Theme.fontSizeSmall
                                        wrapMode: Text.WordWrap
                                    }
                                }
                                TriToggle {
                                    state: triState
                                    onStateChangeRequested: function(s) { apply(s); }
                                }
                            }
                        }
                    }
                }

                Item { Layout.preferredHeight: Theme.spacingNormal }
            }
        }

        // Footer
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            color: Theme.bgDarkest
            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 1
                color: Theme.bgLight
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacingLarge
                anchors.rightMargin: Theme.spacingLarge
                Item { Layout.fillWidth: true }
                Button {
                    text: "Close"
                    contentItem: Text {
                        text: parent.text
                        color: "white"
                        font.pixelSize: Theme.fontSizeNormal
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: parent.hovered ? Theme.accentHover : Theme.accent
                        radius: Theme.radiusSmall
                        implicitHeight: Theme.buttonHeight
                        implicitWidth: 100
                    }
                    onClicked: channelSettings.close()
                }
            }
        }
    }
}
