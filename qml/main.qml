import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

ApplicationWindow {
    id: root
    width: 1280
    height: 720
    minimumWidth: 800
    minimumHeight: 500
    visible: true
    title: {
        if (serverManager.activeServer && serverManager.activeServer.activeRoomName !== "")
            return "BSFChat - #" + serverManager.activeServer.activeRoomName;
        return "BSFChat";
    }
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

    property bool showMemberList: true

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

                MessageView { }
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

    function openUserSettings() { userSettingsGlobal.open(); }
    function openClientSettings() { clientSettingsGlobal.open(); }
}
