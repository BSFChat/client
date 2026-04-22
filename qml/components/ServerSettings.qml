import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import BSFChat

Popup {
    id: serverSettingsPopup
    anchors.centerIn: Overlay.overlay
    width: parent ? parent.width * 0.85 : 800
    height: parent ? parent.height * 0.85 : 600
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    property int selectedSection: 0

    // Emitted when the user clicks a channel row in the Channels tab. The
    // host component is expected to open its ChannelSettings instance for
    // the given room (we don't have access to it from here — it's owned by
    // ChannelList.qml alongside this popup).
    signal channelSettingsRequested(string roomId, string roomName)

    // Emitted when the user clicks the trash icon on a channel row. Host
    // opens its delete-confirmation popup; we don't close the settings
    // popup because the user is mid-task and may delete several in a row.
    signal channelDeleteRequested(string roomId, string roomName)

    // Emitted by the "+ Add channel" / "+ Add category" affordances. Host
    // opens its create-prompt popup seeded with the right kind/parent.
    //   kind: "category" | "text" | "voice"
    //   categoryId: "" for top-level create or for kind=="category"
    signal createChannelRequested(string kind, string categoryId)

    // Per-tab header (24px title + divider rule). SPEC §3.10 section-header.
    // Factored here so each tab body doesn't redeclare the five-property
    // ColumnLayout → Text → Rectangle pattern.
    component TabHeader: ColumnLayout {
        property string title: ""
        Layout.fillWidth: true
        spacing: Theme.sp.s3
        Text {
            text: parent.title
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.xxl
            font.weight: Theme.fontWeight.semibold
            font.letterSpacing: Theme.trackTight.xxl
            color: Theme.fg0
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.line }
    }

    // Permission bit definitions mirrored from protocol/include/bsfchat/Permissions.h.
    // Each entry has {key, label, flag} where flag is the bit value.
    readonly property var permissionFlags: [
        {key: "view",     label: "View channels",      flag: 0x0001},
        {key: "send",     label: "Send messages",      flag: 0x0002},
        {key: "attach",   label: "Attach files",       flag: 0x0004},
        {key: "embed",    label: "Embed links",        flag: 0x0008},
        {key: "manmsg",   label: "Manage messages",    flag: 0x0010},
        {key: "manchan",  label: "Manage channels",    flag: 0x0020},
        {key: "manrole",  label: "Manage roles",       flag: 0x0040},
        {key: "kick",     label: "Kick members",       flag: 0x0080},
        {key: "ban",      label: "Ban members",        flag: 0x0100},
        {key: "mentall",  label: "Mention @everyone",  flag: 0x0200},
        {key: "manserv",  label: "Manage server",      flag: 0x0400},
        {key: "admin",    label: "Administrator",      flag: 0x8000}
    ]

    property string editingRoleId: ""
    property string editingMemberId: ""

    // Swap the role at `index` with its neighbour in the given
    // direction (-1 = up, +1 = down). We swap BOTH array order and
    // position fields: the UI renders in array order (the ListView
    // iterates as the server serialises), and the server's permission
    // engine sorts by position — so if we only swapped positions the
    // UI would look unchanged until the next reload. Keeping both in
    // sync means the rebuilt view matches what we just asked for.
    function moveRole(index, direction) {
        if (!serverManager.activeServer) return;
        var roles = serverManager.activeServer.serverRoles;
        if (!roles || roles.length === 0) return;
        var other = index + direction;
        if (other < 0 || other >= roles.length) return;

        // Deep copy so we don't mutate the live Q_PROPERTY payload.
        var out = [];
        for (var i = 0; i < roles.length; i++) {
            var r = {};
            for (var k in roles[i]) r[k] = roles[i][k];
            out.push(r);
        }
        // Swap positions first so that sorting by position downstream
        // still lines up with our array order.
        var p1 = out[index].position;
        var p2 = out[other].position;
        if (p1 === p2) p2 = p1 + direction;
        out[index].position = p2;
        out[other].position = p1;
        // Then swap the array slots so the ListView reflects the
        // change immediately (sync echo will confirm the same shape).
        var tmp = out[index];
        out[index] = out[other];
        out[other] = tmp;
        serverManager.activeServer.updateServerRoles(out);
    }

    // Transient state for the kick/ban/unban confirm dialog. Populated by
    // the row that initiated the action; consumed on confirm.
    //   { kind: "kick" | "ban" | "unban", userId, displayName }
    property var confirmMod: ({ kind: "", userId: "", displayName: "" })
    property alias confirmModDialog: _confirmModDialog

    // Inline error toast state. Populated by the stateWriteFailed handler
    // on the active server; auto-clears after 5 seconds.
    property string _toastMessage: ""
    Timer {
        id: _toastTimer
        interval: 5000
        onTriggered: serverSettingsPopup._toastMessage = ""
    }
    Connections {
        target: serverManager.activeServer
        function onStateWriteFailed(kind, status, error) {
            var prefix;
            switch (kind) {
                case "role-assign":
                    prefix = "Couldn't save role assignments";
                    break;
                case "server-name":
                    prefix = "Couldn't update server name";
                    break;
                case "channel-override":
                    prefix = "Couldn't save channel permissions";
                    break;
                case "channel-settings":
                    prefix = "Couldn't update channel settings";
                    break;
                case "server-roles":
                    prefix = "Couldn't save server roles";
                    break;
                default:
                    prefix = "Save failed";
            }
            var suffix = status === 403
                ? "you don't have permission."
                : (error || ("HTTP " + status));
            serverSettingsPopup._toastMessage = prefix + " — " + suffix;
            _toastTimer.restart();
        }
    }

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1

        // Inline error toast — slides in from the top when the active
        // server's stateWriteFailed signal fires, auto-dismisses after 5s.
        // Anchored inside the popup's background so it floats above content
        // at any tab.
        Rectangle {
            id: _errorToast
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.topMargin: serverSettingsPopup._toastMessage !== ""
                               ? Theme.sp.s5 : -_errorToast.height - Theme.sp.s5
            width: Math.min(parent.width - Theme.sp.s7 * 2, 520)
            height: _errorToastCol.implicitHeight + Theme.sp.s4 * 2
            radius: Theme.r2
            color: Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.14)
            border.color: Theme.danger
            border.width: 1
            z: 20
            visible: anchors.topMargin > -_errorToast.height
            Behavior on anchors.topMargin {
                NumberAnimation { duration: 220; easing.type: Easing.OutCubic }
            }

            RowLayout {
                id: _errorToastCol
                anchors.fill: parent
                anchors.leftMargin: Theme.sp.s5
                anchors.rightMargin: Theme.sp.s4
                anchors.topMargin: Theme.sp.s4
                anchors.bottomMargin: Theme.sp.s4
                spacing: Theme.sp.s3
                Icon {
                    name: "x"
                    size: 14
                    color: Theme.danger
                    Layout.alignment: Qt.AlignTop
                    Layout.topMargin: 2
                }
                Text {
                    Layout.fillWidth: true
                    text: serverSettingsPopup._toastMessage
                    color: Theme.fg0
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    wrapMode: Text.WordWrap
                }
                Rectangle {
                    Layout.preferredWidth: 20
                    Layout.preferredHeight: 20
                    Layout.alignment: Qt.AlignTop
                    radius: Theme.r1
                    color: _toastDismissMouse.containsMouse
                           ? Qt.rgba(0, 0, 0, 0.15) : "transparent"
                    Text {
                        anchors.centerIn: parent
                        text: "×"
                        color: Theme.fg1
                        font.pixelSize: 14
                    }
                    MouseArea {
                        id: _toastDismissMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: serverSettingsPopup._toastMessage = ""
                    }
                }
            }
        }

        // Top-right close X — floats on top of whatever the content is,
        // matches the ChannelSettings pattern. Esc / click-outside still
        // work the same way.
        Rectangle {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: Theme.sp.s5
            anchors.rightMargin: Theme.sp.s5
            width: 28; height: 28
            radius: Theme.r1
            color: closeXMouse.containsMouse ? Theme.bg3 : "transparent"
            z: 10
            Icon {
                anchors.centerIn: parent
                name: "x"
                size: 14
                color: closeXMouse.containsMouse ? Theme.fg0 : Theme.fg2
            }
            MouseArea {
                id: closeXMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: serverSettingsPopup.close()
            }
        }
    }

    contentItem: RowLayout {
        spacing: 0

        // Left nav sidebar
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 200
            color: Theme.bg0
            radius: Theme.r2

            // Clip right radius
            Rectangle {
                anchors.right: parent.right
                width: Theme.r2
                height: parent.height
                color: Theme.bg0
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.sp.s3
                spacing: 2

                Text {
                    text: "SERVER SETTINGS"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xs
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackWidest.xs
                    color: Theme.fg3
                    Layout.leftMargin: Theme.sp.s3
                    Layout.topMargin: Theme.sp.s3
                    Layout.bottomMargin: Theme.sp.s3
                }

                Repeater {
                    model: ["Overview", "Roles", "Members", "Channels", "Bans"]
                    delegate: Rectangle {
                        Layout.fillWidth: true
                        height: 36
                        radius: Theme.r1
                        readonly property bool isActive: selectedSection === index
                        color: isActive ? Theme.bg3
                             : navItemMouse.containsMouse ? Theme.bg2
                             : "transparent"
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: Theme.sp.s3
                            text: modelData
                            color: parent.isActive ? Theme.fg0 : Theme.fg1
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.md
                            font.weight: parent.isActive
                                         ? Theme.fontWeight.semibold
                                         : Theme.fontWeight.medium
                        }

                        MouseArea {
                            id: navItemMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: selectedSection = index
                        }
                    }
                }

                // Close-nav row removed; the dialog is dismissed via the
                // top-right X (see below) or Esc / click-outside.
                Item { Layout.fillHeight: true }
            }
        }

        // Content area
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: selectedSection

            // ---- Overview (index 0) ----
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.sp.s7 * 2
                    spacing: Theme.sp.s7

                    TabHeader { title: "Server Overview" }

                    // Server icon — 80×80 preview on the left, Upload /
                    // Remove actions on the right. Icon is a resolved
                    // http URL (mxc → media endpoint). Fallback is the
                    // server's initial letter in an accent tile.
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.maximumWidth: 420
                        spacing: Theme.sp.s5

                        Rectangle {
                            Layout.preferredWidth: 80
                            Layout.preferredHeight: 80
                            radius: Theme.r3
                            color: Theme.bg2
                            border.color: Theme.line
                            border.width: 1
                            clip: true

                            Text {
                                anchors.centerIn: parent
                                visible: serverIconPreview.status !== Image.Ready
                                text: {
                                    var n = serverManager.activeServer
                                        ? serverManager.activeServer.serverName : "?";
                                    var stripped = (n || "?").replace(/^[^a-zA-Z0-9]+/, "");
                                    return (stripped.charAt(0) || "?").toUpperCase();
                                }
                                font.family: Theme.fontSans
                                font.pixelSize: 32
                                font.weight: Theme.fontWeight.semibold
                                color: Theme.fg1
                            }

                            Image {
                                id: serverIconPreview
                                anchors.fill: parent
                                anchors.margins: 1
                                source: serverManager.activeServer
                                    ? serverManager.activeServer.serverAvatarUrl : ""
                                visible: status === Image.Ready
                                fillMode: Image.PreserveAspectCrop
                                smooth: true
                                asynchronous: true
                                cache: true
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.sp.s2

                            Text {
                                text: "SERVER ICON"
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.xs
                                font.weight: Theme.fontWeight.semibold
                                font.letterSpacing: Theme.trackWidest.xs
                                color: Theme.fg3
                            }

                            Text {
                                text: "Square image, at least 128×128. PNG, JPEG, GIF or WebP."
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.sm
                                color: Theme.fg2
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            RowLayout {
                                spacing: Theme.sp.s3
                                Layout.topMargin: Theme.sp.s2

                                Button {
                                    id: uploadIconBtn
                                    text: "Upload icon…"
                                    contentItem: Text {
                                        text: uploadIconBtn.text
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.sm
                                        font.weight: Theme.fontWeight.medium
                                        color: Theme.fg0
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    background: Rectangle {
                                        color: uploadIconBtn.hovered ? Theme.bg3 : Theme.bg2
                                        border.color: Theme.line
                                        border.width: 1
                                        radius: Theme.r2
                                        implicitWidth: 120
                                        implicitHeight: 32
                                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                    }
                                    onClicked: serverIconFileDialog.open()
                                }
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.maximumWidth: 420
                        Layout.preferredHeight: 1
                        color: Theme.lineSoft
                    }

                    Text {
                        text: "SERVER NAME"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                        font.weight: Theme.fontWeight.semibold
                        font.letterSpacing: Theme.trackWidest.xs
                        color: Theme.fg3
                    }

                    TextField {
                        id: serverNameField
                        Layout.fillWidth: true
                        Layout.maximumWidth: 420
                        // Shown to all users across the server (channel-list
                        // header, tooltips, etc.). Only editable by users
                        // with MANAGE_SERVER; server enforces regardless.
                        text: serverManager.activeServer ? serverManager.activeServer.serverName : ""
                        color: Theme.fg0
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        background: Rectangle {
                            color: Theme.bg0
                            radius: Theme.r2
                            border.color: serverNameField.activeFocus ? Theme.accent : Theme.line
                            border.width: 1
                        }
                        leftPadding: Theme.sp.s4
                        rightPadding: Theme.sp.s4
                        topPadding: Theme.sp.s3
                        bottomPadding: Theme.sp.s3
                    }

                    // Primary action row — "Save" as a proper primary
                    // button, sized so it commands attention without
                    // dominating. Disabled until the field value differs
                    // from the currently persisted server name so the
                    // button reads the current state at a glance.
                    RowLayout {
                        Layout.topMargin: Theme.sp.s3
                        Layout.maximumWidth: 420

                        Button {
                            id: saveServerNameBtn
                            enabled: serverManager.activeServer
                                     && serverNameField.text.trim().length > 0
                                     && serverNameField.text.trim() !== serverManager.activeServer.serverName
                            contentItem: Text {
                                text: "Save changes"
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.md
                                font.weight: Theme.fontWeight.semibold
                                color: saveServerNameBtn.enabled ? Theme.onAccent : Theme.fg3
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                color: !saveServerNameBtn.enabled
                                       ? Theme.bg2
                                       : (saveServerNameBtn.hovered ? Theme.accentDim : Theme.accent)
                                radius: Theme.r2
                                implicitWidth: 140
                                implicitHeight: 36
                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                            }
                            onClicked: {
                                if (serverManager.activeServer) {
                                    serverManager.activeServer.updateServerName(serverNameField.text.trim());
                                }
                            }
                        }
                    }

                    InfoBanner {
                        Layout.maximumWidth: 420
                        icon: "shield"
                        text: "Only members with the Manage Server permission can change the server name. Everyone else sees the field read-only."
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.maximumWidth: 420
                        Layout.topMargin: Theme.sp.s5
                        Layout.preferredHeight: 1
                        color: Theme.lineSoft
                    }

                    Text {
                        text: "SCREEN-SHARE MAXIMUM QUALITY"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                        font.weight: Theme.fontWeight.semibold
                        font.letterSpacing: Theme.trackWidest.xs
                        color: Theme.fg3
                    }

                    Text {
                        Layout.maximumWidth: 420
                        text: "Cap the highest quality preset users may pick when "
                            + "sharing their screen in voice channels. Lower caps "
                            + "protect bandwidth on busy servers; Ultra is uncapped."
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                        color: Theme.fg2
                        wrapMode: Text.WordWrap
                    }

                    ThemedComboBox {
                        id: serverMaxQualityCombo
                        Layout.maximumWidth: 260
                        textRole: "label"
                        model: [
                            { label: "Low (2 fps · 960 px · Q40)",   value: 0 },
                            { label: "Medium (5 fps · 1280 px · Q60)", value: 1 },
                            { label: "High (10 fps · 1600 px · Q75)",  value: 2 },
                            { label: "Ultra (15 fps · 1920 px · Q85)", value: 3 }
                        ]
                        enabled: {
                            var s = serverManager.activeServer;
                            if (!s) return false;
                            if (s.permissionsGeneration < 0) return false;
                            // Reuse manage-channel on the active room as a
                            // proxy for "server admin" until a proper
                            // manage-server check lands.
                            return s.canManageChannel(s.activeRoomId);
                        }
                        Component.onCompleted: {
                            var s = serverManager.activeServer;
                            currentIndex = s ? s.maxScreenShareQuality : 3;
                        }
                        onActivated: {
                            if (!serverManager.activeServer) return;
                            var v = model[currentIndex].value;
                            serverManager.activeServer.setMaxScreenShareQuality(v);
                        }
                        // Reflect live updates (e.g. from another device).
                        Connections {
                            target: serverManager.activeServer
                            ignoreUnknownSignals: true
                            function onMaxScreenShareQualityChanged() {
                                serverMaxQualityCombo.currentIndex =
                                    serverManager.activeServer.maxScreenShareQuality;
                            }
                        }
                    }

                    InfoBanner {
                        Layout.maximumWidth: 420
                        icon: "shield"
                        text: "Only members with the Manage Server permission can change this cap."
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            // ---- Roles (index 1) ----
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.sp.s7 * 2
                    spacing: Theme.sp.s7

                    TabHeader { title: "Roles" }

                    Text {
                        Layout.fillWidth: true
                        text: "Roles grant permissions server-wide. Assign them to members in the Members tab; override per-channel in each channel's settings."
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                        color: Theme.fg2
                        wrapMode: Text.WordWrap
                    }

                    // Roles list
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true

                        ScrollBar.vertical: ThemedScrollBar {}
                        model: serverManager.activeServer ? serverManager.activeServer.serverRoles : []
                        spacing: 4

                        delegate: Column {
                            id: roleDelegate
                            width: ListView.view ? ListView.view.width : 400
                            spacing: 2

                            property var role: modelData
                            property int roleIndex: index
                            property bool isEditing: serverSettingsPopup.editingRoleId === (role.id || role.name)
                            readonly property bool canMoveUp:
                                roleIndex > 0
                            readonly property bool canMoveDown: {
                                if (!serverManager.activeServer) return false;
                                return roleIndex < serverManager.activeServer.serverRoles.length - 1;
                            }

                            Rectangle {
                                width: parent.width
                                height: 48
                                radius: Theme.r2
                                color: roleRowMouse.containsMouse ? Theme.bg3 : Theme.bg2
                                border.color: roleDelegate.isEditing ? Theme.accent : Theme.line
                                border.width: 1
                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                Behavior on border.color { ColorAnimation { duration: Theme.motion.fastMs } }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: Theme.sp.s5
                                    anchors.rightMargin: Theme.sp.s3
                                    spacing: Theme.sp.s4

                                    // Role color chip — slightly larger so
                                    // it reads as a real identifier.
                                    Rectangle {
                                        width: 18; height: 18; radius: 9
                                        color: roleDelegate.role.color || Theme.accent
                                        border.color: Theme.bg0
                                        border.width: 1
                                    }
                                    Text {
                                        text: roleDelegate.role.name || ""
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.md
                                        font.weight: Theme.fontWeight.semibold
                                        color: Theme.fg0
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                    }

                                    // Reorder chevrons — reveal-on-hover so
                                    // the resting list stays quiet. Click
                                    // swaps the role with its neighbour's
                                    // position and writes the new ordering
                                    // back. Disabled at the ends of the
                                    // list so `position`s stay unique.
                                    component ReorderBtn: Rectangle {
                                        id: rbtn
                                        property string iconName: ""
                                        property bool disabled: false
                                        signal clicked()
                                        Layout.preferredWidth: 22
                                        Layout.preferredHeight: 22
                                        radius: Theme.r1
                                        color: rbtnMouse.containsMouse && !disabled
                                            ? Theme.bg2 : "transparent"
                                        opacity: disabled
                                            ? 0.25
                                            : (roleRowMouse.containsMouse
                                               || rbtnMouse.containsMouse ? 1.0 : 0.0)
                                        Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }
                                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                        Icon {
                                            anchors.centerIn: parent
                                            name: rbtn.iconName
                                            size: 12
                                            color: rbtnMouse.containsMouse && !rbtn.disabled
                                                ? Theme.fg0 : Theme.fg2
                                        }
                                        MouseArea {
                                            id: rbtnMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            enabled: !rbtn.disabled
                                            cursorShape: rbtn.disabled
                                                ? Qt.ArrowCursor : Qt.PointingHandCursor
                                            onClicked: rbtn.clicked()
                                        }
                                    }

                                    ReorderBtn {
                                        iconName: "chevron-right"
                                        rotation: -90          // point up
                                        disabled: !roleDelegate.canMoveUp
                                        onClicked: serverSettingsPopup.moveRole(
                                            roleDelegate.roleIndex, -1)
                                    }
                                    ReorderBtn {
                                        iconName: "chevron-right"
                                        rotation: 90           // point down
                                        disabled: !roleDelegate.canMoveDown
                                        onClicked: serverSettingsPopup.moveRole(
                                            roleDelegate.roleIndex, 1)
                                    }

                                    // Expand / collapse chevron — rotates
                                    // 90° rather than swapping glyphs.
                                    Icon {
                                        name: "chevron-right"
                                        size: 14
                                        color: Theme.fg2
                                        rotation: roleDelegate.isEditing ? 90 : 0
                                        Behavior on rotation {
                                            NumberAnimation { duration: Theme.motion.fastMs
                                                              easing.type: Easing.BezierSpline
                                                              easing.bezierCurve: Theme.motion.bezier }
                                        }
                                    }
                                }

                                MouseArea {
                                    id: roleRowMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    // Sit BEHIND the chevron buttons in
                                    // z-order so their MouseAreas get
                                    // clicks first and don't propagate
                                    // back to the row's expand/collapse
                                    // handler. In QML later-declared
                                    // items stack on top; we declared
                                    // the RowLayout first, so without
                                    // a negative z this MouseArea
                                    // overlays the chevrons and steals
                                    // their clicks.
                                    z: -1
                                    onClicked: {
                                        var rid = roleDelegate.role.id || roleDelegate.role.name;
                                        serverSettingsPopup.editingRoleId =
                                            roleDelegate.isEditing ? "" : rid;
                                    }
                                }
                            }

                            // Inline editor
                            Rectangle {
                                id: roleEditCard
                                width: parent.width
                                visible: parent.isEditing
                                radius: Theme.r2
                                color: Theme.bg0
                                border.color: Theme.line
                                border.width: 1
                                height: visible ? roleEditCol.implicitHeight + Theme.sp.s7 * 2 : 0

                                // Single source of truth for the form.
                                // editRole binds to the delegate's role, so
                                // scratch values reset whenever the user opens
                                // a different role. Falls back to an empty
                                // object so the descendant bindings don't
                                // explode into "Cannot read property X of
                                // undefined" during the brief window between
                                // the surrounding Column being recycled and
                                // the new role reference landing (e.g. after
                                // a role is deleted).
                                // Directly reference the enclosing delegate's
                                // role. The old `parent.parent.role` walked
                                // up past the Column into the ListView's
                                // contentItem (which has no `role` property)
                                // so editRole was always `({})` — harmless-
                                // looking because the descendant bindings
                                // all short-circuit to defaults, but it
                                // meant the Save button's `editRoleName`
                                // (which depends on `editRole.name`) had
                                // nothing to fall back to and saves looked
                                // like they silently cleared the name.
                                readonly property var editRole: roleDelegate.role || ({})
                                property string scratchColor: editRole.color || "#5865f2"
                                property int scratchPos: editRole.position !== undefined
                                    ? editRole.position : 0
                                // Permissions as a raw bitfield (QML number).
                                // Parsed from the role's hex string.
                                property double scratchPerms: {
                                    var p = editRole.permissions;
                                    if (typeof p === "string") {
                                        var s = p;
                                        if (s.indexOf("0x") === 0 || s.indexOf("0X") === 0) s = s.substr(2);
                                        return parseInt(s, 16) || 0;
                                    }
                                    return p || 0;
                                }
                                function togglePerm(flag) {
                                    // XOR the flag in/out of the bitfield.
                                    scratchPerms = (Number(scratchPerms) ^ flag);
                                }
                                function hasPerm(flag) {
                                    return (Number(scratchPerms) & flag) !== 0;
                                }

                                ColumnLayout {
                                    id: roleEditCol
                                    anchors.fill: parent
                                    anchors.margins: Theme.sp.s7
                                    spacing: Theme.sp.s3

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.sp.s3

                                        TextField {
                                            id: editRoleName
                                            Layout.fillWidth: true
                                            text: roleEditCard.editRole.name || ""
                                            color: Theme.fg0
                                            font.family: Theme.fontSans
                                            font.pixelSize: Theme.fontSize.md
                                            background: Rectangle {
                                                color: Theme.bg1
                                                radius: Theme.r2
                                                border.color: editRoleName.activeFocus ? Theme.accent : Theme.line
                                                border.width: 1
                                            }
                                            leftPadding: Theme.sp.s4
                                            rightPadding: Theme.sp.s4
                                            topPadding: Theme.sp.s3
                                            bottomPadding: Theme.sp.s3
                                        }

                                        // SpinBox keeps Qt Controls chrome for its
                                        // up/down buttons but we swap in a themed
                                        // background and centre the value.
                                        SpinBox {
                                            id: editRolePosition
                                            from: 0; to: 1000
                                            value: roleEditCard.scratchPos
                                            onValueModified: roleEditCard.scratchPos = value
                                            font.family: Theme.fontMono
                                            font.pixelSize: Theme.fontSize.md
                                            background: Rectangle {
                                                color: Theme.bg1
                                                radius: Theme.r2
                                                border.color: Theme.line
                                                border.width: 1
                                                implicitWidth: 100
                                                implicitHeight: 36
                                            }
                                        }
                                    }

                                    // Role color palette — the four Designer accents up
                                    // front, then a wider gamut of classic chat role
                                    // colors. Selected swatch gets an fg0 ring.
                                    Row {
                                        spacing: Theme.sp.s2
                                        Repeater {
                                            model: [
                                                "#36d6c7", "#a28bff", "#ec6dd6", "#ffa34a",
                                                "#57f287", "#fee75c", "#ed4245", "#f47067",
                                                "#39c5cf", "#dcbdfb", "#f69d50", "#768390"
                                            ]
                                            delegate: Rectangle {
                                                width: 22; height: 22; radius: 11
                                                color: modelData
                                                readonly property bool selected:
                                                    roleEditCard.scratchColor === modelData
                                                border.color: selected ? Theme.fg0 : Theme.line
                                                border.width: selected ? 3 : 1
                                                Behavior on border.width {
                                                    NumberAnimation { duration: Theme.motion.fastMs }
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: roleEditCard.scratchColor = modelData
                                                }
                                            }
                                        }
                                    }

                                    Text {
                                        Layout.topMargin: Theme.sp.s3
                                        text: "PERMISSIONS"
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.xs
                                        font.weight: Theme.fontWeight.semibold
                                        font.letterSpacing: Theme.trackWidest.xs
                                        color: Theme.fg3
                                    }

                                    Grid {
                                        Layout.fillWidth: true
                                        columns: 2
                                        columnSpacing: Theme.sp.s7
                                        rowSpacing: Theme.sp.s1

                                        Repeater {
                                            model: serverSettingsPopup.permissionFlags
                                            delegate: Row {
                                                spacing: 6
                                                ThemedCheckBox {
                                                    id: cb
                                                    // Bind directly to the bitfield. The .scratchPerms
                                                    // access registers the dep so checkbox state stays
                                                    // in sync with togglePerm() mutations and role
                                                    // switches.
                                                    checked: roleEditCard.hasPerm(modelData.flag)
                                                    onToggled: roleEditCard.togglePerm(modelData.flag)
                                                }
                                                Text {
                                                    text: modelData.label
                                                    color: Theme.fg0
                                                    font.family: Theme.fontSans
                                                    font.pixelSize: Theme.fontSize.sm
                                                    anchors.verticalCenter: cb.verticalCenter
                                                }
                                            }
                                        }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.topMargin: Theme.sp.s3
                                        spacing: Theme.sp.s3

                                        // Primary Save — accent pill.
                                        Button {
                                            id: roleSaveBtn
                                            contentItem: Text {
                                                text: "Save role"
                                                font.family: Theme.fontSans
                                                font.pixelSize: Theme.fontSize.md
                                                font.weight: Theme.fontWeight.semibold
                                                color: Theme.onAccent
                                                horizontalAlignment: Text.AlignHCenter
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                            background: Rectangle {
                                                color: roleSaveBtn.hovered ? Theme.accentDim : Theme.accent
                                                radius: Theme.r2
                                                implicitHeight: 36
                                                implicitWidth: 120
                                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                            }
                                            onClicked: {
                                                if (!serverManager.activeServer) return;
                                                var permsVal = Number(roleEditCard.scratchPerms) | 0;
                                                var existing = serverManager.activeServer.serverRoles;
                                                var out = [];
                                                var myId = roleEditCard.editRole.id || roleEditCard.editRole.name;
                                                for (var j = 0; j < existing.length; j++) {
                                                    var r = existing[j];
                                                    var rid = r.id || r.name;
                                                    if (rid === myId) {
                                                        out.push({
                                                            id: myId,
                                                            name: editRoleName.text.trim() || r.name,
                                                            color: roleEditCard.scratchColor || r.color,
                                                            position: roleEditCard.scratchPos,
                                                            permissions: "0x" + permsVal.toString(16),
                                                            mentionable: r.mentionable || false,
                                                            hoist: r.hoist || false
                                                        });
                                                    } else {
                                                        out.push(r);
                                                    }
                                                }
                                                serverManager.activeServer.updateServerRoles(out);
                                                serverSettingsPopup.editingRoleId = "";
                                            }
                                        }

                                        // Ghost danger Delete — hollow with danger
                                        // border, fills on hover.
                                        Button {
                                            id: roleDeleteBtn
                                            visible: (roleEditCard.editRole.id || roleEditCard.editRole.name) !== "everyone"
                                                  && (roleEditCard.editRole.id || roleEditCard.editRole.name) !== "admin"
                                            contentItem: Text {
                                                text: "Delete role"
                                                font.family: Theme.fontSans
                                                font.pixelSize: Theme.fontSize.md
                                                font.weight: Theme.fontWeight.semibold
                                                color: roleDeleteBtn.hovered ? Theme.onAccent : Theme.danger
                                                horizontalAlignment: Text.AlignHCenter
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                            background: Rectangle {
                                                color: roleDeleteBtn.hovered ? Theme.danger : "transparent"
                                                radius: Theme.r2
                                                border.color: Theme.danger
                                                border.width: 1
                                                implicitHeight: 36
                                                implicitWidth: 120
                                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                            }
                                            onClicked: {
                                                if (!serverManager.activeServer) return;
                                                var existing = serverManager.activeServer.serverRoles;
                                                var out = [];
                                                var myId = roleEditCard.editRole.id || roleEditCard.editRole.name;
                                                for (var j = 0; j < existing.length; j++) {
                                                    var r = existing[j];
                                                    var rid = r.id || r.name;
                                                    if (rid !== myId) out.push(r);
                                                }
                                                serverManager.activeServer.updateServerRoles(out);
                                                serverSettingsPopup.editingRoleId = "";
                                            }
                                        }

                                        Item { Layout.fillWidth: true }
                                    }
                                }
                            }
                        }

                        Text {
                            anchors.centerIn: parent
                            visible: parent.count === 0
                            text: "No roles configured — defaults will seed on first boot."
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.md
                            color: Theme.fg2
                        }
                    }

                    // Add-role footer — accent pill, r2/36h matching the
                    // rest of the settings button vocabulary. Inline icon +
                    // label instead of a unicode plus glyph.
                    Button {
                        id: addRoleBtn
                        contentItem: RowLayout {
                            spacing: Theme.sp.s2
                            Icon { name: "plus"; size: 14; color: Theme.onAccent; Layout.alignment: Qt.AlignVCenter }
                            Text {
                                text: "Add role"
                                color: Theme.onAccent
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.md
                                font.weight: Theme.fontWeight.semibold
                                Layout.alignment: Qt.AlignVCenter
                            }
                        }
                        background: Rectangle {
                            color: addRoleBtn.hovered ? Theme.accentDim : Theme.accent
                            radius: Theme.r2
                            implicitHeight: 36
                            implicitWidth: 140
                            Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                        }
                        onClicked: {
                            if (!serverManager.activeServer) return;
                            var existing = serverManager.activeServer.serverRoles || [];
                            var out = [];
                            var maxPos = 0;
                            for (var i = 0; i < existing.length; i++) {
                                out.push(existing[i]);
                                if (existing[i].position > maxPos) maxPos = existing[i].position;
                            }
                            var newId = "role-" + Date.now();
                            out.push({
                                id: newId,
                                name: "New Role",
                                // Default new-role swatch — Designer cyan.
                                // (Was Discord blurple; swapped for
                                // visual continuity with our accent.)
                                color: "#36d6c7",
                                position: maxPos + 1,
                                permissions: "0x080f", // everyone defaults
                                mentionable: false,
                                hoist: false
                            });
                            serverManager.activeServer.updateServerRoles(out);
                        }
                    }
                }
            }

            // ---- Members (index 2) ----
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.sp.s7 * 2
                    spacing: Theme.sp.s7

                    TabHeader { title: "Members" }

                    // Search field
                    TextField {
                        id: memberSearchField
                        Layout.fillWidth: true
                        Layout.maximumWidth: 400
                        placeholderText: "Search members..."
                        placeholderTextColor: Theme.fg2
                        color: Theme.fg0
                        font.pixelSize: Theme.fontSize.md
                        background: Rectangle {
                            color: Theme.bg0
                            radius: Theme.r2
                            border.color: memberSearchField.activeFocus ? Theme.accent : Theme.line
                            border.width: 1
                        }
                        padding: Theme.sp.s3
                    }

                    // Empty-state card when no members have synced yet.
                    // Mirrors the Bans-tab placeholder so the two tabs read
                    // as siblings.
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: {
                            if (!serverManager.activeServer) return true;
                            return serverManager.activeServer.serverMembers.length === 0;
                        }
                        ColumnLayout {
                            anchors.centerIn: parent
                            width: 360
                            spacing: Theme.sp.s4
                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 56
                                Layout.preferredHeight: 56
                                radius: Theme.r3
                                color: Theme.bg2
                                border.color: Theme.line
                                border.width: 1
                                Icon {
                                    anchors.centerIn: parent
                                    name: "users"
                                    size: 24
                                    color: Theme.fg3
                                }
                            }
                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: "No members synced yet"
                                color: Theme.fg0
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.lg
                                font.weight: Theme.fontWeight.semibold
                            }
                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                text: "Give it a moment — the member list populates from sync as each channel's state arrives."
                                color: Theme.fg2
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.sm
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        visible: serverManager.activeServer
                                 && serverManager.activeServer.serverMembers.length > 0

                        ScrollBar.vertical: ThemedScrollBar {}
                        // Server-wide member union (not the active-room model
                        // which only knows about the currently-selected
                        // channel). See ServerConnection::serverMembers.
                        model: serverManager.activeServer
                               ? serverManager.activeServer.serverMembers : []
                        spacing: 2

                        delegate: Column {
                            id: memberRow
                            width: ListView.view ? ListView.view.width : 400
                            visible: {
                                var search = memberSearchField.text.toLowerCase();
                                if (search.length === 0) return true;
                                var dn = modelData.displayName ? modelData.displayName.toLowerCase() : "";
                                var uid = modelData.userId ? modelData.userId.toLowerCase() : "";
                                return dn.indexOf(search) >= 0 || uid.indexOf(search) >= 0;
                            }
                            readonly property string memberUserId: modelData.userId || ""
                            readonly property bool expanded: serverSettingsPopup.editingMemberId === memberUserId
                            // Set of assigned role ids. Rebuilt when the row
                            // expands; mutated by checkbox clicks; read by
                            // the Save button. Explicit object ref so QML
                            // tracks writes.
                            property var assignedSet: ({})

                            function rebuildAssignedSet() {
                                var m = {};
                                if (serverManager.activeServer && memberUserId) {
                                    var list = serverManager.activeServer.memberRoles(memberUserId);
                                    for (var i = 0; i < list.length; i++) m[list[i]] = true;
                                }
                                assignedSet = m;
                            }
                            onExpandedChanged: if (expanded) rebuildAssignedSet()

                            Rectangle {
                                width: parent.width
                                height: 52
                                radius: Theme.r2
                                color: memberItemMouse.containsMouse ? Theme.bg3 : Theme.bg2
                                border.color: parent.expanded ? Theme.accent : Theme.line
                                border.width: 1
                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                Behavior on border.color { ColorAnimation { duration: Theme.motion.fastMs } }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: Theme.sp.s5
                                    anchors.rightMargin: Theme.sp.s4
                                    spacing: Theme.sp.s4

                                    // Rounded-square avatar to match the
                                    // ServerRail + MemberList treatment
                                    // instead of the old circle.
                                    Rectangle {
                                        width: Theme.avatar.md
                                        height: Theme.avatar.md
                                        radius: Theme.r2
                                        color: Theme.senderColor(modelData.userId || "")
                                        Text {
                                            anchors.centerIn: parent
                                            text: {
                                                var n = modelData.displayName || modelData.userId || "?";
                                                var s = n.replace(/^[^a-zA-Z0-9]+/, "");
                                                return (s.length > 0 ? s.charAt(0) : "?").toUpperCase();
                                            }
                                            font.family: Theme.fontSans
                                            font.pixelSize: 13
                                            font.weight: Theme.fontWeight.semibold
                                            color: Theme.onAccent
                                        }
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        // mxid pinned above name+chips so the
                                        // three-line stack reads: display
                                        // name → id → assigned roles.
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: Theme.sp.s2
                                            Text {
                                                text: modelData.displayName || ""
                                                font.family: Theme.fontSans
                                                font.pixelSize: Theme.fontSize.md
                                                font.weight: Theme.fontWeight.semibold
                                                color: Theme.fg0
                                                elide: Text.ElideRight
                                            }
                                            Text {
                                                text: modelData.userId || ""
                                                font.family: Theme.fontMono
                                                font.pixelSize: Theme.fontSize.xs
                                                color: Theme.fg3
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }
                                        }

                                        // Assigned-role chips. Binds to
                                        // permissionsGeneration so the row
                                        // updates immediately after save —
                                        // no expand/collapse required.
                                        Flow {
                                            Layout.fillWidth: true
                                            spacing: 4
                                            readonly property int _gen:
                                                serverManager.activeServer
                                                    ? serverManager.activeServer.permissionsGeneration : 0
                                            readonly property var assigned: {
                                                _gen;
                                                if (!serverManager.activeServer) return [];
                                                return serverManager.activeServer.memberRoles(memberRow.memberUserId);
                                            }
                                            visible: assigned.length > 0
                                            Repeater {
                                                model: parent.assigned
                                                delegate: Rectangle {
                                                    // Resolve the role's
                                                    // colour + display name
                                                    // from serverRoles; fall
                                                    // back to the raw id.
                                                    readonly property var roleInfo: {
                                                        if (!serverManager.activeServer) return null;
                                                        var list = serverManager.activeServer.serverRoles;
                                                        for (var i = 0; i < list.length; i++) {
                                                            var rid = list[i].id || list[i].name;
                                                            if (rid === modelData) return list[i];
                                                        }
                                                        return null;
                                                    }
                                                    readonly property color rcolor:
                                                        roleInfo && roleInfo.color ? roleInfo.color : Theme.fg3
                                                    readonly property string rname:
                                                        roleInfo && roleInfo.name ? roleInfo.name : modelData
                                                    visible: modelData !== "everyone"
                                                    implicitWidth: chipRow.implicitWidth + 10
                                                    implicitHeight: 18
                                                    radius: 9
                                                    color: Qt.rgba(rcolor.r, rcolor.g, rcolor.b, 0.14)
                                                    border.color: Qt.rgba(rcolor.r, rcolor.g, rcolor.b, 0.38)
                                                    border.width: 1
                                                    RowLayout {
                                                        id: chipRow
                                                        anchors.centerIn: parent
                                                        spacing: 4
                                                        Rectangle {
                                                            width: 6; height: 6; radius: 3
                                                            color: parent.parent.rcolor
                                                            Layout.alignment: Qt.AlignVCenter
                                                        }
                                                        Text {
                                                            text: parent.parent.rname
                                                            color: parent.parent.rcolor
                                                            font.family: Theme.fontSans
                                                            font.pixelSize: 10
                                                            font.weight: Theme.fontWeight.semibold
                                                            Layout.alignment: Qt.AlignVCenter
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    // Chevron to mirror the role row.
                                    Icon {
                                        name: "chevron-right"
                                        size: 14
                                        color: Theme.fg2
                                        rotation: parent.parent.expanded ? 90 : 0
                                        Behavior on rotation {
                                            NumberAnimation { duration: Theme.motion.fastMs
                                                              easing.type: Easing.BezierSpline
                                                              easing.bezierCurve: Theme.motion.bezier }
                                        }
                                    }

                                    // Chevron alone carries the expand/collapse
                                    // affordance; no more duplicate "Roles ▸" /
                                    // "Close ▾" text — it read as two different
                                    // buttons instead of one gesture.
                                }

                                MouseArea {
                                    id: memberItemMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        serverSettingsPopup.editingMemberId =
                                            memberRow.expanded ? "" : memberRow.memberUserId;
                                    }
                                }
                            }

                            // Role assignment card — mirrors the role-edit
                            // card's treatment (r2 bg0 with line border) so the
                            // two expansion patterns read consistently.
                            Rectangle {
                                id: roleAssignCard
                                width: parent.width
                                visible: memberRow.expanded
                                radius: Theme.r2
                                color: Theme.bg0
                                border.color: Theme.line
                                border.width: 1
                                height: visible ? roleAssignCol.implicitHeight + Theme.sp.s7 * 2 : 0

                                ColumnLayout {
                                    id: roleAssignCol
                                    anchors.fill: parent
                                    anchors.margins: Theme.sp.s7
                                    spacing: Theme.sp.s3

                                    Text {
                                        text: "ASSIGNED ROLES"
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.xs
                                        font.weight: Theme.fontWeight.semibold
                                        font.letterSpacing: Theme.trackWidest.xs
                                        color: Theme.fg3
                                    }

                                    Repeater {
                                        model: serverManager.activeServer ? serverManager.activeServer.serverRoles : []
                                        delegate: Rectangle {
                                            // Each role is a bg1-tinted row with
                                            // checkbox + color dot + name. Hover
                                            // flips the row bg so the click
                                            // affordance reads even when the
                                            // checkbox is a small target.
                                            readonly property string roleId: modelData.id || modelData.name
                                            visible: roleId !== "everyone"
                                            Layout.fillWidth: true
                                            implicitHeight: 32
                                            radius: Theme.r1
                                            color: assignHover.containsMouse ? Theme.bg2 : "transparent"
                                            Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.leftMargin: Theme.sp.s3
                                                anchors.rightMargin: Theme.sp.s3
                                                spacing: Theme.sp.s3

                                                ThemedCheckBox {
                                                    id: rolecb
                                                    checked: memberRow.assignedSet[parent.parent.roleId] === true
                                                    onToggled: {
                                                        var m = {};
                                                        for (var k in memberRow.assignedSet) m[k] = memberRow.assignedSet[k];
                                                        if (checked) m[parent.parent.roleId] = true;
                                                        else delete m[parent.parent.roleId];
                                                        memberRow.assignedSet = m;
                                                    }
                                                }
                                                Rectangle {
                                                    Layout.alignment: Qt.AlignVCenter
                                                    width: 12; height: 12; radius: 6
                                                    color: modelData.color || Theme.accent
                                                    border.color: Theme.bg0
                                                    border.width: 1
                                                }
                                                Text {
                                                    Layout.alignment: Qt.AlignVCenter
                                                    Layout.fillWidth: true
                                                    text: modelData.name || ""
                                                    color: Theme.fg0
                                                    font.family: Theme.fontSans
                                                    font.pixelSize: Theme.fontSize.md
                                                    font.weight: Theme.fontWeight.medium
                                                    elide: Text.ElideRight
                                                }
                                            }

                                            MouseArea {
                                                id: assignHover
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                // Click anywhere on the row
                                                // toggles the role in/out of
                                                // assignedSet. The checkbox's
                                                // `checked: assignedSet[roleId]`
                                                // binding then flips visually.
                                                //
                                                // We used to call rolecb.toggle()
                                                // here, but Qt Controls only
                                                // fires the CheckBox's
                                                // `toggled` signal for real
                                                // user clicks on the indicator
                                                // — a programmatic toggle()
                                                // flips state silently with no
                                                // `toggled` emission. So
                                                // clicks that landed on the
                                                // row (not the 18px box)
                                                // visually flipped but never
                                                // reached onToggled, and the
                                                // save path sent the unchanged
                                                // role list.
                                                onClicked: {
                                                    var m = {};
                                                    for (var k in memberRow.assignedSet) {
                                                        m[k] = memberRow.assignedSet[k];
                                                    }
                                                    var rid = parent.roleId;
                                                    if (m[rid]) delete m[rid];
                                                    else m[rid] = true;
                                                    memberRow.assignedSet = m;
                                                }
                                            }
                                        }
                                    }

                                    // Action row — primary Save (accent pill)
                                    // on the left; ghost-danger Kick + filled
                                    // danger Ban pushed to the right so the
                                    // destructive actions don't compete with
                                    // the role-assignment save. Self is
                                    // protected: the buttons disappear when
                                    // editing your own account.
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.topMargin: Theme.sp.s3
                                        spacing: Theme.sp.s3

                                        readonly property bool isSelf:
                                            serverManager.activeServer
                                            && memberRow.memberUserId
                                               === serverManager.activeServer.userId

                                        Button {
                                            id: roleAssignSaveBtn
                                            contentItem: Text {
                                                text: "Save assignments"
                                                font.family: Theme.fontSans
                                                font.pixelSize: Theme.fontSize.md
                                                font.weight: Theme.fontWeight.semibold
                                                color: Theme.onAccent
                                                horizontalAlignment: Text.AlignHCenter
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                            background: Rectangle {
                                                color: roleAssignSaveBtn.hovered ? Theme.accentDim : Theme.accent
                                                radius: Theme.r2
                                                implicitHeight: 36
                                                implicitWidth: 160
                                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                            }
                                            onClicked: {
                                                if (!serverManager.activeServer) return;
                                                var ids = [];
                                                for (var k in memberRow.assignedSet) {
                                                    if (memberRow.assignedSet[k]) ids.push(k);
                                                }
                                                serverManager.activeServer.setMemberRoles(
                                                    memberRow.memberUserId, ids);
                                                serverSettingsPopup.editingMemberId = "";
                                            }
                                        }

                                        Item { Layout.fillWidth: true }

                                        // Ghost-danger Kick — hollow with
                                        // danger border, fills on hover.
                                        // Hidden for the current user.
                                        Button {
                                            id: kickBtn
                                            visible: !parent.isSelf
                                            contentItem: Text {
                                                text: "Kick"
                                                font.family: Theme.fontSans
                                                font.pixelSize: Theme.fontSize.md
                                                font.weight: Theme.fontWeight.semibold
                                                color: kickBtn.hovered ? Theme.onAccent : Theme.danger
                                                horizontalAlignment: Text.AlignHCenter
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                            background: Rectangle {
                                                color: kickBtn.hovered ? Theme.danger : "transparent"
                                                radius: Theme.r2
                                                border.color: Theme.danger
                                                border.width: 1
                                                implicitHeight: 36
                                                implicitWidth: 80
                                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                            }
                                            onClicked: {
                                                serverSettingsPopup.confirmMod = {
                                                    kind: "kick",
                                                    userId: memberRow.memberUserId,
                                                    displayName: modelData.displayName || memberRow.memberUserId
                                                };
                                                serverSettingsPopup.confirmModDialog.open();
                                            }
                                        }

                                        // Filled danger Ban — primary
                                        // destructive action, strongest read.
                                        Button {
                                            id: banBtn
                                            visible: !parent.isSelf
                                            contentItem: Text {
                                                text: "Ban"
                                                font.family: Theme.fontSans
                                                font.pixelSize: Theme.fontSize.md
                                                font.weight: Theme.fontWeight.semibold
                                                color: Theme.onAccent
                                                horizontalAlignment: Text.AlignHCenter
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                            background: Rectangle {
                                                color: banBtn.hovered
                                                       ? Qt.lighter(Theme.danger, 1.1)
                                                       : Theme.danger
                                                radius: Theme.r2
                                                implicitHeight: 36
                                                implicitWidth: 80
                                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                            }
                                            onClicked: {
                                                serverSettingsPopup.confirmMod = {
                                                    kind: "ban",
                                                    userId: memberRow.memberUserId,
                                                    displayName: modelData.displayName || memberRow.memberUserId
                                                };
                                                serverSettingsPopup.confirmModDialog.open();
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ---- Channels (index 3) ----
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.sp.s7 * 2
                    spacing: Theme.sp.s7

                    RowLayout {
                        Layout.fillWidth: true
                        TabHeader { title: "Channels"; Layout.fillWidth: true }

                        // Top-right "+ Add category" — matches the accent-pill
                        // vocabulary used on "Add role" in the Roles tab. The
                        // create-prompt popup is owned by ChannelList, so we
                        // emit a signal instead of opening it directly.
                        Button {
                            id: addCategoryBtn
                            contentItem: RowLayout {
                                spacing: Theme.sp.s2
                                Icon { name: "plus"; size: 14; color: Theme.onAccent; Layout.alignment: Qt.AlignVCenter }
                                Text {
                                    text: "Add category"
                                    color: Theme.onAccent
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.fontSize.md
                                    font.weight: Theme.fontWeight.semibold
                                    Layout.alignment: Qt.AlignVCenter
                                }
                            }
                            background: Rectangle {
                                color: addCategoryBtn.hovered ? Theme.accentDim : Theme.accent
                                radius: Theme.r2
                                implicitHeight: 36
                                implicitWidth: 150
                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                            }
                            onClicked: serverSettingsPopup.createChannelRequested("category", "")
                        }
                    }

                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true

                        ScrollBar.vertical: ThemedScrollBar {}
                        model: serverManager.activeServer ? serverManager.activeServer.categorizedRooms : []
                        spacing: Theme.sp.s3

                        delegate: Column {
                            width: ListView.view ? ListView.view.width : 400
                            readonly property string catId: modelData.categoryId || ""

                            // Category header — widest-tracked small caps
                            // matching the channel-list + settings labels.
                            // Hover reveals "+ Add text / + Add voice"
                            // affordances pushed to the right.
                            Rectangle {
                                id: catHeader
                                width: parent.width
                                height: 32
                                color: "transparent"
                                visible: parent.catId !== ""

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: Theme.sp.s1
                                    anchors.rightMargin: Theme.sp.s1
                                    spacing: Theme.sp.s2

                                    Text {
                                        text: (modelData.categoryName || "").toUpperCase()
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.xs
                                        font.weight: Theme.fontWeight.semibold
                                        font.letterSpacing: Theme.trackWidest.xs
                                        color: Theme.fg3
                                        Layout.fillWidth: true
                                    }

                                    Text {
                                        text: modelData.channels ? modelData.channels.length + " channels" : "0 channels"
                                        font.family: Theme.fontMono
                                        font.pixelSize: Theme.fontSize.xs
                                        color: Theme.fg3
                                        visible: !catHeaderHover.containsMouse
                                    }

                                    // Inline create buttons — only show on
                                    // category hover so the list stays quiet
                                    // at rest. Each is an icon-only 24×24
                                    // ghost button with an accent-on-hover tint.
                                    component CatAddBtn: Rectangle {
                                        id: _cab
                                        property string iconName: ""
                                        property string tooltipText: ""
                                        property string createKind: ""
                                        Layout.preferredWidth: 24
                                        Layout.preferredHeight: 24
                                        radius: Theme.r1
                                        color: _cabMouse.containsMouse ? Theme.bg3 : "transparent"
                                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                        Icon {
                                            anchors.centerIn: parent
                                            name: _cab.iconName
                                            size: 12
                                            color: _cabMouse.containsMouse ? Theme.accent : Theme.fg2
                                        }
                                        MouseArea {
                                            id: _cabMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: serverSettingsPopup.createChannelRequested(
                                                           _cab.createKind, catHeader.parent.catId)
                                        }
                                        ToolTip.visible: _cabMouse.containsMouse && tooltipText.length > 0
                                        ToolTip.text: tooltipText
                                        ToolTip.delay: 500
                                    }

                                    CatAddBtn {
                                        iconName: "hash"
                                        tooltipText: "Add text channel"
                                        createKind: "text"
                                        visible: catHeaderHover.containsMouse
                                    }
                                    CatAddBtn {
                                        iconName: "volume"
                                        tooltipText: "Add voice channel"
                                        createKind: "voice"
                                        visible: catHeaderHover.containsMouse
                                    }
                                }

                                MouseArea {
                                    id: catHeaderHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    acceptedButtons: Qt.NoButton  // pass clicks through to the CatAddBtn children
                                }
                            }

                            // Channels in category.
                            Repeater {
                                model: modelData.channels

                                delegate: Rectangle {
                                    width: parent.width
                                    height: 36
                                    radius: Theme.r1
                                    color: chSettingsMouse.containsMouse ? Theme.bg3 : "transparent"
                                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                                    readonly property string rowRoomId: modelData.roomId || ""
                                    readonly property string rowRoomName: modelData.displayName || ""
                                    readonly property bool rowIsVoice: modelData.isVoice === true

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: Theme.sp.s4
                                        anchors.rightMargin: Theme.sp.s2
                                        spacing: Theme.sp.s3

                                        Icon {
                                            name: parent.parent.rowIsVoice ? "volume" : "hash"
                                            size: 14
                                            color: chSettingsMouse.containsMouse ? Theme.accent : Theme.fg2
                                        }

                                        Text {
                                            text: parent.parent.rowRoomName
                                            font.family: Theme.fontSans
                                            font.pixelSize: Theme.fontSize.md
                                            color: Theme.fg1
                                            Layout.fillWidth: true
                                            elide: Text.ElideRight
                                        }

                                        Text {
                                            text: (modelData.roomType || (parent.parent.rowIsVoice ? "voice" : "text")).toUpperCase()
                                            font.family: Theme.fontMono
                                            font.pixelSize: Theme.fontSize.xs
                                            font.letterSpacing: Theme.trackWide.xs
                                            color: Theme.fg3
                                        }

                                        // Settings (text channels only) and
                                        // Delete (all channels) on hover. Each
                                        // has its own MouseArea so the chrome
                                        // reliably routes clicks — bare Icon
                                        // inside a parent MouseArea would
                                        // eat everything.
                                        Rectangle {
                                            Layout.preferredWidth: 24
                                            Layout.preferredHeight: 24
                                            radius: Theme.r1
                                            color: _settingsMouse.containsMouse ? Theme.bg2 : "transparent"
                                            visible: chSettingsMouse.containsMouse
                                                   && !parent.parent.rowIsVoice
                                            Icon {
                                                anchors.centerIn: parent
                                                name: "settings"
                                                size: 12
                                                color: _settingsMouse.containsMouse ? Theme.fg0 : Theme.fg2
                                            }
                                            MouseArea {
                                                id: _settingsMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    var rid = parent.parent.parent.rowRoomId;
                                                    var rname = parent.parent.parent.rowRoomName;
                                                    serverSettingsPopup.close();
                                                    serverSettingsPopup.channelSettingsRequested(rid, rname);
                                                }
                                            }
                                            ToolTip.visible: _settingsMouse.containsMouse
                                            ToolTip.text: "Channel settings"
                                            ToolTip.delay: 500
                                        }
                                        Rectangle {
                                            Layout.preferredWidth: 24
                                            Layout.preferredHeight: 24
                                            radius: Theme.r1
                                            color: _deleteMouse.containsMouse
                                                   ? Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.16)
                                                   : "transparent"
                                            visible: chSettingsMouse.containsMouse
                                            Icon {
                                                anchors.centerIn: parent
                                                name: "x"
                                                size: 12
                                                color: _deleteMouse.containsMouse ? Theme.danger : Theme.fg2
                                            }
                                            MouseArea {
                                                id: _deleteMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    var rid = parent.parent.parent.rowRoomId;
                                                    var rname = parent.parent.parent.rowRoomName;
                                                    serverSettingsPopup.channelDeleteRequested(rid, rname);
                                                }
                                            }
                                            ToolTip.visible: _deleteMouse.containsMouse
                                            ToolTip.text: "Delete channel"
                                            ToolTip.delay: 500
                                        }
                                    }

                                    MouseArea {
                                        id: chSettingsMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        // Hover-only — clicks on the settings/
                                        // delete buttons are handled by their
                                        // own MouseAreas stacked above.
                                        acceptedButtons: Qt.NoButton
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ---- Bans (index 4) ----
            // Aggregates banned users across every room we've synced. The
            // list is a snapshot — drop to empty-state when there's nothing
            // to show rather than rendering a blank ListView.
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.sp.s7 * 2
                    spacing: Theme.sp.s7

                    TabHeader { title: "Bans" }

                    readonly property var banList:
                        serverManager.activeServer
                            ? serverManager.activeServer.bannedMembers : []

                    Text {
                        Layout.fillWidth: true
                        text: "Banned users can't see or join any channel on this server until they're unbanned."
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                        color: Theme.fg2
                        wrapMode: Text.WordWrap
                    }

                    // Empty-state card, shown when there are no bans.
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: parent.banList.length === 0

                        ColumnLayout {
                            anchors.centerIn: parent
                            width: 360
                            spacing: Theme.sp.s4

                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 56
                                Layout.preferredHeight: 56
                                radius: Theme.r3
                                color: Theme.bg2
                                border.color: Theme.line
                                border.width: 1
                                Icon {
                                    anchors.centerIn: parent
                                    name: "shield"
                                    size: 24
                                    color: Theme.fg3
                                }
                            }
                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: "No one is banned"
                                color: Theme.fg0
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.lg
                                font.weight: Theme.fontWeight.semibold
                            }
                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                text: "Ban a member from the Members tab and they'll show up here."
                                color: Theme.fg2
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.sm
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                    // Populated list — each row shows avatar + name + mxid +
                    // room count + reason, with a ghost Unban button on the
                    // right. Same visual vocabulary as the Members rows so
                    // the two tabs read as siblings.
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        visible: parent.banList.length > 0
                        ScrollBar.vertical: ThemedScrollBar {}
                        model: parent.banList
                        spacing: 4

                        delegate: Rectangle {
                            width: ListView.view ? ListView.view.width : 400
                            height: 60
                            radius: Theme.r2
                            color: Theme.bg2
                            border.color: Theme.line
                            border.width: 1

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.sp.s5
                                anchors.rightMargin: Theme.sp.s4
                                spacing: Theme.sp.s4

                                Rectangle {
                                    width: Theme.avatar.md
                                    height: Theme.avatar.md
                                    radius: Theme.r2
                                    color: Theme.senderColor(modelData.userId || "")
                                    Text {
                                        anchors.centerIn: parent
                                        text: {
                                            var n = modelData.displayName || modelData.userId || "?";
                                            var s = n.replace(/^[^a-zA-Z0-9]+/, "");
                                            return (s.length > 0 ? s.charAt(0) : "?").toUpperCase();
                                        }
                                        font.family: Theme.fontSans
                                        font.pixelSize: 13
                                        font.weight: Theme.fontWeight.semibold
                                        color: Theme.onAccent
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0

                                    Text {
                                        text: modelData.displayName || modelData.userId || ""
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.md
                                        font.weight: Theme.fontWeight.semibold
                                        color: Theme.fg0
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.sp.s2

                                        Text {
                                            text: modelData.userId || ""
                                            font.family: Theme.fontMono
                                            font.pixelSize: Theme.fontSize.xs
                                            color: Theme.fg3
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                        // Reason, if any — quieter, italic.
                                        Text {
                                            visible: (modelData.reason || "") !== ""
                                            text: "· " + (modelData.reason || "")
                                            font.family: Theme.fontSans
                                            font.pixelSize: Theme.fontSize.xs
                                            font.italic: true
                                            color: Theme.fg3
                                            elide: Text.ElideRight
                                        }
                                    }
                                }

                                // Ghost Unban.
                                Button {
                                    id: unbanBtn
                                    contentItem: Text {
                                        text: "Unban"
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.md
                                        font.weight: Theme.fontWeight.semibold
                                        color: unbanBtn.hovered ? Theme.fg0 : Theme.fg1
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    background: Rectangle {
                                        color: unbanBtn.hovered ? Theme.bg3 : "transparent"
                                        radius: Theme.r2
                                        border.color: Theme.line
                                        border.width: 1
                                        implicitHeight: 32
                                        implicitWidth: 88
                                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                    }
                                    onClicked: {
                                        serverSettingsPopup.confirmMod = {
                                            kind: "unban",
                                            userId: modelData.userId,
                                            displayName: modelData.displayName || modelData.userId
                                        };
                                        serverSettingsPopup.confirmModDialog.open();
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // (Legacy top-right close-X removed; the new one lives in `background`
    // so it floats above content regardless of the current pane.)

    onOpened: selectedSection = 0

    // Shared confirm dialog for kick / ban / unban. Reuses the delete-channel
    // popup vocabulary — bg1 + r3 + line, danger-tinted icon tile for
    // destructive actions, ghost Cancel + filled primary action.
    Popup {
        id: _confirmModDialog
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        width: 420
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: Theme.sp.s7

        readonly property bool isDestructive:
            serverSettingsPopup.confirmMod.kind === "kick"
            || serverSettingsPopup.confirmMod.kind === "ban"
        readonly property string titleText: {
            var who = serverSettingsPopup.confirmMod.displayName || "this member";
            switch (serverSettingsPopup.confirmMod.kind) {
                case "kick":  return "Kick " + who + "?";
                case "ban":   return "Ban " + who + "?";
                case "unban": return "Unban " + who + "?";
                default:      return "";
            }
        }
        readonly property string descText: {
            switch (serverSettingsPopup.confirmMod.kind) {
                case "kick":
                    return "They're removed from every channel on this server but can be invited back or rejoin if the server allows.";
                case "ban":
                    return "They're removed from every channel and prevented from rejoining until you unban them. Their messages stay — you can delete those separately.";
                case "unban":
                    return "They can be re-invited or rejoin this server (subject to channel permissions) once the ban is lifted.";
                default:
                    return "";
            }
        }

        background: Rectangle {
            color: Theme.bg1
            radius: Theme.r3
            border.color: Theme.line
            border.width: 1
        }

        contentItem: ColumnLayout {
            spacing: Theme.sp.s4

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp.s3
                Rectangle {
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 32
                    radius: Theme.r2
                    color: _confirmModDialog.isDestructive
                        ? Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.15)
                        : Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.15)
                    Icon {
                        anchors.centerIn: parent
                        name: _confirmModDialog.isDestructive ? "x" : "check"
                        size: 16
                        color: _confirmModDialog.isDestructive ? Theme.danger : Theme.accent
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: _confirmModDialog.titleText
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xl
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackTight.xl
                    color: Theme.fg0
                    wrapMode: Text.WordWrap
                }
            }

            Text {
                Layout.fillWidth: true
                text: _confirmModDialog.descText
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
                wrapMode: Text.WordWrap
            }

            // Optional reason input — only for kick/ban, since unban doesn't
            // carry a reason in our model. Matrix attaches the reason to the
            // ban event so it shows up in the banned-members reason column.
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp.s1
                visible: _confirmModDialog.isDestructive
                Text {
                    text: "REASON (OPTIONAL)"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xs
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackWidest.xs
                    color: Theme.fg3
                }
                TextField {
                    id: reasonField
                    Layout.fillWidth: true
                    placeholderText: "Spam, harassment, off-topic, …"
                    placeholderTextColor: Theme.fg3
                    color: Theme.fg0
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    background: Rectangle {
                        color: Theme.bg0
                        radius: Theme.r2
                        border.color: reasonField.activeFocus ? Theme.accent : Theme.line
                        border.width: 1
                    }
                    leftPadding: Theme.sp.s4
                    rightPadding: Theme.sp.s4
                    topPadding: Theme.sp.s3
                    bottomPadding: Theme.sp.s3
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.sp.s3
                spacing: Theme.sp.s3
                Item { Layout.fillWidth: true }

                Button {
                    id: cancelModBtn
                    contentItem: Text {
                        text: "Cancel"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.medium
                        color: Theme.fg1
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: cancelModBtn.hovered ? Theme.bg3 : "transparent"
                        border.color: Theme.line
                        border.width: 1
                        radius: Theme.r2
                        implicitWidth: 100
                        implicitHeight: 36
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                    onClicked: _confirmModDialog.close()
                }

                Button {
                    id: confirmModBtn
                    contentItem: Text {
                        text: {
                            switch (serverSettingsPopup.confirmMod.kind) {
                                case "kick":  return "Kick member";
                                case "ban":   return "Ban member";
                                case "unban": return "Unban member";
                                default:      return "Confirm";
                            }
                        }
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.onAccent
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        readonly property color base: _confirmModDialog.isDestructive
                            ? Theme.danger : Theme.accent
                        color: confirmModBtn.hovered
                               ? (_confirmModDialog.isDestructive
                                   ? Qt.lighter(Theme.danger, 1.1)
                                   : Theme.accentDim)
                               : base
                        radius: Theme.r2
                        implicitWidth: 160
                        implicitHeight: 36
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                    onClicked: {
                        if (!serverManager.activeServer) {
                            _confirmModDialog.close();
                            return;
                        }
                        var m = serverSettingsPopup.confirmMod;
                        var reason = reasonField.text.trim();
                        switch (m.kind) {
                            case "kick":
                                serverManager.activeServer.kickFromServer(m.userId, reason);
                                break;
                            case "ban":
                                serverManager.activeServer.banFromServer(m.userId, reason);
                                break;
                            case "unban":
                                serverManager.activeServer.unbanFromServer(m.userId);
                                break;
                        }
                        serverSettingsPopup.editingMemberId = "";
                        _confirmModDialog.close();
                    }
                }
            }
        }

        onClosed: reasonField.text = ""
    }

    FileDialog {
        id: serverIconFileDialog
        title: "Choose Server Icon"
        nameFilters: ["Image files (*.png *.jpg *.jpeg *.gif *.webp)"]
        onAccepted: {
            if (!serverManager.activeServer) return;
            serverManager.activeServer.uploadServerAvatar(
                selectedFile.toString());
        }
    }
}
