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
}
