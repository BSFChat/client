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
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1

        // Top-right close X — matches ServerSettings / ChannelSettings.
        // Esc / click-outside still work the same way.
        Rectangle {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: Theme.sp.s5
            anchors.rightMargin: Theme.sp.s5
            width: 28; height: 28
            radius: Theme.r1
            color: closeXMouse.containsMouse ? Theme.bg3 : "transparent"
            z: 10
            Icon {
                anchors.centerIn: parent
                name: "x"
                size: 14
                color: closeXMouse.containsMouse ? Theme.fg0 : Theme.fg2
            }
            MouseArea {
                id: closeXMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: clientSettingsPopup.close()
            }
        }
    }

    // SPEC §3.10 SectionHeader: title 24px fg0 + thin divider below.
    component SectionHeader: ColumnLayout {
        property string text: ""
        Layout.fillWidth: true
        spacing: Theme.sp.s3
        Text {
            text: parent.text
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.xxl
            font.weight: Theme.fontWeight.semibold
            font.letterSpacing: Theme.trackTight.xxl
            color: Theme.fg0
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.line }
    }

    // Row with title + description on the left and an arbitrary control on
    // the right. Reused across all settings rows.
    component SettingRow: RowLayout {
        property string title: ""
        property string description: ""
        default property alias rightControl: rightContainer.children
        Layout.fillWidth: true
        spacing: Theme.sp.s7

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Text {
                text: title
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.md
                font.weight: Theme.fontWeight.semibold
                color: Theme.fg0
            }
            Text {
                visible: description.length > 0
                Layout.fillWidth: true
                text: description
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
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
            color: Theme.bg0
            radius: Theme.r2
            Rectangle { // right-edge clip
                anchors.right: parent.right
                width: Theme.r2
                height: parent.height
                color: Theme.bg0
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.sp.s3
                spacing: 2

                Text {
                    text: "CLIENT SETTINGS"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xs
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackWidest.xs
                    color: Theme.fg3
                    Layout.topMargin: Theme.sp.s3
                    Layout.leftMargin: Theme.sp.s3
                    Layout.bottomMargin: Theme.sp.s3
                }

                Repeater {
                    model: ["Appearance", "Audio", "Screen Share", "Notifications"]
                    delegate: Rectangle {
                        Layout.fillWidth: true
                        height: 36
                        radius: Theme.r1
                        readonly property bool isActive:
                            clientSettingsPopup.section === index
                        color: isActive ? Theme.bg3
                             : navItemMouse.containsMouse ? Theme.bg2
                             : "transparent"
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: Theme.sp.s3
                            text: modelData
                            color: parent.isActive ? Theme.fg0 : Theme.fg1
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.md
                            font.weight: parent.isActive
                                         ? Theme.fontWeight.semibold
                                         : Theme.fontWeight.medium
                        }
                        MouseArea {
                            id: navItemMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: clientSettingsPopup.section = index
                        }
                    }
                }

                // Bottom-nav "Close" row removed — replaced by the top-right
                // X on the dialog background (same convention as
                // ServerSettings / ChannelSettings). Esc and click-outside
                // still dismiss.
                Item { Layout.fillHeight: true }
            }
        }

        // Right content
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: clientSettingsPopup.section

            // ---- Appearance ----
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.sp.s7 * 2
                    spacing: Theme.sp.s7

                    SectionHeader { text: "Appearance" }

                    // Theme toggle: two big buttons so the choice reads at
                    // a glance rather than hiding behind a dropdown.
                    SettingRow {
                        title: "Theme"
                        description: "Light mode for daylight desks, dark mode for everything else."
                        RowLayout {
                            spacing: Theme.sp.s1

                            Repeater {
                                model: [
                                    { key: "dark",  label: "Dark"  },
                                    { key: "light", label: "Light" }
                                ]
                                delegate: Rectangle {
                                    required property var modelData
                                    implicitWidth: 96
                                    implicitHeight: 36
                                    radius: Theme.r2
                                    readonly property bool selected: appSettings.theme === modelData.key
                                    color: selected ? Theme.accent
                                           : (themeMouse.containsMouse ? Theme.bg3 : Theme.bg2)
                                    border.color: selected ? Theme.accent : Theme.line
                                    border.width: 1
                                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData.label
                                        color: parent.selected ? Theme.onAccent : Theme.fg1
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.md
                                        font.weight: parent.selected
                                                     ? Theme.fontWeight.semibold
                                                     : Theme.fontWeight.medium
                                    }
                                    MouseArea {
                                        id: themeMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: appSettings.theme = modelData.key
                                    }
                                }
                            }
                        }
                    }

                    // Accent palette. Four Designer-kit hues drive the
                    // palette in tokens.json — 180 (cyan), 260 (violet),
                    // 320 (magenta), 30 (amber). Clicking a swatch writes
                    // the hue int to Settings; Theme.qml binds live.
                    SettingRow {
                        title: "Accent color"
                        description: "Highlights, focus rings, active channel stripes, mic meter."
                        RowLayout {
                            spacing: Theme.sp.s3
                            Repeater {
                                model: [
                                    { hue: 180, label: "Cyan",    color: "#36d6c7" },
                                    { hue: 260, label: "Violet",  color: "#a28bff" },
                                    { hue: 320, label: "Magenta", color: "#ec6dd6" },
                                    { hue:  30, label: "Amber",   color: "#ffa34a" }
                                ]
                                delegate: Rectangle {
                                    required property var modelData
                                    implicitWidth: 32
                                    implicitHeight: 32
                                    radius: Theme.r3
                                    color: modelData.color
                                    readonly property bool selected:
                                        appSettings.accentHue === modelData.hue
                                    border.color: selected ? Theme.fg0 : Theme.line
                                    border.width: selected ? 3 : 1
                                    Behavior on border.width { NumberAnimation { duration: Theme.motion.fastMs } }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: appSettings.accentHue = modelData.hue
                                    }
                                    ToolTip.visible: hoverHandler.hovered
                                    ToolTip.text: modelData.label
                                    ToolTip.delay: 400
                                    HoverHandler { id: hoverHandler }
                                }
                            }
                        }
                    }

                    // Layout density — three preset "shapes" defined in
                    // Theme.layout (standard / compact / focus). The
                    // picker writes the string to appSettings; Theme's
                    // `variant` is bound to it so widths switch live.
                    SettingRow {
                        title: "Layout density"
                        description: "Standard = full desktop layout. Compact shrinks sidebars and participant tiles. Focus hides the member list + chat panel for a voice-first view."
                        RowLayout {
                            spacing: Theme.sp.s1

                            Repeater {
                                model: [
                                    { key: "standard", label: "Standard" },
                                    { key: "compact",  label: "Compact"  },
                                    { key: "focus",    label: "Focus"    }
                                ]
                                delegate: Rectangle {
                                    required property var modelData
                                    implicitWidth: 96
                                    implicitHeight: 36
                                    radius: Theme.r2
                                    readonly property bool selected:
                                        appSettings.layoutVariant === modelData.key
                                    color: selected ? Theme.accent
                                           : (variantMouse.containsMouse ? Theme.bg3 : Theme.bg2)
                                    border.color: selected ? Theme.accent : Theme.line
                                    border.width: 1
                                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData.label
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.md
                                        font.weight: parent.selected
                                                     ? Theme.fontWeight.semibold
                                                     : Theme.fontWeight.medium
                                        color: parent.selected ? Theme.onAccent : Theme.fg1
                                    }
                                    MouseArea {
                                        id: variantMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: appSettings.layoutVariant = modelData.key
                                    }
                                }
                            }
                        }
                    }

                    SettingRow {
                        title: "Accessibility mode"
                        description: "Draws thick, high-contrast borders between the server sidebar, channel list, chat, and member list so panel boundaries are unambiguous."
                        ThemedSwitch {
                            checked: appSettings.accessibilityMode
                            onToggled: appSettings.accessibilityMode = checked
                        }
                    }

                    InfoBanner {
                        icon: "bolt"
                        text: "Theme changes apply instantly across the entire app — no restart required."
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            // ---- Audio ----
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.sp.s7 * 2
                    spacing: Theme.sp.s7

                    SectionHeader { text: "Audio" }

                    SettingRow {
                        title: "Input device"
                        description: "Microphone used in voice channels."
                        ThemedComboBox {
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
                        ThemedSlider {
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
                        ThemedComboBox {
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
                        ThemedSlider {
                            id: outputVolSlider
                            implicitWidth: 260
                            from: 0; to: 100; stepSize: 1
                            value: appSettings.outputVolume
                            onMoved: appSettings.outputVolume = Math.round(value)
                        }
                    }

                    InfoBanner {
                        icon: "signal"
                        tint: Theme.warn
                        text: "Device changes apply the next time you join a voice channel — leave and rejoin to pick up a new selection mid-call. Volume sliders aren't applied yet."
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            // ---- Screen Share (index 2) ----
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.sp.s7 * 2
                    spacing: Theme.sp.s7

                    SectionHeader { text: "Screen Share" }

                    SettingRow {
                        title: "Stream quality"
                        description: {
                            var s = serverManager.activeServer;
                            var serverMax = s ? s.maxScreenShareQuality : 3;
                            var base = "Bandwidth-vs-fidelity tradeoff applied when "
                                     + "you share a screen. Lower presets reduce fps, "
                                     + "resolution, and JPEG quality.";
                            var labels = ["Low", "Medium", "High", "Ultra"];
                            if (serverMax < 3)
                                return base + " This server caps the maximum at "
                                     + labels[serverMax] + ".";
                            return base;
                        }
                        ThemedComboBox {
                            id: ssQualityCombo
                            implicitWidth: 300
                            textRole: "label"
                            // Reactive to server-max changes so items disable
                            // live when an admin tightens the policy.
                            model: {
                                var s = serverManager.activeServer;
                                var max = s ? s.maxScreenShareQuality : 3;
                                var entries = [];
                                var labels = ["Low (2 fps · 960 px · Q40)",
                                              "Medium (5 fps · 1280 px · Q60)",
                                              "High (10 fps · 1600 px · Q75)",
                                              "Ultra (15 fps · 1920 px · Q85)"];
                                for (var i = 0; i <= 3; ++i) {
                                    entries.push({
                                        label: i > max
                                            ? labels[i] + " — server max exceeded"
                                            : labels[i],
                                        value: i,
                                        enabled: i <= max
                                    });
                                }
                                return entries;
                            }
                            Component.onCompleted:
                                currentIndex = appSettings.screenShareQuality
                            onActivated: {
                                var entry = model[currentIndex];
                                if (!entry.enabled) {
                                    var s = serverManager.activeServer;
                                    var cap = s ? s.maxScreenShareQuality : 3;
                                    currentIndex = cap;
                                    appSettings.screenShareQuality = cap;
                                } else {
                                    appSettings.screenShareQuality = entry.value;
                                }
                            }
                        }
                    }

                    InfoBanner {
                        icon: "signal"
                        text: "The chosen preset applies the next time you start a "
                            + "share. Your current stream is unaffected until you "
                            + "stop and restart it."
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            // ---- Notifications ----
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.sp.s7 * 2
                    spacing: Theme.sp.s7

                    SectionHeader { text: "Notifications" }

                    SettingRow {
                        title: "Enable notifications"
                        description: "Show an OS notification when a new message arrives in a channel you're not currently viewing."
                        ThemedSwitch {
                            checked: appSettings.notificationsEnabled
                            onToggled: appSettings.notificationsEnabled = checked
                        }
                    }

                    SettingRow {
                        title: "Play a sound"
                        description: "Play the notification chime when a new message arrives."
                        ThemedSwitch {
                            enabled: appSettings.notificationsEnabled
                            checked: appSettings.notificationSound
                            onToggled: appSettings.notificationSound = checked
                        }
                    }

                    InfoBanner {
                        icon: "bolt"
                        tint: Theme.warn
                        text: "OS-level notification permissions may need to be granted separately in your system preferences."
                    }

                    Item { Layout.fillHeight: true }
                }
            }
        }
    }
}
