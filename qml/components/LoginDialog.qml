import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

Dialog {
    id: dialog
    title: "Add Server"
    anchors.centerIn: parent
    width: 400
    modal: true
    standardButtons: Dialog.NoButton

    property string errorMessage: ""
    property bool isConnecting: false
    property bool checkingFlows: false
    property bool oidcAvailable: false
    property string oidcProviderUrl: ""
    property bool passwordAvailable: true
    property bool oidcInProgress: false

    background: Rectangle {
        color: Theme.bgMedium
        radius: Theme.radiusNormal
        border.color: Theme.bgLight
        border.width: 1
    }

    header: Rectangle {
        color: "transparent"
        height: 60
        Text {
            anchors.centerIn: parent
            text: "Add a Server"
            font.pixelSize: 20
            font.bold: true
            color: Theme.textPrimary
        }
    }

    Connections {
        target: serverManager
        function onLoginFlowsChecked(url, oidcAvail, providerUrl, passwordAvail) {
            dialog.checkingFlows = false;
            dialog.oidcAvailable = oidcAvail;
            dialog.oidcProviderUrl = providerUrl;
            dialog.passwordAvailable = passwordAvail;
        }
        function onLoginSuccess(serverUrl) {
            dialog.oidcInProgress = false;
            dialog.isConnecting = false;
        }
        function onLoginError(serverUrl, error) {
            dialog.oidcInProgress = false;
            dialog.isConnecting = false;
        }
    }

    onClosed: {
        dialog.isConnecting = false;
        dialog.oidcInProgress = false;
        dialog.errorMessage = "";
        dialog.checkingFlows = false;
        dialog.oidcAvailable = false;
        dialog.oidcProviderUrl = "";
        dialog.passwordAvailable = true;
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacingLarge

        // Server URL
        ColumnLayout {
            spacing: Theme.spacingSmall
            Layout.fillWidth: true

            Text {
                text: "SERVER URL"
                font.pixelSize: Theme.fontSizeSmall
                font.bold: true
                color: Theme.textSecondary
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingSmall

                TextField {
                    id: urlField
                    Layout.fillWidth: true
                    placeholderText: "http://localhost:8448"
                    placeholderTextColor: Theme.textMuted
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSizeNormal
                    enabled: !dialog.isConnecting && !dialog.oidcInProgress
                    background: Rectangle {
                        color: Theme.bgDarkest
                        radius: Theme.radiusSmall
                        border.color: urlField.activeFocus ? Theme.accent : Theme.bgLight
                        border.width: 1
                    }
                    padding: Theme.spacingNormal

                    onEditingFinished: {
                        if (text.trim() !== "") {
                            dialog.checkingFlows = true;
                            dialog.oidcAvailable = false;
                            dialog.passwordAvailable = true;
                            serverManager.checkLoginFlows(text.trim());
                        }
                    }
                }

                Button {
                    id: checkButton
                    text: "Check"
                    enabled: urlField.text.trim() !== "" && !dialog.isConnecting && !dialog.oidcInProgress && !dialog.checkingFlows
                    contentItem: Text {
                        text: parent.text
                        font.pixelSize: Theme.fontSizeSmall
                        color: parent.enabled ? Theme.textPrimary : Theme.textMuted
                        horizontalAlignment: Text.AlignHCenter
                    }
                    background: Rectangle {
                        color: parent.enabled ? (parent.hovered ? Theme.bgLight : Theme.bgDark) : Theme.bgDarkest
                        radius: Theme.radiusSmall
                    }
                    onClicked: {
                        dialog.checkingFlows = true;
                        dialog.oidcAvailable = false;
                        dialog.passwordAvailable = true;
                        dialog.errorMessage = "";
                        serverManager.checkLoginFlows(urlField.text.trim());
                    }
                }
            }
        }

        // Checking indicator
        Text {
            text: "Checking server capabilities..."
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.textMuted
            visible: dialog.checkingFlows
            Layout.alignment: Qt.AlignHCenter
        }

        // OIDC login button
        Button {
            id: oidcButton
            Layout.fillWidth: true
            visible: dialog.oidcAvailable && !dialog.checkingFlows
            enabled: !dialog.isConnecting && !dialog.oidcInProgress
            contentItem: Text {
                text: "Sign in with BSFChat ID"
                font.pixelSize: Theme.fontSizeNormal
                font.bold: true
                color: parent.enabled ? "white" : Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
            }
            background: Rectangle {
                color: parent.enabled ? (parent.hovered ? Theme.accentHover : Theme.accent) : Theme.bgDarkest
                radius: Theme.radiusSmall
                implicitHeight: 44
            }
            onClicked: {
                dialog.errorMessage = "";
                dialog.oidcInProgress = true;
                serverManager.addServerWithOidc(urlField.text.trim());
            }
        }

        // Separator between OIDC and password
        RowLayout {
            Layout.fillWidth: true
            visible: dialog.oidcAvailable && dialog.passwordAvailable && !dialog.checkingFlows
            spacing: Theme.spacingNormal

            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Theme.bgLight
            }
            Text {
                text: "or"
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textMuted
            }
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Theme.bgLight
            }
        }

        // Username (only when password auth is available or flows haven't been checked yet)
        ColumnLayout {
            spacing: Theme.spacingSmall
            Layout.fillWidth: true
            visible: dialog.passwordAvailable && !dialog.checkingFlows

            Text {
                text: "USERNAME"
                font.pixelSize: Theme.fontSizeSmall
                font.bold: true
                color: Theme.textSecondary
            }

            TextField {
                id: usernameField
                Layout.fillWidth: true
                placeholderText: "Enter username"
                placeholderTextColor: Theme.textMuted
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeNormal
                enabled: !dialog.isConnecting && !dialog.oidcInProgress
                background: Rectangle {
                    color: Theme.bgDarkest
                    radius: Theme.radiusSmall
                    border.color: usernameField.activeFocus ? Theme.accent : Theme.bgLight
                    border.width: 1
                }
                padding: Theme.spacingNormal
            }
        }

        // Password
        ColumnLayout {
            spacing: Theme.spacingSmall
            Layout.fillWidth: true
            visible: dialog.passwordAvailable && !dialog.checkingFlows

            Text {
                text: "PASSWORD"
                font.pixelSize: Theme.fontSizeSmall
                font.bold: true
                color: Theme.textSecondary
            }

            TextField {
                id: passwordField
                Layout.fillWidth: true
                placeholderText: "Enter password"
                placeholderTextColor: Theme.textMuted
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeNormal
                echoMode: TextInput.Password
                enabled: !dialog.isConnecting && !dialog.oidcInProgress
                background: Rectangle {
                    color: Theme.bgDarkest
                    radius: Theme.radiusSmall
                    border.color: passwordField.activeFocus ? Theme.accent : Theme.bgLight
                    border.width: 1
                }
                padding: Theme.spacingNormal

                Keys.onReturnPressed: loginButton.clicked()
            }
        }

        // Error message
        Text {
            text: dialog.errorMessage
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.danger
            visible: dialog.errorMessage !== ""
            Layout.fillWidth: true
            wrapMode: Text.Wrap
        }

        // Connecting indicator
        Text {
            text: dialog.oidcInProgress ? "Waiting for browser login..." : "Connecting..."
            font.pixelSize: Theme.fontSizeNormal
            color: Theme.textMuted
            visible: dialog.isConnecting || dialog.oidcInProgress
            Layout.alignment: Qt.AlignHCenter
        }

        // Password auth buttons
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingNormal
            visible: dialog.passwordAvailable && !dialog.checkingFlows

            Button {
                text: "Register"
                Layout.fillWidth: true
                enabled: !dialog.isConnecting && !dialog.oidcInProgress
                contentItem: Text {
                    text: parent.text
                    font.pixelSize: Theme.fontSizeNormal
                    color: parent.enabled ? Theme.textPrimary : Theme.textMuted
                    horizontalAlignment: Text.AlignHCenter
                }
                background: Rectangle {
                    color: parent.enabled ? (parent.hovered ? Theme.bgLight : Theme.bgDark) : Theme.bgDarkest
                    radius: Theme.radiusSmall
                }
                onClicked: {
                    dialog.errorMessage = "";
                    if (urlField.text.trim() === "" || usernameField.text.trim() === "" || passwordField.text.trim() === "") {
                        dialog.errorMessage = "All fields are required";
                        return;
                    }
                    dialog.isConnecting = true;
                    serverManager.registerServer(urlField.text.trim(), usernameField.text.trim(), passwordField.text.trim());
                }
            }

            Button {
                id: loginButton
                text: "Login"
                Layout.fillWidth: true
                enabled: !dialog.isConnecting && !dialog.oidcInProgress
                contentItem: Text {
                    text: parent.text
                    font.pixelSize: Theme.fontSizeNormal
                    color: parent.enabled ? "white" : Theme.textMuted
                    horizontalAlignment: Text.AlignHCenter
                }
                background: Rectangle {
                    color: parent.enabled ? (parent.hovered ? Theme.accentHover : Theme.accent) : Theme.bgDarkest
                    radius: Theme.radiusSmall
                }
                onClicked: {
                    dialog.errorMessage = "";
                    if (urlField.text.trim() === "" || usernameField.text.trim() === "" || passwordField.text.trim() === "") {
                        dialog.errorMessage = "All fields are required";
                        return;
                    }
                    dialog.isConnecting = true;
                    serverManager.addServer(urlField.text.trim(), usernameField.text.trim(), passwordField.text.trim());
                }
            }
        }

        // Cancel
        Button {
            text: "Cancel"
            Layout.fillWidth: true
            contentItem: Text {
                text: parent.text
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
            }
            background: Rectangle { color: "transparent" }
            onClicked: dialog.close()
        }
    }
}
