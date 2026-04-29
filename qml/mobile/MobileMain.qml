import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import BSFChat

// Mobile entry point — loaded instead of main.qml on iOS/Android.
// Reuses every desktop leaf component (MessageView, MessageBubble,
// MessageInput, MemberList, ChannelList) inside a phone-native chrome:
// left drawer for servers+channels, right drawer for members, chat
// occupies the main column.
//
// The three-panel desktop layout doesn't fit a phone; we swap it for
// an overlay-drawer pattern borrowed from Discord / Slack mobile.
// Server rail + channel list live in the left drawer; member list
// lives in the right drawer; everything else is the chat view.
ApplicationWindow {
    id: root
    visible: true
    visibility: Window.Maximized
    color: Theme.bg0

    // Software keyboard avoidance. Tracked so layout can bind its
    // bottom margin; Android's adjustResize should auto-handle the
    // window-level resize for us, but modal dialogs parent to the
    // Overlay which doesn't participate in adjustResize — hence the
    // manual push applied via a Binding below.
    readonly property int keyboardHeight: Qt.inputMethod.visible
        ? Math.ceil(Qt.inputMethod.keyboardRectangle.height
            / (Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1))
        : 0

    // ── Safe-area handling ───────────────────────────────────────
    // Qt's ApplicationWindow on Android already reserves space for the
    // system status bar (it doesn't draw edge-to-edge by default), so
    // we don't need to add our own top padding — doing so stacks an
    // extra status-bar-sized gap above the header and makes the top
    // chrome look comically tall. Kept as a 0 here (rather than
    // removing it entirely) so anywhere in the tree that reads
    // `Window.window.topInset` still has something to bind to; iOS
    // may bring this back under QQuickWindow.safeAreaMargins in a
    // later Qt release.
    readonly property int topInset: 0
    // Bottom inset for the iOS home indicator / Android gesture bar.
    // Use QQuickWindow.safeAreaMargins if Qt 6.7+, else a 16px
    // fallback that looks right on a Pixel / Samsung gesture strip.
    readonly property int bottomInset: 16

    // Android hardware-back should cascade through the UI: close the
    // nearest drawer/popup, not blow past everything and quit the
    // app. Qt 6 fires Keys.onBackPressed on ApplicationWindow for
    // the Android hardware-back gesture. We intercept and dispatch.
    onClosing: (close) => {
        // Cascade: thread panel → right drawer → left drawer → modal
        // popups → flip out of voice-room view → default back-to-OS.
        // We don't leave the voice channel on back — the user can
        // keep listening while reading a text channel or another
        // app foreground — only the full-screen voice VIEW closes.
        if (threadPanelOpen()) {
            close.accepted = false;
            closeThread();
        } else if (rightDrawer.opened) {
            close.accepted = false;
            rightDrawer.close();
        } else if (leftDrawer.opened) {
            close.accepted = false;
            leftDrawer.close();
        } else if (searchPopupGlobal.opened) {
            close.accepted = false;
            searchPopupGlobal.close();
        } else if (serverManager.viewingDms
                   && (!serverManager.activeServer
                       || !serverManager.activeServer.activeRoomId)) {
            // DM view with no DM selected → back to normal
            // channel view; matches the "back unwinds DM mode"
            // expectation when there's no active conversation.
            close.accepted = false;
            serverManager.setViewingDms(false);
        } else if (clientSettingsGlobal.opened) {
            close.accepted = false;
            clientSettingsGlobal.close();
        } else if (userSettingsGlobal.opened) {
            close.accepted = false;
            userSettingsGlobal.close();
        } else if (serverManager.activeServer
                   && serverManager.activeServer.viewingVoiceRoom) {
            close.accepted = false;
            serverManager.activeServer.setActiveRoom(
                serverManager.activeServer.activeRoomId);
        }
    }
    // ThreadPanel doesn't have a global id to poke — use a helper.
    function threadPanelOpen() {
        return chatView && chatView.threadPanelOpen
            ? chatView.threadPanelOpen() : false;
    }
    function closeThread() {
        if (chatView && chatView.closeThread) chatView.closeThread();
    }

    // Reactive emptiness check — `servers.rowCount` is a function on
    // QAbstractListModel, not a property, so comparing directly
    // always reads as truthy. Poll via an invocation bound to
    // ListView.count on a hidden Instantiator so it updates live.
    property int _serverCount: 0
    Instantiator {
        active: serverManager && serverManager.servers
        model: serverManager ? serverManager.servers : null
        delegate: QtObject {}
        onObjectAdded: root._serverCount = count
        onObjectRemoved: root._serverCount = count
        onModelChanged: root._serverCount = model ? model.rowCount() : 0
    }
    readonly property bool _noServers: _serverCount === 0

    // Toast host for every subsystem — reachable via Window.window.toast().
    ToastHost { id: toastHostGlobal; parent: Overlay.overlay }
    function toast(t, kind) { toastHostGlobal.show(t, kind || "info"); }
    function toastError(t)   { toast(t, "error"); }
    function toastSuccess(t) { toast(t, "success"); }

    // ── Top bar ──────────────────────────────────────────────────
    // Minimal: channel name + burger (drawers). Title taps open the
    // channel list drawer; the avatar on the right opens the member
    // list drawer. Status-bar padding via topInset.
    header: Rectangle {
        color: Theme.bg1
        height: 48 + root.topInset

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width; height: 1; color: Theme.line
        }

        RowLayout {
            anchors.fill: parent
            anchors.topMargin: root.topInset
            anchors.leftMargin: Theme.sp.s4
            anchors.rightMargin: Theme.sp.s4
            spacing: Theme.sp.s3

            // Burger → left drawer
            Rectangle {
                Layout.preferredWidth: 40
                Layout.preferredHeight: 40
                radius: Theme.r1
                color: burgerMouse.pressed ? Theme.bg3 : "transparent"
                // TalkBack / VoiceOver read this as "Channels, button".
                // Role=Button + name + clickable onPressAction gives
                // screen-reader users a usable navigation target.
                Accessible.role: Accessible.Button
                Accessible.name: "Channels"
                Accessible.description: "Open the server and channel drawer"
                Accessible.onPressAction: leftDrawer.open()
                Icon { anchors.centerIn: parent; name: "menu"; size: 20; color: Theme.fg0 }
                MouseArea {
                    id: burgerMouse
                    anchors.fill: parent
                    onClicked: leftDrawer.open()
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Text {
                    text: {
                        var s = serverManager.activeServer;
                        // In-DM always takes priority — the title
                        // shows the peer you're talking to.
                        if (s && s.activeRoomId
                            && s.isDirectRoom && s.isDirectRoom(s.activeRoomId)) {
                            return "@" + s.directRoomPeer(s.activeRoomId);
                        }
                        // DM view with nothing selected yet — show
                        // the section title so the user knows why
                        // they don't see channels.
                        if (serverManager.viewingDms) return "Direct Messages";
                        if (!s || !s.activeRoomId) return "BSFChat";
                        var n = s.roomListModel
                            ? s.roomListModel.roomDisplayName(s.activeRoomId)
                            : s.activeRoomId;
                        return "#" + n;
                    }
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.lg
                    font.weight: Theme.fontWeight.semibold
                    color: Theme.fg0
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                Text {
                    text: serverManager.activeServer
                        ? serverManager.activeServer.serverName : ""
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xs
                    color: Theme.fg3
                    visible: text.length > 0
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
            }

            Rectangle {
                Layout.preferredWidth: 40
                Layout.preferredHeight: 40
                radius: Theme.r1
                color: membersMouse.pressed ? Theme.bg3 : "transparent"
                Accessible.role: Accessible.Button
                Accessible.name: "Members"
                Accessible.description: "Open the member list"
                Accessible.onPressAction: rightDrawer.open()
                Icon { anchors.centerIn: parent; name: "users"; size: 20; color: Theme.fg0 }
                MouseArea {
                    id: membersMouse
                    anchors.fill: parent
                    onClicked: rightDrawer.open()
                }
            }

            Rectangle {
                Layout.preferredWidth: 40
                Layout.preferredHeight: 40
                radius: Theme.r1
                color: overflowMouse.pressed ? Theme.bg3 : "transparent"
                Icon {
                    anchors.centerIn: parent
                    name: "more-horizontal"
                    size: 20
                    color: Theme.fg0
                }
                MouseArea {
                    id: overflowMouse
                    anchors.fill: parent
                    onClicked: overflowMenu.popup(parent, parent.width - 200, parent.height)
                }

                // Overflow menu: settings, search, sign out. Adds back
                // the entry points that the desktop version scatters
                // across chat-header buttons + footer gear — none of
                // which are visible on mobile.
                Menu {
                    id: overflowMenu
                    background: Rectangle {
                        color: Theme.bg1
                        radius: Theme.r2
                        border.color: Theme.line
                        border.width: 1
                        implicitWidth: 200
                    }

                    component OverflowItem: MenuItem {
                        id: omi
                        implicitHeight: 40
                        property string iconName: ""
                        contentItem: RowLayout {
                            spacing: Theme.sp.s3
                            Icon {
                                name: omi.iconName
                                size: 14
                                color: omi.hovered ? Theme.fg0 : Theme.fg2
                                Layout.leftMargin: Theme.sp.s3
                            }
                            Text {
                                text: omi.text
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.md
                                color: Theme.fg0
                                Layout.fillWidth: true
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                        background: Rectangle {
                            color: omi.hovered ? Theme.bg2 : "transparent"
                            radius: Theme.r1
                        }
                    }

                    OverflowItem {
                        text: {
                            var s = serverManager.activeServer;
                            if (!s) return "Set status…";
                            var msg = s.selfStatusMessage();
                            return msg && msg.length > 0
                                ? "Status: " + msg : "Set status…";
                        }
                        iconName: "smile"
                        onTriggered: root.openStatusPicker()
                    }
                    OverflowItem {
                        text: "Direct messages"
                        iconName: "at"
                        onTriggered: root.openDirectMessages()
                    }
                    OverflowItem {
                        text: "Search messages"
                        iconName: "search"
                        onTriggered: root.openSearch()
                    }
                    OverflowItem {
                        text: "Client settings"
                        iconName: "settings"
                        onTriggered: root.openClientSettings()
                    }
                    OverflowItem {
                        text: "Your profile"
                        iconName: "at"
                        onTriggered: root.openUserSettings()
                    }

                    MenuSeparator { }

                    // Switch server / add another account. Opens the
                    // same LoginDialog used on first launch; users
                    // can pick an existing server from the list or
                    // add a new one without having to sign out first.
                    OverflowItem {
                        text: "Switch server…"
                        iconName: "forward"
                        onTriggered: loginDialogGlobal.open()
                    }

                    // Sign out of the active server. Preserves saved
                    // credentials for other servers — we remove just
                    // the current one.
                    OverflowItem {
                        text: "Sign out"
                        iconName: "phone-off"
                        enabled: serverManager.activeServerIndex >= 0
                        onTriggered: {
                            var idx = serverManager.activeServerIndex;
                            if (idx < 0) return;
                            serverManager.removeServer(idx);
                        }
                    }
                }
            }
        }
    }

    // ── Main content ─────────────────────────────────────────────
    // MessageView fills the screen; on no-active-channel, an empty
    // state promotes the user to open the drawer. When the user is
    // connected to a voice room and has flipped to the voice view
    // (via tapping the voice channel, VoiceStatusCard, or VoiceDock),
    // the voice surface takes over the main column.
    Rectangle {
        id: mainArea
        anchors.fill: parent
        color: Theme.bg0

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: (serverManager.activeServer
                               && serverManager.activeServer.viewingVoiceRoom) ? 1 : 0

                MessageView {
                    id: chatView
                    visible: serverManager.activeServer
                          && serverManager.activeServer.activeRoomId !== ""
                }
                VoiceRoom { id: voiceRoomView }
            }

            // Persistent VoiceDock — only rendered while connected to
            // a voice channel. Gives the user a one-tap mute/deafen
            // target and a way to flip back to the chat view.
            VoiceDock {
                Layout.fillWidth: true
                visible: serverManager.activeServer
                      && serverManager.activeServer.inVoiceChannel
            }
        }

        ColumnLayout {
            anchors.centerIn: parent
            visible: !chatView.visible
            spacing: Theme.sp.s5
            Icon {
                name: _noServers ? "plus" : "hash"
                size: 48
                color: Theme.fg3
                Layout.alignment: Qt.AlignHCenter
            }
            Text {
                text: _noServers
                    ? "No servers yet"
                    : "No channel selected"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xl
                font.weight: Theme.fontWeight.semibold
                color: Theme.fg1
                Layout.alignment: Qt.AlignHCenter
            }
            Text {
                text: _noServers
                    ? "Sign in to a BSFChat server to start chatting."
                    : "Tap the menu button to pick a server and channel."
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.md
                color: Theme.fg3
                Layout.alignment: Qt.AlignHCenter
                horizontalAlignment: Text.AlignHCenter
                Layout.preferredWidth: Math.min(parent.width * 0.8, 320)
                wrapMode: Text.WordWrap
            }
            // Re-open the login dialog — first-launch opens it via
            // Component.onCompleted but if it closes we need a way
            // back in that doesn't require a force-quit.
            Button {
                visible: _noServers
                text: "Add a server"
                Layout.alignment: Qt.AlignHCenter
                onClicked: loginDialogGlobal.open()
                contentItem: Text {
                    text: parent.text
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    font.weight: Theme.fontWeight.semibold
                    color: Theme.onAccent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: parent.hovered ? Theme.accentDim : Theme.accent
                    radius: Theme.r2
                    implicitWidth: 180
                    implicitHeight: 44
                }
            }
        }
    }

    // ── Left drawer: servers + channels ──────────────────────────
    Drawer {
        id: leftDrawer
        width: Math.min(root.width * 0.85, 340)
        height: root.height
        edge: Qt.LeftEdge
        // Swipe-from-edge gesture area — Qt's Drawer defaults to a
        // 20-px hot edge which feels natural.

        background: Rectangle { color: Theme.bg1 }

        RowLayout {
            anchors.fill: parent
            spacing: 0

            // Server rail — reused as-is (72px wide).
            ServerSidebar {
                Layout.preferredWidth: Theme.layout.serverRailW
                Layout.fillHeight: true
            }

            // Channel list — reused as-is.
            ChannelList {
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
        }

        // Auto-close after the user picks a channel so they drop
        // straight into the chat. Binding-as-trigger: cache the
        // last seen activeRoomId and close when it changes. More
        // reliable than Connections{} which wasn't firing here —
        // likely because `serverManager.activeServer` is a
        // property whose changes re-target the Connections without
        // re-wiring activeRoomIdChanged.
        property string _lastRoom: ""
        readonly property string _currentRoom: serverManager.activeServer
            ? serverManager.activeServer.activeRoomId : ""
        on_CurrentRoomChanged: {
            if (_currentRoom !== "" && _currentRoom !== _lastRoom) {
                _lastRoom = _currentRoom;
                leftDrawer.close();
            }
        }
    }

    // ── Right drawer: member list ────────────────────────────────
    Drawer {
        id: rightDrawer
        width: Math.min(root.width * 0.75, 280)
        height: root.height
        edge: Qt.RightEdge
        background: Rectangle { color: Theme.bg1 }

        MemberList { anchors.fill: parent }
    }

    // Global popups — reachable via Window.window.openXyz() helpers.
    UserSettings   { id: userSettingsGlobal;   parent: Overlay.overlay }
    ClientSettings { id: clientSettingsGlobal; parent: Overlay.overlay }
    RoleAssignPopup { id: roleAssignGlobal;    parent: Overlay.overlay }
    SearchPopup {
        id: searchPopupGlobal
        parent: Overlay.overlay
        onResultClicked: (eventId) => {
            if (serverManager.activeServer)
                serverManager.activeServer.scrollToEventRequested(eventId);
        }
    }
    StatusPicker {
        id: statusPickerGlobal
        parent: Overlay.overlay
    }

    function openUserSettings()   { userSettingsGlobal.open(); }
    function openClientSettings() { clientSettingsGlobal.open(); }
    function openSearch()         { searchPopupGlobal.open(); }
    function openStatusPicker()   { statusPickerGlobal.open(); }
    // Drawer + sidebar need to be open for the DM list to be
    // visible; flipping `viewingDms` alone would leave the user
    // staring at "No channel selected" with no obvious next step.
    function openDirectMessages() {
        serverManager.setViewingDms(true);
        leftDrawer.open();
    }
    function openShortcutsDialog() { /* no-op on mobile */ }
    function openRoleAssignment(userId, displayName) {
        roleAssignGlobal.openFor(userId, displayName);
    }

    // showMemberList on mobile is always "the right drawer"; shim
    // the desktop-level property for components that peek at it.
    property bool showMemberList: rightDrawer.opened

    // Login dialog when not authenticated to any server. Mobile
    // builds hit the same LoginDialog — OIDC flow works in-process
    // via the existing UrlHandler + WebView on mobile.
    LoginDialog {
        id: loginDialogGlobal
        parent: Overlay.overlay
    }
    Component.onCompleted: {
        if (!serverManager || !serverManager.servers
            || serverManager.servers.rowCount() === 0) {
            loginDialogGlobal.open();
        } else {
            _maybeAutoSelect();
        }
    }

    // Drop the user into a channel the moment we have one. Prefer
    // the channel they were last reading on this server — stored in
    // appSettings by the persistence Connections block below — and
    // fall back to the first text room if that room is unknown (new
    // login, room deleted, etc.). Voice rooms are never restored on
    // launch: auto-rejoining voice would push the user's mic onto
    // the network the instant the app opens, which is a bad default.
    function _maybeAutoSelect() {
        var s = serverManager ? serverManager.activeServer : null;
        if (!s) {
            if (serverManager && serverManager.activeServerIndex < 0
                && serverManager.servers
                && serverManager.servers.rowCount() > 0) {
                serverManager.setActiveServer(0);
                Qt.callLater(_maybeAutoSelect);
            }
            return;
        }
        if (s.activeRoomId && s.activeRoomId.length > 0) return;
        if (!s.roomListModel) return;

        // 1) Try the remembered channel for this server.
        var remembered = appSettings.lastTextRoomFor(s.serverUrl);
        if (remembered && s.roomListModel.hasRoom(remembered)
            && !s.roomListModel.isVoiceRoom(remembered)) {
            s.setActiveRoom(remembered);
            return;
        }
        // 2) Otherwise pick the first text room.
        var rid = s.roomListModel.firstTextRoomId();
        if (rid && rid.length > 0) s.setActiveRoom(rid);
    }

    // If rooms populate asynchronously (initial /sync lands after
    // Component.onCompleted), retry once the active server announces
    // a new room list.
    Connections {
        target: serverManager && serverManager.activeServer
                ? serverManager.activeServer.roomListModel : null
        function onRowsInserted() { _maybeAutoSelect(); }
    }

    // Send-side feedback (rate limits, permission errors, …) as
    // toasts. ServerConnection pre-formats the copy + severity.
    Connections {
        target: serverManager ? serverManager.activeServer : null
        ignoreUnknownSignals: true
        function onSendFeedback(text, kind) {
            root.toast(text, kind || "error");
        }
    }

    // Persist the active text channel every time it changes. Skip
    // voice rooms so a quick-tap into voice doesn't clobber the
    // "last text channel" memory — on restart we'd jump into voice
    // and (accidentally) transmit.
    Connections {
        target: serverManager ? serverManager.activeServer : null
        ignoreUnknownSignals: true
        function onActiveRoomIdChanged() {
            var s = serverManager.activeServer;
            if (!s || !s.roomListModel) return;
            var rid = s.activeRoomId;
            if (!rid || rid.length === 0) return;
            if (s.roomListModel.isVoiceRoom(rid)) return;
            appSettings.setLastTextRoomFor(s.serverUrl, rid);
        }
    }

    // Android "Share to BSFChat" handler — sends the shared payload
    // to the currently-active channel. Text payloads become normal
    // messages; files go through the media-upload pipeline.
    Connections {
        target: typeof urlHandler !== "undefined" ? urlHandler : null
        function onSharedPayloadReceived(payload, mimeType, isFile) {
            var s = serverManager.activeServer;
            if (!s || !s.activeRoomId || s.activeRoomId.length === 0) {
                // No channel open — stash for the user to retry once
                // they pick one. For now just toast.
                toast("Pick a channel first, then share again.", "info");
                return;
            }
            if (isFile) {
                s.sendMediaMessage(payload);
                toast("Uploading shared file…", "info");
            } else {
                s.sendMessage(payload);
                toast("Shared text posted", "success");
            }
        }
    }
}
