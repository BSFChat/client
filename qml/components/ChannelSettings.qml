import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// Channel-level settings popup: slowmode + privacy (DENY VIEW_CHANNEL for
// @everyone) + per-role allow/deny overrides. Minimal UX — a single role picker
// plus a small Allow/Deny toggle list for key permissions.
Popup {
    id: channelSettings
    anchors.centerIn: Overlay.overlay
    width: parent ? Math.min(parent.width * 0.7, 620) : 620
    height: parent ? Math.min(parent.height * 0.8, 700) : 700
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    property string roomId: ""
    property string roomName: ""

    // Permission flags surfaced here (subset of the full set — view/send/attach/embed are
    // the ones that matter per-channel).
    readonly property var channelFlags: [
        {key: "view",   label: "View channel",  flag: 0x0001},
        {key: "send",   label: "Send messages", flag: 0x0002},
        {key: "attach", label: "Attach files",  flag: 0x0004},
        {key: "embed",  label: "Embed links",   flag: 0x0008},
        {key: "manmsg", label: "Manage msgs",   flag: 0x0010}
    ]

    background: Rectangle {
        color: Theme.bgDark
        radius: Theme.radiusNormal
        border.color: Theme.bgLight; border.width: 1
    }

    contentItem: ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacingLarge
        spacing: Theme.spacingLarge

        Text {
            text: "Channel settings — " + channelSettings.roomName
            font.pixelSize: 18
            font.bold: true
            color: Theme.textPrimary
        }

        // --- Slowmode ---
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingNormal

            Column {
                Layout.fillWidth: true
                Text { text: "Slowmode"; font.pixelSize: Theme.fontSizeNormal; font.bold: true; color: Theme.textPrimary }
                Text { text: "Members wait this long between messages."; font.pixelSize: Theme.fontSizeSmall; color: Theme.textMuted }
            }

            ComboBox {
                id: slowmodeCombo
                Layout.preferredWidth: 180
                model: [
                    {label: "Off", seconds: 0},
                    {label: "5 seconds", seconds: 5},
                    {label: "10 seconds", seconds: 10},
                    {label: "30 seconds", seconds: 30},
                    {label: "1 minute", seconds: 60},
                    {label: "5 minutes", seconds: 300},
                    {label: "10 minutes", seconds: 600},
                    {label: "1 hour", seconds: 3600}
                ]
                textRole: "label"
                valueRole: "seconds"

                Component.onCompleted: {
                    if (!serverManager.activeServer) return;
                    var cur = serverManager.activeServer.channelSlowmode(channelSettings.roomId);
                    for (var i = 0; i < model.length; i++) {
                        if (model[i].seconds === cur) { currentIndex = i; return; }
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

        // --- Privacy shortcut ---
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingNormal

            Column {
                Layout.fillWidth: true
                Text { text: "Private channel"; font.pixelSize: Theme.fontSizeNormal; font.bold: true; color: Theme.textPrimary }
                Text {
                    text: "When on, only members with a role that explicitly allows VIEW_CHANNEL can see this channel."
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.textMuted
                    wrapMode: Text.WordWrap
                    width: parent.width - 60
                }
            }

            Switch {
                id: privateSwitch
                // Consider channel "private" if @everyone has DENY VIEW_CHANNEL.
                checked: {
                    if (!serverManager.activeServer) return false;
                    var overrides = serverManager.activeServer.channelOverrides(channelSettings.roomId);
                    for (var i = 0; i < overrides.length; i++) {
                        if (overrides[i].target === "role:everyone" &&
                            (Number(overrides[i].denyFlags) & 0x1) !== 0) return true;
                    }
                    return false;
                }
                onToggled: {
                    if (!serverManager.activeServer) return;
                    if (checked) {
                        // DENY VIEW_CHANNEL (0x1) for @everyone.
                        serverManager.activeServer.setChannelOverride(
                            channelSettings.roomId, "role:everyone", 0, 0x1);
                    } else {
                        // Clear by setting to 0/0 (server treats empty as removal).
                        serverManager.activeServer.setChannelOverride(
                            channelSettings.roomId, "role:everyone", 0, 0);
                    }
                }
            }
        }

        // --- Per-role overrides ---
        Text {
            text: "Role overrides"
            font.pixelSize: Theme.fontSizeNormal
            font.bold: true
            color: Theme.textPrimary
        }
        Text {
            Layout.fillWidth: true
            text: "Allow = grants the permission in this channel. Deny = removes it. Neutral = inherits from role."
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.textMuted
            wrapMode: Text.WordWrap
        }

        // Role picker
        ComboBox {
            id: roleCombo
            Layout.fillWidth: true
            model: serverManager.activeServer ? serverManager.activeServer.serverRoles : []
            textRole: "name"
            currentIndex: 0
        }

        // Allow/neutral/deny toggles for each channel flag against the selected role
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: channelSettings.channelFlags

            // Lookup current allow/deny for selected role
            function findOverride(targetKey) {
                if (!serverManager.activeServer) return {allow: 0, deny: 0};
                var list = serverManager.activeServer.channelOverrides(channelSettings.roomId);
                for (var i = 0; i < list.length; i++) {
                    if (list[i].target === targetKey) {
                        return { allow: Number(list[i].allowFlags), deny: Number(list[i].denyFlags) };
                    }
                }
                return {allow: 0, deny: 0};
            }

            delegate: Row {
                width: ListView.view.width
                height: 40
                spacing: Theme.spacingNormal

                Text {
                    text: modelData.label
                    font.pixelSize: Theme.fontSizeNormal
                    color: Theme.textPrimary
                    width: 180
                    anchors.verticalCenter: parent.verticalCenter
                }

                property var selectedRole: roleCombo.currentIndex >= 0 && roleCombo.model.length > 0
                    ? roleCombo.model[roleCombo.currentIndex]
                    : null
                property string targetKey: selectedRole
                    ? ("role:" + (selectedRole.id || selectedRole.name))
                    : ""
                property var current: parent.findOverride(targetKey)
                property int state:
                    (current.allow & modelData.flag) !== 0 ? 1 :
                    (current.deny & modelData.flag) !== 0 ? -1 : 0

                function setState(newState) {
                    if (!serverManager.activeServer) return;
                    var allow = current.allow & ~modelData.flag;
                    var deny = current.deny & ~modelData.flag;
                    if (newState === 1) allow |= modelData.flag;
                    else if (newState === -1) deny |= modelData.flag;
                    serverManager.activeServer.setChannelOverride(
                        channelSettings.roomId, targetKey, allow, deny);
                }

                Button {
                    text: "Allow"
                    anchors.verticalCenter: parent.verticalCenter
                    contentItem: Text { text: parent.text; color: parent.parent.state === 1 ? "white" : Theme.textMuted; font.pixelSize: Theme.fontSizeSmall; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: parent.parent.parent.state === 1 ? "#57f287" : Theme.bgMedium; radius: Theme.radiusSmall; implicitWidth: 64; implicitHeight: 28 }
                    onClicked: parent.setState(parent.state === 1 ? 0 : 1)
                }
                Button {
                    text: "Neutral"
                    anchors.verticalCenter: parent.verticalCenter
                    contentItem: Text { text: parent.text; color: parent.parent.state === 0 ? "white" : Theme.textMuted; font.pixelSize: Theme.fontSizeSmall; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: parent.parent.parent.state === 0 ? "#768390" : Theme.bgMedium; radius: Theme.radiusSmall; implicitWidth: 64; implicitHeight: 28 }
                    onClicked: parent.setState(0)
                }
                Button {
                    text: "Deny"
                    anchors.verticalCenter: parent.verticalCenter
                    contentItem: Text { text: parent.text; color: parent.parent.state === -1 ? "white" : Theme.textMuted; font.pixelSize: Theme.fontSizeSmall; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { color: parent.parent.parent.state === -1 ? "#ed4245" : Theme.bgMedium; radius: Theme.radiusSmall; implicitWidth: 64; implicitHeight: 28 }
                    onClicked: parent.setState(parent.state === -1 ? 0 : -1)
                }
            }
        }

        Button {
            Layout.alignment: Qt.AlignRight
            text: "Close"
            contentItem: Text { text: parent.text; color: "white"; font.pixelSize: Theme.fontSizeNormal; horizontalAlignment: Text.AlignHCenter }
            background: Rectangle { color: parent.hovered ? Theme.accentHover : Theme.accent; radius: Theme.radiusSmall; implicitHeight: Theme.buttonHeight; implicitWidth: 100 }
            onClicked: channelSettings.close()
        }
    }
}
