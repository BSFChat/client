import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

Dialog {
    id: dialog
    title: "Add Server"
    anchors.centerIn: parent
    // Clamp to viewport so the dialog doesn't overflow on narrow
    // phone screens. Min margin of 16dp on each side; max 400 on
    // large screens preserves the desktop look.
    width: Math.min(400, (parent ? parent.width : 400) - 32)
    // Also bound height so the dialog doesn't grow past the screen
    // when the software keyboard is up.
    height: Math.min(implicitHeight,
        (parent ? parent.height : 800) - 32)
    modal: true
    standardButtons: Dialog.NoButton

    property string errorMessage: ""
    property bool isConnecting: false
    property bool checkingFlows: false
    property bool oidcAvailable: false
    property string oidcProviderUrl: ""
    property bool passwordAvailable: true
    property bool oidcInProgress: false
    // When OIDC is available, password fields collapse behind a toggle
    // so OIDC is the obvious default. Click "Use password instead" to expand.
    property bool showPasswordFallback: false
    // Whether the "add a specific server" block is expanded. Hidden by
    // default when the dialog opens with no saved servers so the big
    // BSFChat-ID button is the obvious path.
    property bool showManualServer: false
    // True while we wait for the identity-first sync flow (browser OIDC +
    // /api/servers fetch + per-server auto-login).
    property bool identitySyncInProgress: false

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1
    }

    header: Rectangle {
        color: "transparent"
        height: 64
        Text {
            anchors.centerIn: parent
            text: "Add a server"
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.xl
            font.weight: Theme.fontWeight.semibold
            font.letterSpacing: Theme.trackTight.xl
            color: Theme.fg0
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
        function onIdentityLoginComplete(serverUrls) {
            dialog.identitySyncInProgress = false;
            // Let the individual per-server logins proceed in the
            // background — close the dialog so the user sees their
            // servers populate the sidebar.
            dialog.close();
        }
        function onIdentityLoginFailed(error) {
            dialog.identitySyncInProgress = false;
            dialog.errorMessage = "Identity login failed: " + error;
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
        dialog.showPasswordFallback = false;
        dialog.showManualServer = false;
        dialog.identitySyncInProgress = false;
    }

    contentItem: ColumnLayout {
        spacing: Theme.sp.s5

        // --- Identity-first sign-in (the fast path).
        Text {
            text: "Sign in with your BSFChat ID to restore every server you've joined."
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.md
            color: Theme.fg1
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
        }

        Button {
            id: identityButton
            Layout.fillWidth: true
            Layout.topMargin: Theme.sp.s3
            enabled: !dialog.identitySyncInProgress && !dialog.isConnecting && !dialog.oidcInProgress
            contentItem: Text {
                text: dialog.identitySyncInProgress ? "Waiting for browser login…"
                                                    : "Sign in with BSFChat ID"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.md
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackTight.md
                color: parent.enabled ? Theme.onAccent : Theme.fg3
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: identityButton.enabled
                       ? (identityButton.hovered ? Theme.accentDim : Theme.accent)
                       : Theme.bg2
                radius: Theme.r2
                implicitHeight: 48
                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
            }
            onClicked: {
                dialog.errorMessage = "";
                dialog.identitySyncInProgress = true;
                serverManager.loginWithIdentityAndSync(identityUrlField.text.trim());
            }
        }

        // Identity URL — editable for self-hosters, default to hosted service.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.sp.s3

            Text {
                text: "Identity server"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWidest.xs
                color: Theme.fg3
            }

            TextField {
                id: identityUrlField
                Layout.fillWidth: true
                text: "https://id.bsfchat.com"
                placeholderText: "https://id.bsfchat.com"
                placeholderTextColor: Theme.fg3
                color: Theme.fg0
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSize.sm
                enabled: !dialog.identitySyncInProgress && !dialog.isConnecting && !dialog.oidcInProgress
                background: Rectangle {
                    color: Theme.bg0
                    radius: Theme.r2
                    border.color: identityUrlField.activeFocus ? Theme.accent : Theme.line
                    border.width: 1
                }
                leftPadding: Theme.sp.s4
                rightPadding: Theme.sp.s4
                topPadding: Theme.sp.s3
                bottomPadding: Theme.sp.s3
            }
        }

        // Divider with embedded "or" label — softer than a full-width line
        // with the toggle text separately below.
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.sp.s3
            spacing: Theme.sp.s3

            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.line }
            Text {
                text: "OR"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWidest.xs
                color: Theme.fg3
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.line }
        }

        // Collapse toggle for the manual "add a specific server" flow.
        Text {
            Layout.fillWidth: true
            text: dialog.showManualServer ? "Hide manual server entry"
                                          : "Add a specific server"
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.sm
            font.weight: Theme.fontWeight.medium
            color: manualToggle.containsMouse ? Theme.accentDim : Theme.accent
            horizontalAlignment: Text.AlignHCenter

            MouseArea {
                id: manualToggle
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: dialog.showManualServer = !dialog.showManualServer
            }
        }

        // Server URL
        ColumnLayout {
            spacing: Theme.sp.s1
            Layout.fillWidth: true
            visible: dialog.showManualServer

            Text {
                text: "SERVER URL"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWidest.xs
                color: Theme.fg3
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp.s1

                TextField {
                    id: urlField
                    Layout.fillWidth: true
                    placeholderText: "http://localhost:8448"
                    placeholderTextColor: Theme.fg2
                    color: Theme.fg0
                    font.pixelSize: Theme.fontSize.md
                    enabled: !dialog.isConnecting && !dialog.oidcInProgress
                    background: Rectangle {
                        color: Theme.bg0
                        radius: Theme.r2
                        border.color: urlField.activeFocus ? Theme.accent : Theme.line
                        border.width: 1
                    }
                    padding: Theme.sp.s3

                    onEditingFinished: {
                        if (text.trim() !== "") {
                            dialog.checkingFlows = true;
                            dialog.oidcAvailable = false;
                            dialog.passwordAvailable = true;
                            serverManager.checkLoginFlows(text.trim());
                        }
                    }
                }

                // Ghost "Check" — probes the server for available login
                // flows (OIDC / password) without committing to a connect.
                Button {
                    id: checkButton
                    enabled: urlField.text.trim() !== "" && !dialog.isConnecting && !dialog.oidcInProgress && !dialog.checkingFlows
                    contentItem: Text {
                        text: "Check"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                        font.weight: Theme.fontWeight.medium
                        color: checkButton.enabled ? Theme.fg1 : Theme.fg3
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: checkButton.hovered && checkButton.enabled ? Theme.bg3 : "transparent"
                        border.color: Theme.line
                        border.width: 1
                        radius: Theme.r2
                        implicitWidth: 80
                        implicitHeight: 36
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
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
            font.pixelSize: Theme.fontSize.sm
            color: Theme.fg2
            visible: dialog.checkingFlows && dialog.showManualServer
            Layout.alignment: Qt.AlignHCenter
        }

        // OIDC login button
        Button {
            id: oidcButton
            Layout.fillWidth: true
            visible: dialog.showManualServer && dialog.oidcAvailable && !dialog.checkingFlows
            enabled: !dialog.isConnecting && !dialog.oidcInProgress
            contentItem: Text {
                text: "Sign in with BSFChat ID"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.md
                font.weight: Theme.fontWeight.semibold
                color: oidcButton.enabled ? Theme.onAccent : Theme.fg3
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: !oidcButton.enabled ? Theme.bg2
                     : (oidcButton.hovered ? Theme.accentDim : Theme.accent)
                radius: Theme.r2
                implicitHeight: 44
                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
            }
            onClicked: {
                dialog.errorMessage = "";
                dialog.oidcInProgress = true;
                serverManager.addServerWithOidc(urlField.text.trim());
            }
        }

        // When OIDC is available, collapse password behind a link.
        // When OIDC is NOT available, show password fields directly.
        Text {
            Layout.fillWidth: true
            Layout.topMargin: -Theme.sp.s1
            text: dialog.showPasswordFallback ? "Hide password login" : "Use password instead"
            font.pixelSize: Theme.fontSize.sm
            color: Theme.accent
            horizontalAlignment: Text.AlignHCenter
            visible: dialog.showManualServer && dialog.oidcAvailable && dialog.passwordAvailable && !dialog.checkingFlows

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: dialog.showPasswordFallback = !dialog.showPasswordFallback
            }
        }

        // Username (only when password auth is relevant and not collapsed)
        ColumnLayout {
            spacing: Theme.sp.s1
            Layout.fillWidth: true
            visible: dialog.showManualServer && dialog.passwordAvailable && !dialog.checkingFlows
                     && (!dialog.oidcAvailable || dialog.showPasswordFallback)

            Text {
                text: "USERNAME"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWidest.xs
                color: Theme.fg3
            }

            TextField {
                id: usernameField
                Layout.fillWidth: true
                placeholderText: "Enter username"
                placeholderTextColor: Theme.fg2
                color: Theme.fg0
                font.pixelSize: Theme.fontSize.md
                enabled: !dialog.isConnecting && !dialog.oidcInProgress
                background: Rectangle {
                    color: Theme.bg0
                    radius: Theme.r2
                    border.color: usernameField.activeFocus ? Theme.accent : Theme.line
                    border.width: 1
                }
                padding: Theme.sp.s3
            }
        }

        // Password
        ColumnLayout {
            spacing: Theme.sp.s1
            Layout.fillWidth: true
            visible: dialog.showManualServer && dialog.passwordAvailable && !dialog.checkingFlows
                     && (!dialog.oidcAvailable || dialog.showPasswordFallback)

            Text {
                text: "PASSWORD"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWidest.xs
                color: Theme.fg3
            }

            TextField {
                id: passwordField
                Layout.fillWidth: true
                placeholderText: "Enter password"
                placeholderTextColor: Theme.fg2
                color: Theme.fg0
                font.pixelSize: Theme.fontSize.md
                echoMode: TextInput.Password
                enabled: !dialog.isConnecting && !dialog.oidcInProgress
                background: Rectangle {
                    color: Theme.bg0
                    radius: Theme.r2
                    border.color: passwordField.activeFocus ? Theme.accent : Theme.line
                    border.width: 1
                }
                padding: Theme.sp.s3

                Keys.onReturnPressed: loginButton.clicked()
            }
        }

        // Error message
        Text {
            text: dialog.errorMessage
            font.pixelSize: Theme.fontSize.sm
            color: Theme.danger
            visible: dialog.errorMessage !== ""
            Layout.fillWidth: true
            wrapMode: Text.Wrap
        }

        // Connecting indicator
        Text {
            text: dialog.oidcInProgress ? "Waiting for browser login..." : "Connecting..."
            font.pixelSize: Theme.fontSize.md
            color: Theme.fg2
            visible: dialog.isConnecting || dialog.oidcInProgress
            Layout.alignment: Qt.AlignHCenter
        }

        // Password auth buttons
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.sp.s3
            visible: dialog.showManualServer && dialog.passwordAvailable && !dialog.checkingFlows
                     && (!dialog.oidcAvailable || dialog.showPasswordFallback)

            // Ghost Register — secondary action, soft border, fg1.
            Button {
                id: registerBtn
                Layout.fillWidth: true
                enabled: !dialog.isConnecting && !dialog.oidcInProgress
                contentItem: Text {
                    text: "Register"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    font.weight: Theme.fontWeight.medium
                    color: registerBtn.enabled ? Theme.fg1 : Theme.fg3
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: registerBtn.hovered && registerBtn.enabled ? Theme.bg3 : "transparent"
                    border.color: Theme.line
                    border.width: 1
                    radius: Theme.r2
                    implicitHeight: 40
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
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

            // Primary Login — accent filled.
            Button {
                id: loginButton
                Layout.fillWidth: true
                enabled: !dialog.isConnecting && !dialog.oidcInProgress
                contentItem: Text {
                    text: "Login"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    font.weight: Theme.fontWeight.semibold
                    color: loginButton.enabled ? Theme.onAccent : Theme.fg3
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: !loginButton.enabled ? Theme.bg2
                         : (loginButton.hovered ? Theme.accentDim : Theme.accent)
                    radius: Theme.r2
                    implicitHeight: 40
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
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

        // Cancel — ghost text link, no bg at all.
        Button {
            id: cancelBtn
            Layout.fillWidth: true
            contentItem: Text {
                text: "Cancel"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: cancelBtn.hovered ? Theme.fg1 : Theme.fg3
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle { color: "transparent"; implicitHeight: 32 }
            onClicked: dialog.close()
        }
    }
}
