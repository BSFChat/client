import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

Popup {
    id: serverSettingsPopup
    anchors.centerIn: Overlay.overlay
    width: parent ? parent.width * 0.85 : 800
    height: parent ? parent.height * 0.85 : 600
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    property int selectedSection: 0

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

    background: Rectangle {
        color: Theme.bgDark
        radius: Theme.radiusNormal
        border.color: Theme.bgLight
        border.width: 1
    }

    contentItem: RowLayout {
        spacing: 0

        // Left nav sidebar
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 200
            color: Theme.bgDarkest
            radius: Theme.radiusNormal

            // Clip right radius
            Rectangle {
                anchors.right: parent.right
                width: Theme.radiusNormal
                height: parent.height
                color: Theme.bgDarkest
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.spacingNormal
                spacing: 2

                Text {
                    text: "SERVER SETTINGS"
                    font.pixelSize: Theme.fontSizeSmall
                    font.bold: true
                    color: Theme.textMuted
                    Layout.leftMargin: Theme.spacingNormal
                    Layout.topMargin: Theme.spacingNormal
                    Layout.bottomMargin: Theme.spacingNormal
                }

                Repeater {
                    model: ["Overview", "Roles", "Members", "Channels", "Bans"]
                    delegate: Rectangle {
                        Layout.fillWidth: true
                        height: 36
                        radius: Theme.radiusSmall
                        color: selectedSection === index ? Theme.bgLight : navItemMouse.containsMouse ? Qt.darker(Theme.bgMedium, 0.9) : "transparent"

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: Theme.spacingNormal
                            text: modelData
                            color: selectedSection === index ? Theme.textPrimary : Theme.textSecondary
                            font.pixelSize: Theme.fontSizeNormal
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

                Item { Layout.fillHeight: true }

                // Close button at bottom
                Rectangle {
                    Layout.fillWidth: true
                    height: 36
                    radius: Theme.radiusSmall
                    color: closeNavMouse.containsMouse ? Theme.bgLight : "transparent"

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.spacingNormal
                        text: "Close"
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSizeNormal
                    }

                    MouseArea {
                        id: closeNavMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: serverSettingsPopup.close()
                    }
                }
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
                    anchors.margins: Theme.spacingLarge * 2
                    spacing: Theme.spacingLarge

                    Text {
                        text: "Server Overview"
                        font.pixelSize: 22
                        font.bold: true
                        color: Theme.textPrimary
                    }

                    Text {
                        text: "SERVER NAME"
                        font.pixelSize: Theme.fontSizeSmall
                        font.bold: true
                        color: Theme.textSecondary
                    }

                    TextField {
                        id: serverNameField
                        Layout.fillWidth: true
                        Layout.maximumWidth: 400
                        // Shown to all users across the server (channel-list
                        // header, tooltips, etc.). Only editable by users
                        // with MANAGE_SERVER; server enforces regardless.
                        text: serverManager.activeServer ? serverManager.activeServer.serverName : ""
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSizeNormal
                        background: Rectangle {
                            color: Theme.bgDarkest
                            radius: Theme.radiusSmall
                            border.color: serverNameField.activeFocus ? Theme.accent : Theme.bgLight
                            border.width: 1
                        }
                        padding: Theme.spacingNormal
                    }

                    Button {
                        text: "Save"
                        contentItem: Text {
                            text: parent.text
                            font.pixelSize: Theme.fontSizeNormal
                            color: "white"
                            horizontalAlignment: Text.AlignHCenter
                        }
                        background: Rectangle {
                            color: parent.hovered ? Theme.accentHover : Theme.accent
                            radius: Theme.radiusSmall
                            implicitWidth: 100
                            implicitHeight: Theme.buttonHeight
                        }
                        onClicked: {
                            if (serverManager.activeServer) {
                                serverManager.activeServer.updateServerName(serverNameField.text.trim());
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            // ---- Roles (index 1) ----
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingLarge * 2
                    spacing: Theme.spacingLarge

                    Text {
                        text: "Roles"
                        font.pixelSize: 22
                        font.bold: true
                        color: Theme.textPrimary
                    }

                    Text {
                        Layout.fillWidth: true
                        text: "Roles grant permissions server-wide. Assign them to members in the Members tab; override per-channel in each channel's settings."
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.textMuted
                        wrapMode: Text.WordWrap
                    }

                    // Roles list
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: serverManager.activeServer ? serverManager.activeServer.serverRoles : []
                        spacing: 4

                        delegate: Column {
                            width: ListView.view ? ListView.view.width : 400
                            spacing: 2

                            property var role: modelData
                            property bool isEditing: serverSettingsPopup.editingRoleId === (role.id || role.name)

                            Rectangle {
                                width: parent.width
                                height: 44
                                radius: Theme.radiusSmall
                                color: roleRowMouse.containsMouse ? Theme.bgLight : Theme.bgMedium

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: Theme.spacingNormal
                                    anchors.rightMargin: Theme.spacingNormal
                                    spacing: Theme.spacingNormal

                                    Rectangle {
                                        width: 14; height: 14; radius: 7
                                        color: parent.parent.parent.role.color || Theme.accent
                                    }
                                    Text {
                                        text: parent.parent.parent.role.name || ""
                                        font.pixelSize: Theme.fontSizeNormal
                                        color: Theme.textPrimary
                                        Layout.fillWidth: true
                                    }
                                    Text {
                                        text: parent.parent.parent.isEditing ? "Close ▾" : "Edit ▸"
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.accent
                                    }
                                }

                                MouseArea {
                                    id: roleRowMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        var rid = parent.parent.parent.role.id || parent.parent.parent.role.name;
                                        serverSettingsPopup.editingRoleId =
                                            parent.parent.parent.isEditing ? "" : rid;
                                    }
                                }
                            }

                            // Inline editor
                            Rectangle {
                                id: roleEditCard
                                width: parent.width
                                visible: parent.isEditing
                                radius: Theme.radiusSmall
                                color: Theme.bgDarkest
                                border.color: Theme.bgLight
                                border.width: 1
                                height: visible ? roleEditCol.implicitHeight + Theme.spacingLarge * 2 : 0

                                // Single source of truth for the form.
                                // editRole binds to the delegate's role, so
                                // scratch values reset whenever the user opens
                                // a different role.
                                readonly property var editRole: parent.parent.role
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
                                    anchors.margins: Theme.spacingLarge
                                    spacing: Theme.spacingNormal

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.spacingNormal

                                        TextField {
                                            id: editRoleName
                                            Layout.fillWidth: true
                                            text: roleEditCard.editRole.name || ""
                                            color: Theme.textPrimary
                                            font.pixelSize: Theme.fontSizeNormal
                                            background: Rectangle {
                                                color: Theme.bgMedium
                                                radius: Theme.radiusSmall
                                            }
                                            padding: Theme.spacingNormal
                                        }

                                        SpinBox {
                                            id: editRolePosition
                                            from: 0; to: 1000
                                            value: roleEditCard.scratchPos
                                            onValueModified: roleEditCard.scratchPos = value
                                            background: Rectangle {
                                                color: Theme.bgMedium
                                                radius: Theme.radiusSmall
                                                implicitWidth: 90
                                            }
                                        }
                                    }

                                    Row {
                                        spacing: Theme.spacingSmall
                                        Repeater {
                                            model: ["#5865f2", "#57f287", "#fee75c", "#ed4245", "#f47067",
                                                    "#e0823d", "#39c5cf", "#dcbdfb", "#768390", "#f69d50"]
                                            delegate: Rectangle {
                                                width: 20; height: 20; radius: 10
                                                color: modelData
                                                border.color: roleEditCard.scratchColor === modelData
                                                    ? Theme.textPrimary : "transparent"
                                                border.width: 2
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: roleEditCard.scratchColor = modelData
                                                }
                                            }
                                        }
                                    }

                                    Text {
                                        text: "Permissions"
                                        font.pixelSize: Theme.fontSizeSmall
                                        font.bold: true
                                        color: Theme.textSecondary
                                    }

                                    Grid {
                                        Layout.fillWidth: true
                                        columns: 2
                                        columnSpacing: Theme.spacingLarge
                                        rowSpacing: Theme.spacingSmall

                                        Repeater {
                                            model: serverSettingsPopup.permissionFlags
                                            delegate: Row {
                                                spacing: 6
                                                CheckBox {
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
                                                    color: Theme.textPrimary
                                                    font.pixelSize: Theme.fontSizeSmall
                                                    anchors.verticalCenter: cb.verticalCenter
                                                }
                                            }
                                        }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.topMargin: Theme.spacingNormal
                                        spacing: Theme.spacingNormal

                                        Button {
                                            text: "Save"
                                            contentItem: Text { text: parent.text; color: "white"; font.pixelSize: Theme.fontSizeNormal; horizontalAlignment: Text.AlignHCenter }
                                            background: Rectangle {
                                                color: parent.hovered ? Theme.accentHover : Theme.accent
                                                radius: Theme.radiusSmall
                                                implicitHeight: Theme.buttonHeight
                                                implicitWidth: 100
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

                                        Button {
                                            visible: (roleEditCard.editRole.id || roleEditCard.editRole.name) !== "everyone"
                                                  && (roleEditCard.editRole.id || roleEditCard.editRole.name) !== "admin"
                                            text: "Delete"
                                            contentItem: Text { text: parent.text; color: "#ed4245"; font.pixelSize: Theme.fontSizeNormal; horizontalAlignment: Text.AlignHCenter }
                                            background: Rectangle { color: "transparent"; radius: Theme.radiusSmall; border.color: "#ed4245"; border.width: 1; implicitHeight: Theme.buttonHeight; implicitWidth: 100 }
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
                            font.pixelSize: Theme.fontSizeNormal
                            color: Theme.textMuted
                        }
                    }

                    // Add-role footer
                    Button {
                        text: "+ Add Role"
                        contentItem: Text { text: parent.text; color: "white"; font.pixelSize: Theme.fontSizeNormal; horizontalAlignment: Text.AlignHCenter }
                        background: Rectangle { color: parent.hovered ? Theme.accentHover : Theme.accent; radius: Theme.radiusSmall; implicitHeight: Theme.buttonHeight; implicitWidth: 140 }
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
                                color: "#5865f2",
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
                    anchors.margins: Theme.spacingLarge * 2
                    spacing: Theme.spacingLarge

                    Text {
                        text: "Members"
                        font.pixelSize: 22
                        font.bold: true
                        color: Theme.textPrimary
                    }

                    // Search field
                    TextField {
                        id: memberSearchField
                        Layout.fillWidth: true
                        Layout.maximumWidth: 400
                        placeholderText: "Search members..."
                        placeholderTextColor: Theme.textMuted
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSizeNormal
                        background: Rectangle {
                            color: Theme.bgDarkest
                            radius: Theme.radiusSmall
                            border.color: memberSearchField.activeFocus ? Theme.accent : Theme.bgLight
                            border.width: 1
                        }
                        padding: Theme.spacingNormal
                    }

                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: serverManager.activeServer ? serverManager.activeServer.memberListModel : null
                        spacing: 2

                        delegate: Column {
                            width: ListView.view ? ListView.view.width : 400
                            visible: {
                                var search = memberSearchField.text.toLowerCase();
                                if (search.length === 0) return true;
                                var dn = model.displayName ? model.displayName.toLowerCase() : "";
                                var uid = model.userId ? model.userId.toLowerCase() : "";
                                return dn.indexOf(search) >= 0 || uid.indexOf(search) >= 0;
                            }
                            property string memberUserId: model.userId || ""
                            property bool expanded: serverSettingsPopup.editingMemberId === memberUserId

                            Rectangle {
                                width: parent.width
                                height: 48
                                radius: Theme.radiusSmall
                                color: memberItemMouse.containsMouse ? Theme.bgLight : Theme.bgMedium

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: Theme.spacingNormal
                                    anchors.rightMargin: Theme.spacingNormal
                                    spacing: Theme.spacingNormal

                                    Rectangle {
                                        width: 32; height: 32; radius: 16
                                        color: Theme.accent
                                        Text {
                                            anchors.centerIn: parent
                                            text: model.displayName ? model.displayName.charAt(0).toUpperCase() : "?"
                                            font.pixelSize: 14; font.bold: true; color: "white"
                                        }
                                    }

                                    Column {
                                        Layout.fillWidth: true
                                        Text { text: model.displayName || ""; font.pixelSize: Theme.fontSizeNormal; color: Theme.textPrimary }
                                        Text { text: model.userId || ""; font.pixelSize: Theme.fontSizeSmall; color: Theme.textMuted }
                                    }

                                    Text {
                                        text: parent.parent.parent.expanded ? "Close ▾" : "Roles ▸"
                                        color: Theme.accent
                                        font.pixelSize: Theme.fontSizeSmall
                                    }
                                }

                                MouseArea {
                                    id: memberItemMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        serverSettingsPopup.editingMemberId =
                                            parent.parent.parent.expanded ? "" : parent.parent.parent.memberUserId;
                                    }
                                }
                            }

                            // Role checkboxes
                            Rectangle {
                                width: parent.width
                                visible: parent.expanded
                                radius: Theme.radiusSmall
                                color: Theme.bgDarkest
                                border.color: Theme.bgLight; border.width: 1
                                height: visible ? roleAssignCol.implicitHeight + Theme.spacingLarge * 2 : 0

                                ColumnLayout {
                                    id: roleAssignCol
                                    anchors.fill: parent
                                    anchors.margins: Theme.spacingLarge
                                    spacing: Theme.spacingSmall

                                    property var assigned: (serverManager.activeServer
                                        ? serverManager.activeServer.memberRoles(parent.parent.memberUserId)
                                        : [])
                                    property var assignedMap: {
                                        var m = {};
                                        for (var i = 0; i < assigned.length; i++) m[assigned[i]] = true;
                                        return m;
                                    }

                                    Text {
                                        text: "Assigned roles"
                                        font.pixelSize: Theme.fontSizeSmall
                                        font.bold: true
                                        color: Theme.textSecondary
                                    }

                                    Repeater {
                                        model: serverManager.activeServer ? serverManager.activeServer.serverRoles : []
                                        delegate: Row {
                                            spacing: 6
                                            visible: (modelData.id || modelData.name) !== "everyone"
                                            CheckBox {
                                                id: rolecb
                                                checked: roleAssignCol.assignedMap[modelData.id || modelData.name] || false
                                                onClicked: {
                                                    var m = roleAssignCol.assignedMap;
                                                    var rid = modelData.id || modelData.name;
                                                    if (checked) m[rid] = true; else delete m[rid];
                                                    roleAssignCol.assignedMap = m;
                                                }
                                            }
                                            Rectangle { width: 10; height: 10; radius: 5; color: modelData.color || Theme.accent; anchors.verticalCenter: rolecb.verticalCenter }
                                            Text { text: modelData.name || ""; color: Theme.textPrimary; anchors.verticalCenter: rolecb.verticalCenter; font.pixelSize: Theme.fontSizeSmall }
                                        }
                                    }

                                    Button {
                                        Layout.topMargin: Theme.spacingNormal
                                        text: "Save"
                                        contentItem: Text { text: parent.text; color: "white"; font.pixelSize: Theme.fontSizeNormal; horizontalAlignment: Text.AlignHCenter }
                                        background: Rectangle { color: parent.hovered ? Theme.accentHover : Theme.accent; radius: Theme.radiusSmall; implicitHeight: Theme.buttonHeight; implicitWidth: 100 }
                                        onClicked: {
                                            if (!serverManager.activeServer) return;
                                            var ids = [];
                                            for (var k in roleAssignCol.assignedMap) {
                                                if (roleAssignCol.assignedMap[k]) ids.push(k);
                                            }
                                            serverManager.activeServer.setMemberRoles(
                                                parent.parent.parent.memberUserId, ids);
                                            serverSettingsPopup.editingMemberId = "";
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
                    anchors.margins: Theme.spacingLarge * 2
                    spacing: Theme.spacingLarge

                    Text {
                        text: "Channels"
                        font.pixelSize: 22
                        font.bold: true
                        color: Theme.textPrimary
                    }

                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: serverManager.activeServer ? serverManager.activeServer.categorizedRooms : []
                        spacing: 4

                        delegate: Column {
                            width: ListView.view ? ListView.view.width : 400

                            // Category header
                            Rectangle {
                                width: parent.width
                                height: 36
                                radius: Theme.radiusSmall
                                color: Theme.bgMedium
                                visible: modelData.categoryId !== ""

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: Theme.spacingNormal
                                    anchors.rightMargin: Theme.spacingNormal

                                    Text {
                                        text: modelData.categoryName || ""
                                        font.pixelSize: Theme.fontSizeNormal
                                        font.bold: true
                                        color: Theme.textPrimary
                                        Layout.fillWidth: true
                                    }

                                    Text {
                                        text: modelData.channels ? modelData.channels.length + " channels" : "0 channels"
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.textMuted
                                    }
                                }
                            }

                            // Channels in category
                            Repeater {
                                model: modelData.channels

                                delegate: Rectangle {
                                    width: parent.width
                                    height: 32
                                    color: chSettingsMouse.containsMouse ? Theme.bgLight : "transparent"

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: Theme.spacingLarge + Theme.spacingNormal
                                        anchors.rightMargin: Theme.spacingNormal
                                        spacing: Theme.spacingSmall

                                        Text {
                                            text: modelData.isVoice ? "\u25CF" : "#"
                                            font.pixelSize: Theme.fontSizeNormal
                                            color: Theme.textMuted
                                        }

                                        Text {
                                            text: modelData.displayName || ""
                                            font.pixelSize: Theme.fontSizeNormal
                                            color: Theme.textSecondary
                                            Layout.fillWidth: true
                                        }

                                        Text {
                                            text: modelData.roomType || "text"
                                            font.pixelSize: Theme.fontSizeSmall
                                            color: Theme.textMuted
                                        }
                                    }

                                    MouseArea {
                                        id: chSettingsMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ---- Bans (index 4) ----
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingLarge * 2
                    spacing: Theme.spacingLarge

                    Text {
                        text: "Bans"
                        font.pixelSize: 22
                        font.bold: true
                        color: Theme.textPrimary
                    }

                    Text {
                        text: "Banned users will appear here."
                        font.pixelSize: Theme.fontSizeNormal
                        color: Theme.textMuted
                        Layout.fillWidth: true
                    }

                    Item { Layout.fillHeight: true }
                }
            }
        }
    }

    // Close button (top-right X)
    Text {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: Theme.spacingNormal
        anchors.rightMargin: Theme.spacingNormal
        text: "\u2715"
        font.pixelSize: Theme.fontSizeLarge
        color: closeXMouse.containsMouse ? Theme.textPrimary : Theme.textMuted
        z: 10

        MouseArea {
            id: closeXMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: serverSettingsPopup.close()
        }
    }

    onOpened: selectedSection = 0
}
