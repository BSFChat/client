import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// Client-wide settings. Distinct from UserSettings.qml (per-server profile)
// and ServerSettings.qml (per-server admin). Sectioned like Discord:
// Audio (wired), Notifications (placeholder — persists but not yet routed
// through the OS notification system).
Popup {
    id: clientSettingsPopup
    anchors.centerIn: Overlay.overlay
    width: Math.min(parent ? parent.width * 0.85 : 720, 760)
    height: Math.min(parent ? parent.height * 0.85 : 600, 640)
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    property int section: 0

    background: Rectangle {
        color: Theme.bgDark
        radius: Theme.radiusNormal
        border.color: Theme.bgLight
        border.width: 1
    }

    component SectionHeader: Text {
        font.pixelSize: 18
        font.bold: true
        color: Theme.textPrimary
    }

    // Row with title + description on the left and an arbitrary control on
    // the right. Reused across all settings rows.
    component SettingRow: RowLayout {
        property string title: ""
        property string description: ""
        default property alias rightControl: rightContainer.children
        Layout.fillWidth: true
        spacing: Theme.spacingLarge

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Text {
                text: title
                font.pixelSize: Theme.fontSizeNormal
                font.bold: true
                color: Theme.textPrimary
            }
            Text {
                visible: description.length > 0
                Layout.fillWidth: true
                text: description
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textMuted
                wrapMode: Text.WordWrap
            }
        }
        Item {
            id: rightContainer
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: childrenRect.width
            implicitHeight: childrenRect.height
        }
    }

    contentItem: RowLayout {
        spacing: 0

        // Left nav
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 180
            color: Theme.bgDarkest
            radius: Theme.radiusNormal
            Rectangle { // right-edge clip
                anchors.right: parent.right
                width: Theme.radiusNormal
                height: parent.height
                color: Theme.bgDarkest
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.spacingNormal
                spacing: 2

                Text {
                    text: "CLIENT SETTINGS"
                    font.pixelSize: Theme.fontSizeSmall
                    font.bold: true
                    color: Theme.textMuted
                    Layout.topMargin: Theme.spacingNormal
                    Layout.leftMargin: Theme.spacingNormal
                    Layout.bottomMargin: Theme.spacingNormal
                }

                Repeater {
                    model: ["Audio", "Notifications"]
                    delegate: Rectangle {
                        Layout.fillWidth: true
                        height: 36
                        radius: Theme.radiusSmall
                        color: clientSettingsPopup.section === index
                            ? Theme.bgLight
                            : (mouse.containsMouse ? Qt.darker(Theme.bgMedium, 1.05) : "transparent")

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: Theme.spacingNormal
                            text: modelData
                            color: clientSettingsPopup.section === index
                                ? Theme.textPrimary : Theme.textSecondary
                            font.pixelSize: Theme.fontSizeNormal
                        }
                        MouseArea {
                            id: mouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: clientSettingsPopup.section = index
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                Rectangle {
                    Layout.fillWidth: true
                    height: 36
                    radius: Theme.radiusSmall
                    color: closeMouse.containsMouse ? Theme.bgLight : "transparent"
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.spacingNormal
                        text: "Close"
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSizeNormal
                    }
                    MouseArea {
                        id: closeMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: clientSettingsPopup.close()
                    }
                }
            }
        }

        // Right content
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: clientSettingsPopup.section

            // ---- Audio ----
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingLarge * 2
                    spacing: Theme.spacingLarge

                    SectionHeader { text: "Audio" }

                    SettingRow {
                        title: "Input device"
                        description: "Microphone used in voice channels."
                        ComboBox {
                            id: inputCombo
                            implicitWidth: 260
                            model: appSettings.audioInputDevices
                            textRole: "description"
                            Component.onCompleted: selectByDescription(appSettings.audioInputDevice)
                            onActivated: {
                                var item = model[currentIndex];
                                appSettings.audioInputDevice = item.description === "System default" ? "" : item.description;
                            }
                            function selectByDescription(desc) {
                                for (var i = 0; i < model.length; i++) {
                                    if ((desc === "" && model[i].description === "System default")
                                        || model[i].description === desc) {
                                        currentIndex = i;
                                        return;
                                    }
                                }
                                currentIndex = 0;
                            }
                        }
                    }

                    SettingRow {
                        title: "Input volume"
                        description: "Gain applied to your microphone before encoding."
                        Slider {
                            id: inputVolSlider
                            implicitWidth: 260
                            from: 0; to: 100; stepSize: 1
                            value: appSettings.inputVolume
                            onMoved: appSettings.inputVolume = Math.round(value)
                        }
                    }

                    SettingRow {
                        title: "Output device"
                        description: "Speakers / headphones used for voice + notification sounds."
                        ComboBox {
                            id: outputCombo
                            implicitWidth: 260
                            model: appSettings.audioOutputDevices
                            textRole: "description"
                            Component.onCompleted: selectByDescription(appSettings.audioOutputDevice)
                            onActivated: {
                                var item = model[currentIndex];
                                appSettings.audioOutputDevice = item.description === "System default" ? "" : item.description;
                            }
                            function selectByDescription(desc) {
                                for (var i = 0; i < model.length; i++) {
                                    if ((desc === "" && model[i].description === "System default")
                                        || model[i].description === desc) {
                                        currentIndex = i;
                                        return;
                                    }
                                }
                                currentIndex = 0;
                            }
                        }
                    }

                    SettingRow {
                        title: "Output volume"
                        description: "Applied on top of your OS volume."
                        Slider {
                            id: outputVolSlider
                            implicitWidth: 260
                            from: 0; to: 100; stepSize: 1
                            value: appSettings.outputVolume
                            onMoved: appSettings.outputVolume = Math.round(value)
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        color: Qt.rgba(1,1,1,0.04)
                        radius: Theme.radiusSmall
                        Layout.preferredHeight: heads.implicitHeight + Theme.spacingNormal * 2
                        ColumnLayout {
                            id: heads
                            anchors.fill: parent
                            anchors.margins: Theme.spacingNormal
                            Text {
                                text: "Device changes apply the next time you join a voice channel. Leave and rejoin to pick up a new selection mid-call. Volume sliders aren't applied yet."
                                Layout.fillWidth: true
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            // ---- Notifications ----
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingLarge * 2
                    spacing: Theme.spacingLarge

                    SectionHeader { text: "Notifications" }

                    SettingRow {
                        title: "Enable notifications"
                        description: "Not yet implemented — setting persists for when it is."
                        Switch {
                            checked: appSettings.notificationsEnabled
                            onToggled: appSettings.notificationsEnabled = checked
                        }
                    }

                    SettingRow {
                        title: "Play a sound"
                        description: "Play the notification chime when a new message arrives."
                        Switch {
                            enabled: appSettings.notificationsEnabled
                            checked: appSettings.notificationSound
                            onToggled: appSettings.notificationSound = checked
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }
        }
    }
}
