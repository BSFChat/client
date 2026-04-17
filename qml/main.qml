import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.platform as Platform
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
    color: Theme.bgMedium

    property bool showMemberList: true

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // Server sidebar
        ServerSidebar {
            Layout.fillHeight: true
            Layout.preferredWidth: 72
        }

        // Channel separator
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            color: Theme.bgDarkest
        }

        // Channel list
        ChannelList {
            Layout.fillHeight: true
            Layout.preferredWidth: 240
        }

        // Channel separator
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            color: Theme.bgDarkest
        }

        // Message view (main area)
        MessageView {
            Layout.fillHeight: true
            Layout.fillWidth: true
        }

        // Member list separator
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            color: Theme.bgDarkest
            visible: root.showMemberList && serverManager.activeServer !== null
        }

        // Member list
        MemberList {
            Layout.fillHeight: true
            Layout.preferredWidth: 240
            visible: root.showMemberList && serverManager.activeServer !== null
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

    // Native menu bar. On macOS appears in the system menu bar; on Linux/
    // Windows shows at the top of the window. Mirrors the per-user popup in
    // the profile block so both entry points reach the same dialogs.
    Platform.MenuBar {
        Platform.Menu {
            title: qsTr("File")
            Platform.MenuItem {
                text: qsTr("Manage Account…")
                enabled: serverManager.activeServer !== null
                onTriggered: {
                    var base = serverManager.activeServer
                        ? serverManager.activeServer.identityProviderUrl()
                        : "";
                    if (!base) base = "https://id.bsfchat.com";
                    Qt.openUrlExternally(base + "/profile.html");
                }
            }
            Platform.MenuItem {
                text: qsTr("Edit Server Profile…")
                shortcut: "Ctrl+,"
                enabled: serverManager.activeServer !== null
                onTriggered: userSettingsGlobal.open()
            }
            Platform.MenuItem {
                text: qsTr("Client Settings…")
                shortcut: "Ctrl+Shift+,"
                onTriggered: clientSettingsGlobal.open()
            }
            Platform.MenuSeparator {}
            Platform.MenuItem {
                text: qsTr("Quit")
                role: Platform.MenuItem.QuitRole
                shortcut: StandardKey.Quit
                onTriggered: Qt.quit()
            }
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

    function openUserSettings() { userSettingsGlobal.open(); }
    function openClientSettings() { clientSettingsGlobal.open(); }
}
