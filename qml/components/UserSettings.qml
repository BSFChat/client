import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import BSFChat

Popup {
    id: userSettings

    width: 380
    // Let height derive from the content so nothing gets clipped. The
    // contentItem is a ColumnLayout that reports its implicitHeight — add
    // padding + a comfortable margin.
    height: Math.min(
        contentCol.implicitHeight + Theme.sp.s7 * 2 + padding * 2,
        parent ? parent.height * 0.9 : 600
    )
    anchors.centerIn: Overlay.overlay
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: Theme.sp.s7

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0.0; to: 1.0; duration: 150; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: 0.95; to: 1.0; duration: 150; easing.type: Easing.OutCubic }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1.0; to: 0.0; duration: 100; easing.type: Easing.InCubic }
        NumberAnimation { property: "scale"; from: 1.0; to: 0.95; duration: 100; easing.type: Easing.InCubic }
    }

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1
    }

    onAboutToShow: {
        if (serverManager.activeServer) {
            displayNameField.text = serverManager.activeServer.displayName;
        }
    }

    contentItem: ColumnLayout {
        id: contentCol
        spacing: Theme.sp.s7

        // Title — SPEC §3.10 section-header convention (24px semibold +
        // a 1px divider rule below).
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.sp.s3
            Text {
                text: "User Settings"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xxl
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackTight.xxl
                color: Theme.fg0
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.line }
        }

        // Avatar section — 64×64 rounded-square (matches ServerRail /
        // MemberList treatment instead of a circle), with a camera-icon
        // hover overlay rather than a "Change" text label.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.sp.s7

            Rectangle {
                width: 64
                height: 64
                radius: Theme.r3
                color: Theme.senderColor(serverManager.activeServer ? serverManager.activeServer.userId : "")
                clip: true

                Image {
                    anchors.fill: parent
                    source: {
                        if (serverManager.activeServer && serverManager.activeServer.avatarUrl !== "") {
                            return serverManager.activeServer.resolveMediaUrl(serverManager.activeServer.avatarUrl);
                        }
                        return "";
                    }
                    visible: source !== ""
                    fillMode: Image.PreserveAspectCrop
                    layer.enabled: true
                }

                Text {
                    anchors.centerIn: parent
                    text: {
                        if (!serverManager.activeServer) return "?";
                        var n = serverManager.activeServer.displayName
                             || serverManager.activeServer.userId;
                        var s = n.replace(/^[^a-zA-Z0-9]+/, "");
                        return (s.length > 0 ? s.charAt(0) : "?").toUpperCase();
                    }
                    font.family: Theme.fontSans
                    font.pixelSize: 26
                    font.weight: Theme.fontWeight.semibold
                    color: Theme.onAccent
                    visible: !serverManager.activeServer || serverManager.activeServer.avatarUrl === ""
                }

                // Hover overlay — black scrim + small edit icon instead
                // of the old "Change" text label. Icon reads at any
                // avatar content, text would collide with tall initials.
                Rectangle {
                    anchors.fill: parent
                    radius: parent.radius
                    color: Qt.rgba(0, 0, 0, 0.55)
                    opacity: avatarMouse.containsMouse ? 1.0 : 0.0
                    Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }

                    Icon {
                        anchors.centerIn: parent
                        name: "edit"
                        size: 20
                        color: Theme.fg0
                    }

                    MouseArea {
                        id: avatarMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: avatarFileDialog.open()
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Text {
                    text: "Profile picture"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    font.weight: Theme.fontWeight.semibold
                    color: Theme.fg0
                }
                Text {
                    text: "Hover the avatar to change. PNG, JPG, GIF up to 10 MB."
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    color: Theme.fg2
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }

        // Display name field
        Column {
            Layout.fillWidth: true
            spacing: Theme.sp.s1

            Text {
                text: "DISPLAY NAME"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWidest.xs
                color: Theme.fg3
            }

            RowLayout {
                width: parent.width
                spacing: Theme.sp.s3

                TextField {
                    id: displayNameField
                    Layout.fillWidth: true
                    placeholderText: "Enter display name"
                    placeholderTextColor: Theme.fg3
                    color: Theme.fg0
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    background: Rectangle {
                        color: Theme.bg0
                        radius: Theme.r2
                        border.color: displayNameField.activeFocus ? Theme.accent : Theme.line
                        border.width: 1
                    }
                    leftPadding: Theme.sp.s4
                    rightPadding: Theme.sp.s4
                    topPadding: Theme.sp.s3
                    bottomPadding: Theme.sp.s3

                    Keys.onReturnPressed: saveDisplayName()
                }

                Button {
                    id: displayNameSaveBtn
                    enabled: serverManager.activeServer
                             && displayNameField.text.trim().length > 0
                             && displayNameField.text.trim() !== serverManager.activeServer.displayName
                    contentItem: Text {
                        text: "Save"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.semibold
                        color: displayNameSaveBtn.enabled ? Theme.onAccent : Theme.fg3
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: !displayNameSaveBtn.enabled
                               ? Theme.bg2
                               : (displayNameSaveBtn.hovered ? Theme.accentDim : Theme.accent)
                        radius: Theme.r2
                        implicitWidth: 80
                        implicitHeight: 36
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                    onClicked: saveDisplayName()
                }
            }
        }

        // User ID (read-only)
        Column {
            Layout.fillWidth: true
            spacing: Theme.sp.s1

            Text {
                text: "USER ID"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWidest.xs
                color: Theme.fg3
            }

            // mxid reads as code — mono + fg3 matches UserProfileCard.
            Text {
                text: serverManager.activeServer ? serverManager.activeServer.userId : ""
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg3
                elide: Text.ElideRight
                width: parent.width
            }
        }

        Item { Layout.fillHeight: true }

        // Log Out — ghost danger pattern (matches role-delete button in
        // ServerSettings). Transparent on rest, fills danger on hover.
        Button {
            id: logoutBtn
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            contentItem: Text {
                text: "Log out"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.md
                font.weight: Theme.fontWeight.semibold
                color: logoutBtn.hovered ? Theme.onAccent : Theme.danger
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: logoutBtn.hovered ? Theme.danger : "transparent"
                radius: Theme.r2
                border.color: Theme.danger
                border.width: 1
                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
            }
            onClicked: {
                if (serverManager.activeServer) {
                    serverManager.activeServer.disconnectFromServer();
                }
                userSettings.close();
            }
        }
    }

    FileDialog {
        id: avatarFileDialog
        title: "Choose Avatar"
        nameFilters: ["Image files (*.png *.jpg *.jpeg *.gif *.webp)"]
        onAccepted: {
            if (!serverManager.activeServer) return;
            // Read the file and upload it
            var filePath = selectedFile;
            uploadAvatar(filePath);
        }
    }

    function saveDisplayName() {
        var name = displayNameField.text.trim();
        if (name.length === 0 || !serverManager.activeServer) return;
        serverManager.activeServer.updateDisplayName(name);
    }

    function uploadAvatar(fileUrl) {
        if (!serverManager.activeServer) return;
        serverManager.activeServer.uploadAvatar(fileUrl.toString());
    }
}
