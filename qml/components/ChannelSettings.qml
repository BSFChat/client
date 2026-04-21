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
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1
    }

    // ----- Reusable subcomponents -----

    // Group header — widest-tracked small-caps fg3 label with a `line`
    // divider filling the rest of the row. Matches the rest of the app's
    // label vocabulary.
    component SectionHeader: Item {
        property alias text: label.text
        Layout.fillWidth: true
        Layout.preferredHeight: 32
        Text {
            id: label
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.xs
            font.weight: Theme.fontWeight.semibold
            font.letterSpacing: Theme.trackWidest.xs
            color: Theme.fg3
        }
        Rectangle {
            anchors.left: label.right
            anchors.leftMargin: Theme.sp.s4
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            height: 1
            color: Theme.line
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
        spacing: Theme.sp.s7

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Text {
                text: title
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.md
                font.bold: true
                color: Theme.fg0
            }
            Text {
                visible: description.length > 0
                Layout.fillWidth: true
                text: description
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
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

    // 3-segment Allow / Neutral / Deny control (SPEC §3.10 Segment pattern).
    // state = -1 deny, 0 neutral, 1 allow. Emits stateChangeRequested(newState).
    //
    // Visual: all three segments share a single rounded container with the
    // selected segment raised onto a semantic-coloured pill. Colors come
    // from Theme.online / Theme.fg3 / Theme.danger so they track the
    // active theme (dark / light) and stay consistent with the rest of
    // the app rather than hard-coded Discord green/red.
    component TriToggle: Rectangle {
        id: tri
        property int state: 0
        signal stateChangeRequested(int newState)

        implicitWidth: 228
        implicitHeight: 30
        radius: Theme.r2
        color: Theme.bg0
        border.color: Theme.line
        border.width: 1

        Row {
            anchors.fill: parent
            anchors.margins: 2
            spacing: 2
            Repeater {
                model: [
                    {label: "Allow",   value:  1, role: "online" },
                    {label: "Neutral", value:  0, role: "neutral"},
                    {label: "Deny",    value: -1, role: "danger" }
                ]
                delegate: Rectangle {
                    width: (tri.width - 4 - 4) / 3   // container - margins - 2×2 gaps
                    height: parent.height
                    radius: Theme.r1
                    readonly property bool isSelected: tri.state === modelData.value
                    readonly property color roleColor:
                        modelData.role === "online" ? Theme.online
                      : modelData.role === "danger" ? Theme.danger
                                                     : Theme.fg3
                    color: isSelected ? roleColor
                         : segHover.containsMouse ? Theme.bg3
                         : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                    Text {
                        anchors.centerIn: parent
                        text: modelData.label
                        color: parent.isSelected ? Theme.onAccent : Theme.fg2
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                        font.weight: parent.isSelected
                                     ? Theme.fontWeight.semibold
                                     : Theme.fontWeight.medium
                    }
                    MouseArea {
                        id: segHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: tri.stateChangeRequested(modelData.value)
                    }
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
                anchors.leftMargin: Theme.sp.s7
                anchors.rightMargin: Theme.sp.s7
                Text {
                    Layout.fillWidth: true
                    text: channelSettings.roomName
                        ? ("Channel settings — #" + channelSettings.roomName)
                        : "Channel settings"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xl
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackTight.xl
                    color: Theme.fg0
                    elide: Text.ElideRight
                }
                // Close X as a proper icon button rather than a unicode glyph.
                Rectangle {
                    Layout.alignment: Qt.AlignVCenter
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    radius: Theme.r1
                    color: closeXMouse.containsMouse ? Theme.bg3 : "transparent"
                    Icon {
                        anchors.centerIn: parent
                        name: "x"
                        size: 16
                        color: closeXMouse.containsMouse ? Theme.fg0 : Theme.fg2
                    }
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
                color: Theme.line
            }
        }

        // Scrollable body
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: channelSettings.width - Theme.sp.s7 * 2
                x: Theme.sp.s7
                y: Theme.sp.s7
                spacing: Theme.sp.s7 * 1.25

                // ====== OVERVIEW ======
                SectionHeader { text: "OVERVIEW" }

                // Channel name. Rebuilds on each open from the
                // RoomListModel; the save handler writes m.room.name
                // and the sync echo flows back into the sidebar.
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp.s1
                    Text {
                        text: "CHANNEL NAME"
                        color: Theme.fg3
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                        font.weight: Theme.fontWeight.semibold
                        font.letterSpacing: Theme.trackWidest.xs
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.sp.s3

                        TextField {
                            id: nameField
                            Layout.fillWidth: true
                            Layout.maximumWidth: 420
                            color: Theme.fg0
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.md
                            background: Rectangle {
                                color: Theme.bg0
                                radius: Theme.r2
                                border.color: nameField.activeFocus ? Theme.accent : Theme.line
                                border.width: 1
                            }
                            leftPadding: Theme.sp.s4
                            rightPadding: Theme.sp.s4
                            topPadding: Theme.sp.s3
                            bottomPadding: Theme.sp.s3

                            // Original value — seeded when the popup
                            // opens, refreshed when the server's sync
                            // echo bumps `roomName`. Used to disable the
                            // Save button when nothing's changed and to
                            // revert on Esc.
                            property string _original: ""
                            function _resync() {
                                _original = channelSettings.roomName;
                                text = _original;
                            }

                            Keys.onEscapePressed: _resync()
                            Keys.onReturnPressed: saveNameBtn.clicked()
                        }
                        Button {
                            id: saveNameBtn
                            enabled: nameField.text.trim().length > 0
                                  && nameField.text !== nameField._original
                            contentItem: Text {
                                text: "Save"
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.md
                                font.weight: Theme.fontWeight.semibold
                                color: saveNameBtn.enabled ? Theme.onAccent : Theme.fg3
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                color: !saveNameBtn.enabled ? Theme.bg2
                                     : (saveNameBtn.hovered ? Theme.accentDim : Theme.accent)
                                radius: Theme.r2
                                implicitWidth: 80
                                implicitHeight: 36
                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                            }
                            onClicked: {
                                if (!serverManager.activeServer) return;
                                serverManager.activeServer.setRoomName(
                                    channelSettings.roomId, nameField.text.trim());
                                nameField._original = nameField.text;
                            }
                        }
                    }
                }

                // Channel topic (Matrix m.room.topic). Plain single-line
                // for now — Matrix topics are descriptive text, not
                // formatted, so a TextField is sufficient.
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp.s1
                    Text {
                        text: "TOPIC"
                        color: Theme.fg3
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                        font.weight: Theme.fontWeight.semibold
                        font.letterSpacing: Theme.trackWidest.xs
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.sp.s3

                        TextField {
                            id: topicField
                            Layout.fillWidth: true
                            placeholderText: "Describe what this channel is for"
                            placeholderTextColor: Theme.fg3
                            color: Theme.fg0
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.md
                            background: Rectangle {
                                color: Theme.bg0
                                radius: Theme.r2
                                border.color: topicField.activeFocus ? Theme.accent : Theme.line
                                border.width: 1
                            }
                            leftPadding: Theme.sp.s4
                            rightPadding: Theme.sp.s4
                            topPadding: Theme.sp.s3
                            bottomPadding: Theme.sp.s3

                            property string _original: ""
                            function _resync() {
                                if (!serverManager.activeServer
                                    || !serverManager.activeServer.roomListModel) {
                                    _original = "";
                                } else {
                                    _original = serverManager.activeServer.roomListModel
                                        .roomTopic(channelSettings.roomId);
                                }
                                text = _original;
                            }

                            Keys.onEscapePressed: _resync()
                            Keys.onReturnPressed: saveTopicBtn.clicked()
                        }
                        Button {
                            id: saveTopicBtn
                            enabled: topicField.text !== topicField._original
                            contentItem: Text {
                                text: "Save"
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.md
                                font.weight: Theme.fontWeight.semibold
                                color: saveTopicBtn.enabled ? Theme.onAccent : Theme.fg3
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                color: !saveTopicBtn.enabled ? Theme.bg2
                                     : (saveTopicBtn.hovered ? Theme.accentDim : Theme.accent)
                                radius: Theme.r2
                                implicitWidth: 80
                                implicitHeight: 36
                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                            }
                            onClicked: {
                                if (!serverManager.activeServer) return;
                                serverManager.activeServer.setRoomTopic(
                                    channelSettings.roomId, topicField.text);
                                topicField._original = topicField.text;
                            }
                        }
                    }
                }

                // Re-sync the editable fields whenever the popup opens
                // so the user sees the freshest server-side values
                // (not stale text from a previous edit session).
                Connections {
                    target: channelSettings
                    function onOpened() {
                        nameField._resync();
                        topicField._resync();
                    }
                }

                SettingRow {
                    title: "Slowmode"
                    description: "Members must wait this long between messages. Users with Manage messages bypass."
                    ThemedComboBox {
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

                    ThemedSwitch {
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
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    color: Theme.fg2
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp.s3
                    Text {
                        text: "ROLE"
                        color: Theme.fg3
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                        font.weight: Theme.fontWeight.semibold
                        font.letterSpacing: Theme.trackWidest.xs
                    }
                    ThemedComboBox {
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
                            implicitHeight: permRow.implicitHeight + Theme.sp.s4 * 2
                            radius: Theme.r2
                            color: Theme.bg2
                            border.color: Theme.line
                            border.width: 1

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
                                anchors.leftMargin: Theme.sp.s5
                                anchors.rightMargin: Theme.sp.s4
                                spacing: Theme.sp.s4

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Text {
                                        text: flagInfo.label
                                        color: Theme.fg0
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.md
                                    }
                                    Text {
                                        visible: flagInfo.hint.length > 0
                                        Layout.fillWidth: true
                                        text: flagInfo.hint
                                        color: Theme.fg2
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.sm
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

                Item { Layout.preferredHeight: Theme.sp.s3 }
            }
        }

        // Footer — changes save on toggle, so this is just a dismissal rail.
        // Keep it quiet: top hairline + right-aligned accent "Done".
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 64
            color: Theme.bg0
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
                    text: "Changes save automatically."
                    color: Theme.fg3
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                }
                Rectangle {
                    id: doneBtn
                    Layout.preferredWidth: 120
                    Layout.preferredHeight: 36
                    radius: Theme.r2
                    color: doneBtnMouse.containsMouse ? Theme.accentDim : Theme.accent
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    Text {
                        anchors.centerIn: parent
                        text: "Done"
                        color: Theme.onAccent
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.semibold
                    }
                    MouseArea {
                        id: doneBtnMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: channelSettings.close()
                    }
                }
            }
        }
    }
}
