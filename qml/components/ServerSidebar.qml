import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import BSFChat

// ServerRail (per SPEC §3.1) — the 72px leftmost column. Server switcher
// plus (eventually) DM entry point + add-server button.
//
// Key visual vocabulary for the whole app lives here:
//   • 44×44 rounded-square icons that squircle-morph on active
//   • left-edge accent bar (4×28) that grows on hover, extends on active
//   • unread dot (8×8) with 2px bg0 halo, bottom-right
//   • notif count pill (danger bg, onAccent-like fg)
//   • hover scale 1.04, radius tween r3 → r2
Rectangle {
    id: rail
    color: Theme.bg0
    implicitWidth: Theme.layout.serverRailW

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: Theme.sp.s5
        anchors.bottomMargin: Theme.sp.s5
        spacing: Theme.sp.s3

        // DMs destination — sits at the top of the rail as a peer
        // to the server icons, even though each DM is physically
        // hosted on some server. Clicking flips the channel panel
        // into DM-aggregate mode (every 1:1 across every connected
        // server in one list). Clicking any server icon below
        // reverts the mode; the sidebar is single-select across DMs
        // + servers.
        Item {
            id: dmEntry
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44

            readonly property bool _active: serverManager.viewingDms

            Rectangle {
                anchors.fill: parent
                radius: dmMouse.containsMouse || dmEntry._active
                    ? Theme.r2 : Theme.r3
                color: dmEntry._active ? Theme.accent
                     : dmMouse.containsMouse ? Theme.bg3 : Theme.bg2
                Behavior on radius { NumberAnimation { duration: Theme.motion.fastMs } }
                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                // The chip is a peer of the server tiles below and takes
                // part in the same single selection, so it gets the same
                // role. Icon-only: without this it is an unnamed square.
                Accessible.role: Accessible.ListItem
                Accessible.name: dmEntry._active
                    ? qsTr("Direct messages, showing")
                    : qsTr("Direct messages")
                Accessible.onPressAction: serverManager.setViewingDms(true)

                Icon {
                    anchors.centerIn: parent
                    name: "at"
                    size: 20
                    color: dmEntry._active ? Theme.onAccent : Theme.fg1
                }

                MouseArea {
                    id: dmMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: serverManager.setViewingDms(true)
                }

                ToolTip.visible: dmMouse.containsMouse
                ToolTip.text: "Direct messages"
                ToolTip.delay: 500
            }

            // Left-edge accent bar mirrors the server icons'
            // selection affordance so the DMs destination looks
            // native to the rail.
            Rectangle {
                visible: dmEntry._active
                anchors.left: parent.left
                anchors.leftMargin: -8
                anchors.verticalCenter: parent.verticalCenter
                width: 4
                height: 28
                radius: 2
                color: Theme.fg0
                // Draws the selection the chip already announces.
                Accessible.ignored: true
            }
        }

        // Thin divider separating DMs from the server list — makes
        // the "DMs live above servers" grouping legible.
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 28
            Layout.preferredHeight: 1
            color: Theme.lineSoft
            Accessible.ignored: true
        }

        // Server icons.
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.sp.s3
            clip: true

            ScrollBar.vertical: ThemedScrollBar {}
            model: serverManager.servers
            interactive: contentHeight > height

            delegate: Item {
                id: row
                width: ListView.view ? ListView.view.width : Theme.layout.serverRailW
                height: 52

                // A server is "active" only when we're not viewing
                // DMs. Otherwise the DM chip above owns the single
                // selection state and the server tiles render as
                // inactive, even though one of them is technically
                // m_activeServer underneath.
                readonly property bool isActive:
                    index === serverManager.activeServerIndex
                    && !serverManager.viewingDms
                readonly property bool hasUnread: (model.unreadCount || 0) > 0
                readonly property bool isHovered: hoverArea.containsMouse

                // The whole tile is one row: name, whether it is the one
                // being shown, and unread. The letter/icon inside it and
                // the unread dot beside it are ignored below, so this is
                // read once rather than as three fragments.
                Accessible.role: Accessible.ListItem
                Accessible.name: {
                    var n = model.displayName || "";
                    if (row.isActive) return qsTr("%1, showing").arg(n);
                    if (row.hasUnread)
                        return qsTr("%1, %n unread", "", model.unreadCount || 0)
                               .arg(n);
                    return n;
                }
                Accessible.onPressAction: {
                    serverManager.setViewingDms(false);
                    serverManager.setActiveServer(index);
                }

                // Left-edge active / hover indicator bar. Active = tall,
                // hover (inactive) = half-height nudge, unread (inactive,
                // not hovered) = short dot, otherwise hidden.
                Rectangle {
                    id: edgeBar
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: 4
                    radius: 2
                    color: Theme.accent
                    height: row.isActive ? 28
                          : row.isHovered ? 18
                          : row.hasUnread ? 8
                          : 0
                    visible: height > 0
                    // Pure affordance for the row's selected / unread
                    // state, both of which the row's name carries.
                    Accessible.ignored: true
                    Behavior on height {
                        NumberAnimation { duration: Theme.motion.fastMs
                                          easing.type: Easing.BezierSpline
                                          easing.bezierCurve: Theme.motion.bezier }
                    }
                }

                // Icon tile. Rounded-square → squircle morph on active, via
                // a radius animation. Hover pops a subtle scale.
                Rectangle {
                    id: tile
                    width: 44
                    height: 44
                    anchors.centerIn: parent
                    radius: row.isActive ? Theme.r2 : Theme.r3
                    color: row.isActive ? Theme.accent
                         : row.isHovered ? Theme.bg3
                         : Theme.bg2
                    scale: row.isHovered && !row.isActive ? 1.04 : 1.0
                    border.width: row.isActive ? 0 : 1
                    border.color: Theme.line

                    Behavior on radius { NumberAnimation { duration: Theme.motion.normalMs
                                                           easing.type: Easing.BezierSpline
                                                           easing.bezierCurve: Theme.motion.bezier } }
                    Behavior on scale { NumberAnimation { duration: Theme.motion.fastMs
                                                          easing.type: Easing.BezierSpline
                                                          easing.bezierCurve: Theme.motion.bezier } }
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                    // Initial letter — shown when there's no icon
                    // uploaded, or the icon is still loading.
                    Text {
                        anchors.centerIn: parent
                        visible: serverIcon.status !== Image.Ready
                              || !model.iconUrl
                        text: {
                            var name = (model.displayName || "?");
                            var stripped = name.replace(/^[^a-zA-Z0-9]+/, "");
                            if (stripped.length === 0) return "?";
                            return stripped.charAt(0).toUpperCase();
                        }
                        font.family: Theme.fontSans
                        font.pixelSize: 18
                        font.weight: Theme.fontWeight.semibold
                        color: row.isActive ? Theme.onAccent : Theme.fg0
                        // A picture of the server's initial, not a fact.
                        Accessible.ignored: true
                    }

                    // Uploaded server icon. The containing Rectangle is
                    // rounded, so small margins hide any sharp image
                    // corners inside the radius; if the image fails
                    // the initial letter above fades back in via its
                    // `visible` binding.
                    Image {
                        id: serverIcon
                        anchors.fill: parent
                        anchors.margins: 1
                        source: model.iconUrl || ""
                        visible: status === Image.Ready
                        fillMode: Image.PreserveAspectCrop
                        smooth: true
                        asynchronous: true
                        cache: true
                    }
                }

                // Unread dot (bottom-right) — only when inactive and has
                // unread. Active servers have all their rooms visible so
                // the dot would be redundant. Uses the danger palette
                // because any server-level unread is "you have mentions
                // or activity across the whole server" — worth a stronger
                // cue than the neutral-grey channel-row unread.
                Rectangle {
                    width: 10
                    height: 10
                    radius: 5
                    color: Theme.danger
                    border.width: 2
                    border.color: Theme.bg0
                    anchors.right: tile.right
                    anchors.bottom: tile.bottom
                    anchors.rightMargin: -1
                    anchors.bottomMargin: -1
                    visible: !row.isActive && row.hasUnread
                    // "Unread" is already in the row's name.
                    Accessible.ignored: true
                }

                MouseArea {
                    id: hoverArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    // The menu this opens is the only place the client
                    // shows a server's address and connection state after
                    // it has been added, and the only way to edit or
                    // remove one — see the Menu below. Right-click-only
                    // meant a phone could add servers and never remove
                    // them. Long press is the touch equivalent, following
                    // MemberList's precedent.
                    onPressAndHold: {
                        if (typeof haptics !== "undefined") haptics.longPress();
                        serverContextMenu.openFor(index);
                    }
                    onClicked: (mouse) => {
                        if (mouse.button === Qt.RightButton) {
                            serverContextMenu.openFor(index);
                            return;
                        }
                        // Picking a server icon is the primary
                        // way to leave DM view — it's single-
                        // select across the rail.
                        serverManager.setViewingDms(false);
                        serverManager.setActiveServer(index);
                    }
                }

                ToolTip.visible: hoverArea.containsMouse
                ToolTip.text: model.displayName || ""
                ToolTip.delay: 400
            }
        }

        // Add-server button — ghost style, dashed border, matches the
        // icon footprint so visual rhythm stays intact.
        Item {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44

            Rectangle {
                id: addTile
                anchors.fill: parent
                radius: Theme.r3
                color: addArea.containsMouse ? Theme.bg3 : "transparent"
                border.width: 1
                border.color: addArea.containsMouse ? Theme.accent : Theme.line
                scale: addArea.containsMouse ? 1.04 : 1.0

                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                Behavior on border.color { ColorAnimation { duration: Theme.motion.fastMs } }
                Behavior on scale { NumberAnimation { duration: Theme.motion.fastMs
                                                      easing.type: Easing.BezierSpline
                                                      easing.bezierCurve: Theme.motion.bezier } }

                // Icon-only: the tooltip below is the name it would have
                // had, so it is the name it gets.
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Add server")
                Accessible.onPressAction: Window.window.openLoginDialog()

                Icon {
                    anchors.centerIn: parent
                    name: "plus"
                    size: 18
                    color: addArea.containsMouse ? Theme.accent : Theme.fg2
                }

                MouseArea {
                    id: addArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: Window.window.openLoginDialog()
                }

                ToolTip.visible: addArea.containsMouse
                ToolTip.text: "Add server"
                ToolTip.delay: 400
            }
        }
    }

    // Right-click server-context menu. This is the only place the
    // client surfaces a server's actual address + connection state
    // after it's been added, and the only UI path to edit/remove one.
    Menu {
        id: serverContextMenu
        property int serverIndex: -1
        // connectionAt re-resolves per open; the returned connection
        // object carries live connected/syncErrorMessage bindings.
        readonly property var conn: serverManager.connectionAt(serverIndex)
        readonly property string url: conn ? conn.serverUrl : ""

        function openFor(index) {
            serverIndex = index;
            popup();
        }

        background: Rectangle {
            color: Theme.bg1
            radius: Theme.r2
            border.color: Theme.line
            border.width: 1
            implicitWidth: 240
        }

        // Non-interactive header: address + live connection status.
        MenuItem {
            enabled: false
            implicitHeight: headerCol.implicitHeight + Theme.sp.s3 * 2
            // Not a control — it is the menu's title, and it is the only
            // place the address and the live connection state are said.
            // Said once, here, with the dot and both lines below ignored.
            Accessible.role: Accessible.StaticText
            Accessible.name: {
                var c = serverContextMenu.conn;
                var st = !c ? qsTr("Unknown")
                       : c.connectionStatus === 1 ? qsTr("Connected")
                       : c.connectionStatus === 2 ? qsTr("Reconnecting")
                       : (c.syncErrorMessage && c.syncErrorMessage.length > 0
                          ? c.syncErrorMessage : qsTr("Disconnected"));
                return qsTr("%1, %2").arg(serverContextMenu.url).arg(st);
            }
            contentItem: Column {
                id: headerCol
                spacing: 2
                leftPadding: Theme.sp.s3
                Text {
                    text: serverContextMenu.url
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSize.sm
                    color: Theme.fg1
                    elide: Text.ElideMiddle
                    width: 240 - Theme.sp.s3 * 2
                    Accessible.ignored: true
                }
                Row {
                    spacing: Theme.sp.s2
                    // connectionStatus is the sync loop's live verdict
                    // (0 disconnected / 1 healthy / 2 reconnecting /
                    // 3 session expired, which falls through to the
                    // danger branch and shows syncErrorMessage) —
                    // NOT `connected`, which flips true optimistically
                    // when credentials are set, before any sync
                    // succeeds. Must agree with MessageView's banner.
                    readonly property int st: serverContextMenu.conn
                        ? serverContextMenu.conn.connectionStatus : 0
                    Rectangle {
                        width: 8; height: 8; radius: 4
                        anchors.verticalCenter: parent.verticalCenter
                        color: parent.st === 1 ? Theme.online
                             : parent.st === 2 ? Theme.warn
                             : Theme.danger
                        Accessible.ignored: true
                    }
                    Text {
                        text: {
                            var c = serverContextMenu.conn;
                            if (!c) return "Unknown";
                            if (c.connectionStatus === 1) return "Connected";
                            if (c.connectionStatus === 2) return "Reconnecting…";
                            return c.syncErrorMessage && c.syncErrorMessage.length > 0
                                   ? c.syncErrorMessage : "Disconnected";
                        }
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                        color: parent.st === 1 ? Theme.fg2
                             : parent.st === 2 ? Theme.warn
                             : Theme.danger
                        elide: Text.ElideRight
                        width: 240 - Theme.sp.s3 * 2 - 14
                        Accessible.ignored: true
                    }
                }
            }
            background: null
        }

        MenuSeparator {
            contentItem: Rectangle {
                implicitHeight: 1
                color: Theme.lineSoft
            }
        }

        component ServerCtxItem: MenuItem {
            id: mi
            implicitHeight: visible ? 34 : 0
            height: implicitHeight
            property string iconName: ""
            property color labelColor: Theme.fg0

            // Floor for every row in this menu. Each instance names the
            // server it acts on, because "Remove server…" read out of
            // context does not say which one.
            Accessible.role: Accessible.Button
            Accessible.name: mi.text
            Accessible.onPressAction: mi.triggered()

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
                    Accessible.ignored: true
                }
            }
            background: Rectangle {
                color: mi.hovered && mi.enabled ? Theme.bg2 : "transparent"
                radius: Theme.r1
                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
            }
        }

        ServerCtxItem {
            text: "Copy address"
            iconName: "copy"
            Accessible.name: qsTr("Copy address %1").arg(serverContextMenu.url)
            onTriggered: serverManager.copyToClipboard(serverContextMenu.url)
        }
        ServerCtxItem {
            text: "Edit address…"
            iconName: "edit"
            Accessible.name: qsTr("Edit address %1").arg(serverContextMenu.url)
            onTriggered: {
                editServerDialog.serverIndex = serverContextMenu.serverIndex;
                editServerDialog.originalUrl = serverContextMenu.url;
                editServerDialog.open();
            }
        }
        ServerCtxItem {
            text: "Reconnect"
            iconName: "signal"
            Accessible.name: qsTr("Reconnect to %1").arg(serverContextMenu.url)
            // Hidden once the homeserver has rejected our token: a redial
            // cannot help, and offering it there is what sent people round
            // the loop in the 2026-09-19 purge. "Sign in again" replaces it.
            visible: !(serverContextMenu.conn
                       && serverContextMenu.conn.needsReauth === true)
            onTriggered: serverManager.reconnectServer(serverContextMenu.serverIndex)
        }
        ServerCtxItem {
            text: serverContextMenu.conn
                  && serverContextMenu.conn.reauthInProgress
                  ? "Signing in…" : "Sign in again"
            iconName: "signal"
            // The label flips to a progress word mid-action; the name
            // stays the action and the progress moves to the description.
            Accessible.name: qsTr("Sign in again to %1").arg(serverContextMenu.url)
            Accessible.description: serverContextMenu.conn
                                    && serverContextMenu.conn.reauthInProgress
                ? qsTr("Signing in") : qsTr("This server rejected the saved login")
            visible: !!serverContextMenu.conn
                     && serverContextMenu.conn.needsReauth === true
            enabled: visible
                     && !serverContextMenu.conn.reauthInProgress
            onTriggered: serverManager.reauthenticateServer(
                             serverContextMenu.serverIndex)
        }

        MenuSeparator {
            contentItem: Rectangle {
                implicitHeight: 1
                color: Theme.lineSoft
            }
        }

        ServerCtxItem {
            text: "Remove server…"
            iconName: "x"
            labelColor: Theme.danger
            Accessible.name: qsTr("Remove server %1").arg(serverContextMenu.url)
            onTriggered: {
                removeServerDialog.serverIndex = serverContextMenu.serverIndex;
                removeServerDialog.open();
            }
        }
    }

    // Edit-address dialog. Repoints the saved server entry and rebuilds
    // the connection with the stored credentials — no re-login needed
    // when the same server is reachable at the new address.
    Dialog {
        id: editServerDialog
        property int serverIndex: -1
        property string originalUrl: ""

        parent: Overlay.overlay
        anchors.centerIn: parent
        width: 400
        modal: true
        title: ""

        background: Rectangle {
            color: Theme.bg1
            radius: Theme.r2
            border.color: Theme.line
            border.width: 1
        }

        onOpened: {
            editUrlField.text = originalUrl;
            editUrlField.forceActiveFocus();
            editUrlField.selectAll();
        }

        contentItem: ColumnLayout {
            spacing: Theme.sp.s4

            Text {
                text: "Server address"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.lg
                font.weight: Theme.fontWeight.semibold
                color: Theme.fg0
            }
            Text {
                Layout.fillWidth: true
                text: "The connection is rebuilt with your existing login. "
                      + "Use the full base URL, e.g. http://192.168.1.20:8448"
                wrapMode: Text.WordWrap
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
            }
            TextField {
                id: editUrlField
                Layout.fillWidth: true
                Accessible.role: Accessible.EditableText
                Accessible.name: qsTr("Server address")
                placeholderText: "http://localhost:8448"
                placeholderTextColor: Theme.fg3
                color: Theme.fg0
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSize.sm
                background: Rectangle {
                    color: Theme.bg0
                    radius: Theme.r2
                    border.color: editUrlField.activeFocus ? Theme.accent : Theme.line
                    border.width: 1
                }
                padding: Theme.sp.s3
                onAccepted: editServerDialog.saveAndClose()
            }

            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: Theme.sp.s3

                Button {
                    text: "Cancel"
                    flat: true
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Cancel")
                    Accessible.description: qsTr("Leave the address unchanged")
                    Accessible.onPressAction: editServerDialog.close()
                    contentItem: Text {
                        text: parent.text
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        color: Theme.fg1
                        horizontalAlignment: Text.AlignHCenter
                        // The button is the node; its label is not a
                        // second one. Same everywhere below.
                        Accessible.ignored: true
                    }
                    background: Rectangle {
                        color: parent.hovered ? Theme.bg2 : "transparent"
                        radius: Theme.r2
                    }
                    onClicked: editServerDialog.close()
                }
                Button {
                    text: "Save"
                    enabled: editUrlField.text.trim().length > 0
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Save server address")
                    Accessible.onPressAction: editServerDialog.saveAndClose()
                    contentItem: Text {
                        text: parent.text
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.semibold
                        color: parent.enabled ? Theme.onAccent : Theme.fg3
                        horizontalAlignment: Text.AlignHCenter
                        Accessible.ignored: true
                    }
                    background: Rectangle {
                        color: parent.enabled
                               ? (parent.hovered ? Qt.lighter(Theme.accent, 1.1) : Theme.accent)
                               : Theme.bg2
                        radius: Theme.r2
                    }
                    onClicked: editServerDialog.saveAndClose()
                }
            }
        }

        function saveAndClose() {
            var url = editUrlField.text.trim();
            if (url.length === 0) return;
            if (url !== originalUrl)
                serverManager.updateServerUrl(serverIndex, url);
            close();
        }
    }

    // Remove-server confirm. Destructive: drops the saved login too.
    Dialog {
        id: removeServerDialog
        property int serverIndex: -1

        parent: Overlay.overlay
        anchors.centerIn: parent
        width: 360
        modal: true
        title: ""

        background: Rectangle {
            color: Theme.bg1
            radius: Theme.r2
            border.color: Theme.line
            border.width: 1
        }

        contentItem: ColumnLayout {
            spacing: Theme.sp.s4

            Text {
                text: "Remove server?"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.lg
                font.weight: Theme.fontWeight.semibold
                color: Theme.fg0
            }
            Text {
                Layout.fillWidth: true
                text: "This removes the server and its saved login from this "
                      + "device. Your account on the server is untouched."
                wrapMode: Text.WordWrap
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
            }

            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: Theme.sp.s3

                Button {
                    text: "Cancel"
                    flat: true
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Cancel")
                    Accessible.description: qsTr("Keep the server")
                    Accessible.onPressAction: removeServerDialog.close()
                    contentItem: Text {
                        text: parent.text
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        color: Theme.fg1
                        horizontalAlignment: Text.AlignHCenter
                        Accessible.ignored: true
                    }
                    background: Rectangle {
                        color: parent.hovered ? Theme.bg2 : "transparent"
                        radius: Theme.r2
                    }
                    onClicked: removeServerDialog.close()
                }
                Button {
                    text: "Remove"
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Remove server")
                    Accessible.description: qsTr(
                        "Removes the server and its saved login from this device")
                    Accessible.onPressAction: {
                        serverManager.removeServer(removeServerDialog.serverIndex);
                        removeServerDialog.close();
                    }
                    contentItem: Text {
                        text: parent.text
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.onAccent
                        horizontalAlignment: Text.AlignHCenter
                        Accessible.ignored: true
                    }
                    background: Rectangle {
                        color: parent.hovered ? Qt.lighter(Theme.danger, 1.1) : Theme.danger
                        radius: Theme.r2
                    }
                    onClicked: {
                        serverManager.removeServer(removeServerDialog.serverIndex);
                        removeServerDialog.close();
                    }
                }
            }
        }
    }
}
