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

    // Native menu bar. On macOS appears in the system menu bar; on Linux/
    // Windows shows at the top of the window. Mirrors the per-user popup in
    // the profile block so both entry points reach the same dialogs.
    Platform.MenuBar {
        Platform.Menu {
            title: qsTr("File")
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
