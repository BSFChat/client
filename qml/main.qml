import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import BSFChat

ApplicationWindow {
    id: root
    // Window geometry is restored from settings in Component.onCompleted.
    // These values are the first-run defaults only — negative persisted
    // coords fall back to here.
    width: 1280
    height: 720
    minimumWidth: 800
    minimumHeight: 500
    visible: true
    title: {
        var s = serverManager.activeServer;
        if (s && s.activeRoomId !== ""
            && s.isDirectRoom && s.isDirectRoom(s.activeRoomId)) {
            return "BSFChat — @" + s.directRoomPeer(s.activeRoomId);
        }
        if (serverManager.viewingDms) {
            return "BSFChat — Direct Messages";
        }
        if (s && s.activeRoomName !== "")
            return "BSFChat — #" + s.activeRoomName;
        return "BSFChat";
    }

    // Restore persisted geometry on first paint. Clamping: if a saved x/y
    // puts the window off-screen (e.g. a monitor got unplugged since last
    // run), fall back to Qt's default positioning so the app isn't stuck
    // in the void. We only check against the primary screen's available
    // geometry — a more thorough "is any screen covering this rect"
    // sweep would be nicer but this handles the common case.
    Component.onCompleted: {
        var w = appSettings.windowWidth;
        var h = appSettings.windowHeight;
        if (w > 0 && h > 0) { width = w; height = h; }

        var x_ = appSettings.windowX;
        var y_ = appSettings.windowY;
        if (x_ >= 0 && y_ >= 0 && Screen) {
            var sw = Screen.desktopAvailableWidth;
            var sh = Screen.desktopAvailableHeight;
            // Require at least a 40px strip of the window to remain on
            // the primary screen — otherwise ignore the stored position.
            if (x_ + 40 < sw && y_ + 40 < sh) {
                x = x_;
                y = y_;
            }
        }

        // Visibility: 2 = Windowed, 4 = Maximized, 5 = FullScreen.
        var vis = appSettings.windowVisibility;
        if (vis === 4) visibility = ApplicationWindow.Maximized;
        else if (vis === 5) visibility = ApplicationWindow.FullScreen;

        _restoredGeometry = true;

        // Restore the last text channel so the app opens into the
        // room the user was last reading, not the server's first
        // text channel. Voice rooms deliberately never auto-restore
        // (see MobileMain.qml for the same rationale).
        _maybeRestoreLastRoom();
    }

    // Auto-restore persisted channel. Called from Component.onCompleted
    // and re-invoked when the active server's room list populates
    // from /sync — because on a cold launch the room model may be
    // empty the moment we hit onCompleted.
    function _maybeRestoreLastRoom() {
        var s = serverManager ? serverManager.activeServer : null;
        if (!s || !s.roomListModel) return;
        if (s.activeRoomId && s.activeRoomId.length > 0) return;
        var remembered = appSettings.lastTextRoomFor(s.serverUrl);
        if (remembered && s.roomListModel.hasRoom(remembered)
            && !s.roomListModel.isVoiceRoom(remembered)) {
            s.setActiveRoom(remembered);
        }
    }
    Connections {
        target: serverManager && serverManager.activeServer
                ? serverManager.activeServer.roomListModel : null
        function onRowsInserted() { root._maybeRestoreLastRoom(); }
    }
    // Surface server-side send errors (429 rate-limit, M_FORBIDDEN,
    // generic failures) as toasts. ServerConnection pre-formats the
    // message + kind so QML just has to route.
    Connections {
        target: serverManager ? serverManager.activeServer : null
        ignoreUnknownSignals: true
        function onSendFeedback(text, kind) {
            if (toastHostGlobal && toastHostGlobal.show)
                toastHostGlobal.show(text, kind || "error");
        }
    }
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
    // Guard so geometry-save handlers ignore the restore-driven property
    // changes during the first paint.
    property bool _restoredGeometry: false

    // Debounced save — dragging/resizing fires x/y/width/height hundreds
    // of times; a 400ms settle avoids thrashing QSettings writes.
    Timer {
        id: geometrySaveTimer
        interval: 400
        onTriggered: {
            if (!root._restoredGeometry) return;
            if (root.visibility === ApplicationWindow.Windowed) {
                // Only store the window's geometry while it's in windowed
                // mode — maximised/fullscreen expose the screen size, not
                // the restored rect, which would cause the next launch to
                // open "maximised" as a plain window at full-screen size.
                appSettings.windowX = root.x;
                appSettings.windowY = root.y;
                appSettings.windowWidth = root.width;
                appSettings.windowHeight = root.height;
            }
            appSettings.windowVisibility = root.visibility;
        }
    }
    onXChanged:          geometrySaveTimer.restart()
    onYChanged:          geometrySaveTimer.restart()
    onWidthChanged:      geometrySaveTimer.restart()
    onHeightChanged:     geometrySaveTimer.restart()
    onVisibilityChanged: geometrySaveTimer.restart()
    // bg0 is the window bg per the Designer kit — sidebars lift to bg1,
    // popups to bg1/bg2. Using bg2 here made the "window" brighter than
    // the sidebars, inverting the hierarchy and robbing the accent tints
    // of contrast.
    color: Theme.bg0

    // Load Geist + Geist Mono from qrc. Variable-weight TTFs; Qt picks the
    // closest axis value off each Text item's `font.weight`. Without these
    // loaded, every `font.family: Theme.fontSans` would fall back to the
    // platform sans (SF Pro on macOS, Segoe UI on Win, whatever on Linux).
    FontLoader {
        id: geist
        source: "qrc:/qt/qml/BSFChat/qml/fonts/Geist-Variable.ttf"
    }
    FontLoader {
        id: geistMono
        source: "qrc:/qt/qml/BSFChat/qml/fonts/GeistMono-Variable.ttf"
    }

    // Bound two-way to AppSettings so toggling survives restart. The
    // chat-header users button and ⌃M shortcut both write through here.
    property bool showMemberList: appSettings.showMemberList
    onShowMemberListChanged: appSettings.showMemberList = showMemberList

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // Server rail (SPEC §3.1) — width driven from Theme.layout so the
        // 'compact' variant can narrow it to 60.
        ServerSidebar {
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.layout.serverRailW
        }

        // Channel separator
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.panelBorderWidth
            color: Theme.panelBorder
        }

        // Channel sidebar (SPEC §3.2) — width from Theme.layout, switches
        // with 'compact' / 'focus' variants.
        ChannelList {
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.layout.channelSidebarW
        }

        // Channel separator
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.panelBorderWidth
            color: Theme.panelBorder
        }

        // Main column (SPEC §1): main content stacks above the sticky
        // VoiceDock. Main content flips between VoiceRoom (when connected
        // to a voice channel) and MessageView (text channel reading). The
        // `activeServer.inVoiceChannel` toggle is what SPEC §1 means by
        // "main content can be VoiceRoom / ScreenShare / …".
        ColumnLayout {
            Layout.fillHeight: true
            Layout.fillWidth: true
            spacing: 0

            // View swap: MessageView vs. VoiceRoom. The voice CONNECTION
            // (inVoiceChannel) is orthogonal to the displayed VIEW
            // (viewingVoiceRoom) — you can be in voice and reading a text
            // channel. Clicking a text channel drops viewingVoiceRoom;
            // clicking the voice channel, VoiceDock, or VoiceStatusCard
            // raises it again.
            StackLayout {
                Layout.fillHeight: true
                Layout.fillWidth: true
                currentIndex: (serverManager.activeServer
                               && serverManager.activeServer.viewingVoiceRoom) ? 1 : 0

                MessageView { id: messageViewInstance }
                VoiceRoom  { }
            }

            VoiceDock {
                Layout.fillWidth: true
            }
        }

        // Member list separator
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.panelBorderWidth
            color: Theme.panelBorder
            visible: root.showMemberList && serverManager.activeServer !== null
                     && Theme.layout.memberListW > 0
        }

        // Member list
        MemberList {
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.layout.memberListW
            visible: root.showMemberList && serverManager.activeServer !== null
                     && Theme.layout.memberListW > 0
        }
    }

    // Login dialog
    LoginDialog {
        id: loginDialog
    }

    Connections {
        target: serverManager
        function onLoginError(serverUrl, error) {
            loginDialog.isConnecting = false;
            loginDialog.errorMessage = error;
        }
        function onLoginSuccess(serverUrl) {
            loginDialog.isConnecting = false;
            loginDialog.close();
            loginDialog.errorMessage = "";
        }
    }

    // Keyboard shortcut to toggle member list
    Shortcut {
        sequence: "Ctrl+M"
        onActivated: root.showMemberList = !root.showMemberList
    }

    // Cmd+1..9 (Ctrl+1..9 on Win/Linux) selects servers 0..8.
    // Qt maps "Ctrl+N" to the platform's primary modifier automatically,
    // so a single sequence works cross-platform.
    function activateServer(index) {
        if (!serverManager.servers) return;
        if (index < 0 || index >= serverManager.servers.rowCount()) return;
        serverManager.setActiveServer(index);
    }
    // Ctrl+Tab / Ctrl+Shift+Tab — cycle servers forward/backward.
    // On macOS, Qt maps "Ctrl" to Cmd (which is the app-switcher and
    // intercepted by the OS). Physical Ctrl is "Meta" in Qt's macOS
    // mapping. We bind both so the same physical key works cross-platform.
    function cycleServer(dir) {
        if (!serverManager.servers) return;
        var count = serverManager.servers.rowCount();
        if (count < 2) return;
        var next = (serverManager.activeServerIndex + dir + count) % count;
        serverManager.setActiveServer(next);
    }
    Shortcut { sequence: "Ctrl+Tab";       onActivated: root.cycleServer(1) }
    Shortcut { sequence: "Meta+Tab";       onActivated: root.cycleServer(1) }
    Shortcut { sequence: "Ctrl+Shift+Tab"; onActivated: root.cycleServer(-1) }
    Shortcut { sequence: "Meta+Shift+Tab"; onActivated: root.cycleServer(-1) }

    Shortcut { sequence: "Ctrl+1"; onActivated: root.activateServer(0) }
    Shortcut { sequence: "Ctrl+2"; onActivated: root.activateServer(1) }
    Shortcut { sequence: "Ctrl+3"; onActivated: root.activateServer(2) }
    Shortcut { sequence: "Ctrl+4"; onActivated: root.activateServer(3) }
    Shortcut { sequence: "Ctrl+5"; onActivated: root.activateServer(4) }
    Shortcut { sequence: "Ctrl+6"; onActivated: root.activateServer(5) }
    Shortcut { sequence: "Ctrl+7"; onActivated: root.activateServer(6) }
    Shortcut { sequence: "Ctrl+8"; onActivated: root.activateServer(7) }
    Shortcut { sequence: "Ctrl+9"; onActivated: root.activateServer(8) }

    // Alt+Up / Alt+Down — cycle through text channels of the active server,
    // skipping categories and voice channels. Wraps at both ends.
    Shortcut {
        sequence: "Alt+Up"
        onActivated: root.cycleTextChannel(-1)
    }
    Shortcut {
        sequence: "Alt+Down"
        onActivated: root.cycleTextChannel(1)
    }

    // Flattens the currently visible categorized channel list into an
    // ordered array of text channels, locates the active room, and hops
    // `dir` (-1 / +1) with wrap.
    function cycleTextChannel(dir) {
        if (!serverManager.activeServer) return;
        var groups = serverManager.activeServer.categorizedRooms;
        var flat = [];
        for (var i = 0; i < groups.length; i++) {
            var ch = groups[i].channels || [];
            for (var j = 0; j < ch.length; j++) {
                if (!ch[j].isVoice) flat.push(ch[j].roomId);
            }
        }
        if (flat.length === 0) return;
        var active = serverManager.activeServer.activeRoomId || "";
        var idx = flat.indexOf(active);
        if (idx < 0) {
            // Nothing active yet — pick the first/last in the direction.
            idx = dir > 0 ? -1 : flat.length;
        }
        var next = (idx + dir + flat.length) % flat.length;
        serverManager.activeServer.setActiveRoom(flat[next]);
    }

    // Keyboard shortcuts for actions that were previously in the platform
    // menu bar. The menu bar itself is removed — it looked wrong on
    // Windows/Linux and all actions are in the user-profile popup anyway.
    Shortcut { sequence: "Ctrl+,"; onActivated: userSettingsGlobal.open() }
    Shortcut { sequence: "Ctrl+Shift+,"; onActivated: clientSettingsGlobal.open() }
    Shortcut { sequence: "Ctrl+/"; onActivated: shortcutsDialogGlobal.open() }
    Shortcut { sequence: "Ctrl+K"; onActivated: searchPopupGlobal.open() }
    // Ctrl+L focuses the message composer — standard "go to the
    // input field" shortcut borrowed from terminals + Discord.
    Shortcut {
        sequence: "Ctrl+L"
        onActivated: {
            if (serverManager.activeServer
                && serverManager.activeServer.activeRoomId) {
                root.focusComposer();
            }
        }
    }

    // Push-to-talk hotkey. Only meaningful while voiceMode === "ptt" and
    // the user is in a voice call. autoRepeat re-fires every ~30ms while
    // held, so the release timer acts as a "finger off key" detector —
    // once the shortcut stops firing for `interval` ms, we release PTT.
    Shortcut {
        sequence: appSettings.pttKeySequence
        enabled: appSettings.voiceMode === "ptt"
              && serverManager.activeServer
              && serverManager.activeServer.inVoiceChannel
        autoRepeat: true
        context: Qt.ApplicationShortcut
        onActivated: {
            if (serverManager.activeServer)
                serverManager.activeServer.setPttPressed(true);
            pttReleaseTimer.restart();
        }
    }
    Timer {
        id: pttReleaseTimer
        interval: 120
        onTriggered: {
            if (serverManager.activeServer)
                serverManager.activeServer.setPttPressed(false);
        }
    }

    // Global popup instances. ChannelList reaches these via
    // ApplicationWindow.window.openUserSettings() / openClientSettings()
    // because QML ids don't cross file boundaries.
    UserSettings {
        id: userSettingsGlobal
        parent: Overlay.overlay
    }
    ClientSettings {
        id: clientSettingsGlobal
        parent: Overlay.overlay
    }
    ShortcutsDialog {
        id: shortcutsDialogGlobal
        parent: Overlay.overlay
    }
    SearchPopup {
        id: searchPopupGlobal
        parent: Overlay.overlay
        onResultClicked: (eventId) => {
            // MessageView subscribes to scrollToEventRequested on the
            // active ServerConnection — reuse the same channel the
            // cross-server jump path uses.
            var s = serverManager.activeServer;
            if (s) s.scrollToEventRequested(eventId);
        }
    }
    // Global role-assignment popup — reachable from any component via
    // Window.window.openRoleAssignment(userId, displayName).
    RoleAssignPopup {
        id: roleAssignGlobal
        parent: Overlay.overlay
    }

    // Direct-messages surface now lives inline in ChannelList
    // (overlayed when `serverManager.viewingDms`); no separate
    // popup. openDirectMessages() just flips the view flag.

    // Presence + custom-status picker, reachable from the user
    // menu in the channel-list footer (and the mobile overflow).
    StatusPicker {
        id: statusPickerGlobal
        parent: Overlay.overlay
    }

    // App-wide toast surface. Every subsystem reports success/failure
    // through `Window.window.toast(text, kind)` or one of the kind-
    // shortcut helpers. Parented to Overlay.overlay so toasts float
    // above any open modal too.
    ToastHost {
        id: toastHostGlobal
        parent: Overlay.overlay
    }

    function openUserSettings() { userSettingsGlobal.open(); }
    function openClientSettings() { clientSettingsGlobal.open(); }
    function openShortcutsDialog() { shortcutsDialogGlobal.open(); }
    function openSearch() { searchPopupGlobal.open(); }
    // DM chip click / overflow-menu / shortcut all route here.
    // Flipping the view flag is cheaper than opening a popup and
    // matches the "DMs sit in the sidebar as a destination" model.
    function openDirectMessages() {
        serverManager.setViewingDms(true);
    }
    function openStatusPicker() { statusPickerGlobal.open(); }
    // Forward the Ctrl+L shortcut — MessageView exposes
    // `focusComposer()` that walks to the MessageInput's TextArea
    // and forceActiveFocus().
    function focusComposer() {
        if (messageViewInstance && messageViewInstance.focusComposer)
            messageViewInstance.focusComposer();
    }
    function openRoleAssignment(userId, displayName) {
        roleAssignGlobal.openFor(userId, displayName);
    }
    // Toast API — kind defaults to "info". Shortcut helpers are also
    // exposed so call sites read naturally (`Window.window.toastError(...)`).
    function toast(text, kind)   { toastHostGlobal.toast(text, kind || "info"); }
    function toastInfo(text)     { toastHostGlobal.info(text); }
    function toastSuccess(text)  { toastHostGlobal.success(text); }
    function toastWarn(text)     { toastHostGlobal.warn(text); }
    function toastError(text)    { toastHostGlobal.error(text); }

    // Subscribe the toast surface to every error signal we currently
    // have. `Connections.target` re-binds when activeServer changes, so
    // we pick up the right ServerConnection without manual bookkeeping.
    Connections {
        target: serverManager
        function onLoginError(serverUrl, error) {
            toastError("Login failed: " + error);
        }
        function onIdentityLoginFailed(error) {
            toastError("Identity login failed: " + error);
        }
    }
    Connections {
        target: serverManager.activeServer
        ignoreUnknownSignals: true
        function onLoginFailed(error) {
            toastError("Login failed: " + error);
        }
        function onRegisterFailed(error) {
            toastError("Registration failed: " + error);
        }
        function onMediaSendFailed(error) {
            toastError("Upload failed: " + error);
        }
        function onMediaSendCompleted() {
            toastSuccess("Upload complete");
        }
        function onStateWriteFailed(kind, status, error) {
            // ServerSettings has its own inline banner for the cases
            // where a setting dialog is open. Show a toast for the
            // "someone clicked Save then wandered away" case, too.
            var msg;
            switch (kind) {
                case "role-assign":     msg = "Couldn't save role assignments"; break;
                case "server-name":     msg = "Couldn't update server name"; break;
                case "channel-override": msg = "Couldn't save channel permissions"; break;
                case "channel-settings": msg = "Couldn't update channel settings"; break;
                case "server-roles":    msg = "Couldn't save server roles"; break;
                default:                msg = "Save failed";
            }
            if (status === 403) msg += " — you don't have permission.";
            else if (error && error.length > 0) msg += " — " + error;
            toastError(msg);
        }
    }
}
