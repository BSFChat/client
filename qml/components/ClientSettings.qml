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
    // 900: the Screen Share rows carry a slider + mono value + server-cap
    // badge (~400 px) NEXT TO the title/description column — at the old
    // 760 cap the controls clipped off the dialog's right edge on every
    // row. 0.9 keeps it inside small windows.
    width: Math.min(parent ? parent.width * 0.9 : 720, 900)
    height: Math.min(parent ? parent.height * 0.85 : 600, 640)
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    property int section: 0

    // D-C1's rule, applied here. This popup is created once with the window and
    // reused, so anything a control wrote over its own binding — or resolved
    // once in Component.onCompleted — stays wrong for the rest of the session
    // unless it is re-established when the dialog is shown. onAboutToShow runs
    // before the dialog is visible, so nothing paints the stale value first.
    onAboutToShow: {
        // Re-enumerate audio devices. This used to be a CONSTANT property, so a
        // headset plugged in after launch never appeared in either box, however
        // many times the dialog was reopened. The enumeration is cheap and the
        // moment the dialog opens is exactly when a user who just plugged
        // something in comes looking for it (D-L).
        appSettings.refreshAudioDevices();
        inputCombo.selectByDescription(appSettings.audioInputDevice);
        outputCombo.selectByDescription(appSettings.audioOutputDevice);
        voiceModeCombo.syncFromSettings();
        videoCodecCombo.syncFromSettings();
        pttKeyField.text = Qt.binding(function() {
            return appSettings.pttKeySequence;
        });
    }

    onOpened: {
        // D-L: the dialog opened with nothing focused, so the first keypress
        // went nowhere.
        contentItem.forceActiveFocus();
    }

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
                // fillWidth + wrap: a long title must wrap inside its
                // column, never widen the row past the viewport (rows
                // with wide right-hand controls leave the title column
                // narrow).
                Layout.fillWidth: true
                text: title
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.md
                font.weight: Theme.fontWeight.semibold
                color: Theme.fg0
                wrapMode: Text.WordWrap
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
                    model: ["Appearance", "Audio", "Screen Share",
                            "Notifications", "Updates", "Advanced"]
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
                                    implicitHeight: Theme.controlHeight.md
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
                                // From Theme, not a second copy of the four
                                // hex values: written out here they drifted
                                // from the light-mode palette, so every swatch
                                // showed its DARK colour in light mode.
                                model: Theme.accentHues
                                delegate: Rectangle {
                                    required property var modelData
                                    implicitWidth: 32
                                    implicitHeight: Theme.controlHeight.sm
                                    radius: Theme.r3
                                    color: Theme.accentFor(modelData.hue)
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
                                    implicitHeight: Theme.controlHeight.md
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
                            // A user toggle writes `checked` itself, which
                            // replaces the binding above — so put it back, or
                            // this switch stops tracking the setting the moment
                            // it is first touched (D-C1's rule, applied to
                            // every control in this dialog that can be written
                            // imperatively).
                            onToggled: {
                                appSettings.accessibilityMode = checked;
                                checked = Qt.binding(function() {
                                    return appSettings.accessibilityMode;
                                });
                            }
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
                // D-H4: the Audio page is taller than the dialog's 640 px
                // cap — two device rows, the protection rows and the whole
                // Voice Activity block. With a bare ColumnLayout the overflow
                // simply painted past the dialog's edge: no clip, no scroll,
                // no way to reach it. Same Flickable wrapper as Screen Share.
                Flickable {
                    id: audioColFlick
                    anchors.fill: parent
                    anchors.margins: Theme.sp.s7 * 2
                    contentHeight: audioCol.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ThemedScrollBar {}

                ColumnLayout {
                    id: audioCol
                    // Bind to the Flickable, NOT parent (the contentItem):
                    // with contentWidth unset the contentItem's width follows
                    // its children, so parent.width is circular.
                    width: audioColFlick.width
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
                            // The device list is re-enumerated on every dialog
                            // open (and was CONSTANT before, so it never
                            // changed at all). When it does change, currentIndex
                            // points into the OLD list and would silently select
                            // a different device, so re-resolve by name.
                            onModelChanged: selectByDescription(appSettings.audioInputDevice)
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

                    // HIDDEN, DELIBERATELY. `inputVolume` is persisted and has
                    // no consumer anywhere in the app: no gain is applied to the
                    // captured audio, so dragging this changed a number in
                    // QSettings and nothing else. A control that does nothing is
                    // worse than an absent one — the user turns it down, is
                    // still too loud, and now distrusts the rest of the page.
                    //
                    // The row is kept (not deleted) because the setting itself
                    // is sound and the UI is the finished half: applying it is a
                    // gain multiplier in the audio worker (src/voice/), which
                    // this workstream does not own. The value is already exposed
                    // as Settings::inputVolume with a NOTIFY signal, so wiring it
                    // up is a read on that side and flipping `visible` here.
                    SettingRow {
                        visible: false
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
                            // The device list is re-enumerated on every dialog
                            // open (and was CONSTANT before, so it never
                            // changed at all). When it does change, currentIndex
                            // points into the OLD list and would silently select
                            // a different device, so re-resolve by name.
                            onModelChanged: selectByDescription(appSettings.audioOutputDevice)
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

                    // Hidden for the same reason as "Input volume" above:
                    // Settings::outputVolume has no consumer, so this slider
                    // moved a stored number and no audio.
                    SettingRow {
                        visible: false
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
                        text: "Device changes apply the next time you join a voice channel — leave and rejoin to pick up a new selection mid-call."
                    }

                    SectionHeader {
                        text: "Voice Activity"
                        Layout.topMargin: Theme.sp.s5
                    }

                    SettingRow {
                        title: "Input mode"
                        description: "Open mic sends whenever you speak. Push-to-talk only transmits while you hold the shortcut key."
                        ThemedComboBox {
                            id: voiceModeCombo
                            implicitWidth: 220
                            textRole: "label"
                            model: [
                                { label: "Open mic",     value: "open" },
                                { label: "Push to talk", value: "ptt"  }
                            ]
                            function syncFromSettings() {
                                currentIndex = appSettings.voiceMode === "ptt" ? 1 : 0;
                            }
                            // Component.onCompleted runs once, ever: this popup
                            // is created with the window and reused, so without
                            // the re-sync on show (see onAboutToShow) a value
                            // changed anywhere else never reached the box.
                            Component.onCompleted: syncFromSettings()
                            onActivated: appSettings.voiceMode = model[currentIndex].value
                        }
                    }

                    SettingRow {
                        title: "PTT key"
                        description: "Hold this shortcut (application-wide) to transmit while push-to-talk is enabled."
                        visible: appSettings.voiceMode === "ptt"
                        TextField {
                            id: pttKeyField
                            implicitWidth: 220
                            text: appSettings.pttKeySequence
                            color: Theme.fg0
                            placeholderText: "Ctrl+Space"
                            background: Rectangle {
                                color: Theme.bg0
                                radius: Theme.r2
                                border.color: parent.activeFocus ? Theme.accent : Theme.line
                                border.width: 1
                            }
                            leftPadding: Theme.sp.s3
                            rightPadding: Theme.sp.s3
                            topPadding: Theme.sp.s2
                            bottomPadding: Theme.sp.s2
                            onEditingFinished: appSettings.pttKeySequence = text
                        }
                    }

                    // SPEC §3.10: the ONLY honest security surface in Settings.
                    // There is no "Security & Keys" pane and there must not be —
                    // this client has no device keys, no cross-signing and no
                    // key backup, so such a pane could only invent content, and
                    // invented content in a security pane is read as a promise.
                    // The one thing that can be shown truthfully is the state of
                    // the call currently running.
                    //
                    // The text below is ServerConnection.voiceProtectionDetail
                    // VERBATIM — not summarised, not shortened to fit, not
                    // rephrased. It already states the limitation alongside the
                    // guarantee, which is the whole reason it reads the way it
                    // does. Every word this app says about voice security lives
                    // in src/voice/VoiceEncryption.cpp and reaches the UI through
                    // that single Q_PROPERTY; read that header before touching
                    // anything here, and note that test_voice_encryption
                    // (qmlHoldsNoHardCodedSecurityLabel) scans every .qml file in
                    // the tree and fails if such wording reappears as a literal.
                    SettingRow {
                        title: "Call protection"
                        // Both protection properties return an empty string when
                        // no transport is live — an idle client has no call to
                        // describe, and describing one it has not made would be
                        // the exact failure this surface exists to avoid. So the
                        // resting text states only that there is nothing to
                        // describe yet. It deliberately makes no claim about what
                        // a future call would get: that varies by transport and
                        // by server, and asserting it here would be QML making a
                        // promise on the C++ layer's behalf.
                        readonly property string live: serverManager.activeServer
                            ? serverManager.activeServer.voiceProtectionDetail : ""
                        description: live.length > 0
                            ? live
                            : "You're not in a voice call. Join one and this row describes that call."
                    }

                    SectionHeader {
                        text: "Privacy"
                        Layout.topMargin: Theme.sp.s5
                    }

                    // The one honest sentence about the trade-off, and it is a
                    // trade-off in both directions — which is why the
                    // description states the cost rather than only the benefit.
                    //
                    // The CONTROL's wording is allowed to live here because it
                    // describes what the setting does, which is true by
                    // construction. What may never live here is a claim about
                    // the call actually running — whether an address is in fact
                    // hidden depends on the selected ICE candidate pair, which
                    // QML cannot see. That claim is the row below it, read
                    // verbatim from ServerConnection.voiceIpPrivacyDetail.
                    // test_voice_encryption scans every .qml file for both
                    // kinds of overclaim. See src/voice/IpPrivacy.h.
                    SettingRow {
                        title: "Hide my IP address"
                        description: "Routes your calls through the server instead of "
                                   + "connecting straight to the other people in them, so "
                                   + "they see the server's address rather than yours. "
                                   + "Costs some delay, uses the server's bandwidth, and "
                                   + "can lower video quality when the server is the "
                                   + "slowest link. Their addresses stay their own choice. "
                                   + "If the server has no relay, joining a call with this "
                                   + "on will fail rather than connect directly."
                        ThemedSwitch {
                            checked: appSettings.voiceRelayMode === "relayOnly"
                            // D-C1: a user toggle writes `checked` imperatively
                            // and destroys the declarative binding, so it has to
                            // be restored or the row stops tracking the setting.
                            onToggled: {
                                appSettings.voiceRelayMode = checked ? "relayOnly" : "auto";
                                checked = Qt.binding(function() {
                                    return appSettings.voiceRelayMode === "relayOnly";
                                });
                            }
                        }
                    }

                    SettingRow {
                        title: "Current call"
                        // Verbatim from the property, same rule as "Call
                        // protection" above: this one answers whether the
                        // address is hidden RIGHT NOW, which depends on the
                        // candidate pair ICE actually selected and not on what
                        // the switch above asks for.
                        readonly property string liveIp: serverManager.activeServer
                            ? serverManager.activeServer.voiceIpPrivacyDetail : ""
                        description: liveIp.length > 0
                            ? liveIp
                            : "You're not in a voice call. Join one and this row describes how it is routed."
                    }

                    // No trailing fillHeight spacer: inside a Flickable the
                    // column is sized by its content.
                }
                }
            }

            // ---- Screen Share (index 2) ----
            // The one page with more rows than the dialog's max height
            // (640 px) can hold — it scrolls. Plain anchored
            // ColumnLayouts on the other pages paint past the dialog
            // bounds when they overflow (no clip), which is exactly
            // what happened when the quality settings landed here.
            Item {
                Flickable {
                    id: screenShareFlick
                    anchors.fill: parent
                    anchors.margins: Theme.sp.s7 * 2
                    contentHeight: screenShareCol.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ThemedScrollBar {}

                    ColumnLayout {
                    id: screenShareCol
                    // Bind to the Flickable itself, NOT parent (the
                    // contentItem) — contentItem width follows its
                    // children when contentWidth is unset, so
                    // `parent.width` is circular and the column blows
                    // out to its implicit width, clipping the row
                    // trailers off the right edge.
                    width: screenShareFlick.width
                    spacing: Theme.sp.s7

                    SectionHeader { text: "Screen Share" }

                    // Helper used inline below to render the active
                    // server's per-axis cap as a small badge after
                    // the slider, e.g. " · capped @ 30". When the
                    // server hasn't set a cap (-1 sentinel) the
                    // badge reads " · uncapped".
                    component CapBadge: Text {
                        property int cap: -1
                        property string suffix: ""
                        // Compact on purpose — it sits at the end of
                        // rows that already carry a slider + value and
                        // was the first casualty of tight width.
                        text: cap < 0 ? " · uncapped"
                                      : " · cap " + cap + suffix
                        color: cap < 0 ? Theme.fg3 : Theme.warn
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                    }

                    SettingRow {
                        title: "Frame rate"
                        description: "Frames per second sent to peers. "
                                   + "Higher = smoother; bandwidth scales "
                                   + "roughly linearly. 1 fps is fine for "
                                   + "static content, 60 only makes sense "
                                   + "on a fast LAN."
                        RowLayout {
                            spacing: Theme.sp.s3
                            ThemedSlider {
                                id: fpsSlider
                                implicitWidth: 200
                                from: 1; to: 60; stepSize: 1
                                value: appSettings.screenShareFps
                                onMoved: appSettings.screenShareFps = Math.round(value)
                            }
                            Text {
                                Layout.preferredWidth: 56
                                text: appSettings.screenShareFps + " fps"
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.fontSize.sm
                                color: Theme.fg0
                                horizontalAlignment: Text.AlignRight
                            }
                            CapBadge {
                                cap: serverManager.activeServer
                                    ? serverManager.activeServer.maxScreenShareFps : -1
                                suffix: " fps"
                            }
                        }
                    }

                    SettingRow {
                        title: "Maximum resolution"
                        description: "The long edge of the captured frame "
                                   + "before encode. 1280 px ≈ 720p, 1920 ≈ 1080p, "
                                   + "2560 ≈ 1440p, 3840 ≈ 4K. Aspect ratio is "
                                   + "preserved."
                        RowLayout {
                            spacing: Theme.sp.s3
                            ThemedSlider {
                                id: widthSlider
                                implicitWidth: 200
                                from: 480; to: 3840; stepSize: 80
                                value: appSettings.screenShareMaxWidth
                                onMoved: appSettings.screenShareMaxWidth = Math.round(value)
                            }
                            Text {
                                Layout.preferredWidth: 64
                                text: appSettings.screenShareMaxWidth + " px"
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.fontSize.sm
                                color: Theme.fg0
                                horizontalAlignment: Text.AlignRight
                            }
                            CapBadge {
                                cap: serverManager.activeServer
                                    ? serverManager.activeServer.maxScreenShareWidth : -1
                                suffix: " px"
                            }
                        }
                    }

                    SettingRow {
                        title: "Target bitrate"
                        description: "The steady-state budget for the H.264 "
                                   + "stream. The adaptive controller climbs "
                                   + "toward it on a clean link and backs off "
                                   + "under loss — smoothness always wins over "
                                   + "quality. 4 Mbps suits 1080p desktops; go "
                                   + "big on a LAN."
                        RowLayout {
                            spacing: Theme.sp.s3
                            ThemedSlider {
                                id: bitrateSlider
                                implicitWidth: 200
                                from: 250; to: 50000; stepSize: 250
                                value: appSettings.screenShareTargetKbps
                                onMoved: appSettings.screenShareTargetKbps = Math.round(value)
                            }
                            Text {
                                Layout.preferredWidth: 80
                                text: appSettings.screenShareTargetKbps >= 1000
                                    ? (appSettings.screenShareTargetKbps / 1000).toFixed(1) + " Mbps"
                                    : appSettings.screenShareTargetKbps + " kbps"
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.fontSize.sm
                                color: Theme.fg0
                                horizontalAlignment: Text.AlignRight
                            }
                            CapBadge {
                                cap: serverManager.activeServer
                                    ? serverManager.activeServer.maxScreenShareBitrate : -1
                                suffix: " kbps"
                            }
                        }
                    }

                    SettingRow {
                        title: "Keyframe interval"
                        description: "Seconds between full frames. Shorter "
                                   + "recovers from packet loss faster; longer "
                                   + "compresses better on stable links."
                        RowLayout {
                            spacing: Theme.sp.s3
                            ThemedSlider {
                                id: gopSlider
                                implicitWidth: 200
                                from: 1; to: 30; stepSize: 1
                                value: appSettings.screenShareKeyframeSec
                                onMoved: appSettings.screenShareKeyframeSec = Math.round(value)
                            }
                            Text {
                                Layout.preferredWidth: 48
                                text: appSettings.screenShareKeyframeSec + " s"
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.fontSize.sm
                                color: Theme.fg0
                                horizontalAlignment: Text.AlignRight
                            }
                        }
                    }

                    SettingRow {
                        title: "Video codec"
                        description: "H.265 carries the same picture in "
                                   + "roughly 40% fewer bits, which matters "
                                   + "on an internet call and not on a LAN. "
                                   + "A share is encoded ONCE for everyone, "
                                   + "so H.265 is used only while every "
                                   + "viewer can decode it — anyone joining "
                                   + "who can't switches the whole call back "
                                   + "to H.264 automatically."
                        ThemedComboBox {
                            id: videoCodecCombo
                            implicitWidth: 220
                            textRole: "label"
                            model: [
                                { label: "Automatic",   value: "auto"       },
                                { label: "Prefer H.265", value: "preferHevc" },
                                { label: "H.264 only",  value: "h264Only"   }
                            ]
                            function syncFromSettings() {
                                var want = appSettings.videoCodecPreference;
                                for (var i = 0; i < model.length; ++i) {
                                    if (model[i].value === want) {
                                        currentIndex = i;
                                        return;
                                    }
                                }
                                currentIndex = 0;
                            }
                            // Same reason as the input-mode box above:
                            // this popup is created once and reused, so
                            // Component.onCompleted alone would never see
                            // a value changed elsewhere.
                            Component.onCompleted: syncFromSettings()
                            onActivated: appSettings.videoCodecPreference =
                                         model[currentIndex].value
                        }
                    }

                    // S-8: demoted to experimental rather than removed.
                    // The tier works — it is mathematically lossless —
                    // but only inside an envelope most calls are not in,
                    // and it was previously presented as an ordinary
                    // quality setting. The description states the
                    // envelope instead of the capability.
                    SettingRow {
                        title: "Lossless mode (experimental)"
                        description: "Experimental: pixel-exact AV1, "
                                   + "realistic only on a LAN, with at "
                                   + "most two viewers, on static content "
                                   + "— it needs far more bandwidth than "
                                   + "H.264 and drops frames rather than "
                                   + "delay them. Off unless you turn it "
                                   + "on, and used only when everyone in "
                                   + "the call supports it."
                        RowLayout {
                            spacing: Theme.sp.s3
                            ThemedSwitch {
                                enabled: !serverManager.activeServer
                                      || serverManager.activeServer.allowLossless
                                checked: appSettings.screenShareLossless
                                onToggled: appSettings.screenShareLossless = checked
                            }
                            Text {
                                visible: serverManager.activeServer
                                      && !serverManager.activeServer.allowLossless
                                text: "disabled by server policy"
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.xs
                                color: Theme.warn
                            }
                        }
                    }

                    SettingRow {
                        title: "JPEG quality (legacy peers)"
                        description: "Only used toward older clients that "
                                   + "don't speak the H.264 video path. 1 is "
                                   + "postage-stamp lossy, 100 near-lossless."
                        RowLayout {
                            spacing: Theme.sp.s3
                            ThemedSlider {
                                id: qSlider
                                implicitWidth: 200
                                from: 1; to: 100; stepSize: 1
                                value: appSettings.screenShareJpegQuality
                                onMoved: appSettings.screenShareJpegQuality = Math.round(value)
                            }
                            Text {
                                Layout.preferredWidth: 48
                                text: "Q" + appSettings.screenShareJpegQuality
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.fontSize.sm
                                color: Theme.fg0
                                horizontalAlignment: Text.AlignRight
                            }
                            CapBadge {
                                cap: serverManager.activeServer
                                    ? serverManager.activeServer.maxScreenShareJpeg : -1
                                suffix: ""
                            }
                        }
                    }

                    InfoBanner {
                        icon: "signal"
                        text: "Settings apply live — moving a slider mid-share "
                            + "takes effect on the next frame. The server admin "
                            + "may cap individual axes; effective values clamp "
                            + "to min(your-pick, server-cap)."
                    }
                    // No fillHeight spacer: the Flickable sizes from
                    // implicitHeight; a stretch item would inflate it.
                    }
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
                            // A user toggle writes `checked` itself, which
                            // replaces the binding above — so put it back, or
                            // this switch stops tracking the setting the moment
                            // it is first touched (D-C1's rule, applied to
                            // every control in this dialog that can be written
                            // imperatively).
                            onToggled: {
                                appSettings.notificationsEnabled = checked;
                                checked = Qt.binding(function() {
                                    return appSettings.notificationsEnabled;
                                });
                            }
                        }
                    }

                    SettingRow {
                        title: "Play a sound"
                        description: "Play the notification chime when a new message arrives."
                        ThemedSwitch {
                            enabled: appSettings.notificationsEnabled
                            checked: appSettings.notificationSound
                            // A user toggle writes `checked` itself, which
                            // replaces the binding above — so put it back, or
                            // this switch stops tracking the setting the moment
                            // it is first touched (D-C1's rule, applied to
                            // every control in this dialog that can be written
                            // imperatively).
                            onToggled: {
                                appSettings.notificationSound = checked;
                                checked = Qt.binding(function() {
                                    return appSettings.notificationSound;
                                });
                            }
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

            // ---- Updates (index 4) ----
            // Backed by the C++ Updater object exposed as `updater`
            // on desktop builds. Mobile sees an empty placeholder
            // since the OS store owns updates there.
            Item {
                // D-H4: the Updates page overflows once the release notes
                // expand, with the channel picker and the banner below them.
                // With a bare ColumnLayout the overflow simply painted past
                // the dialog's edge: no clip, no scroll, no way to reach it.
                // Same Flickable wrapper as the Screen Share page.
                Flickable {
                    id: updatesColFlick
                    anchors.fill: parent
                    anchors.margins: Theme.sp.s7 * 2
                    contentHeight: updatesCol.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ThemedScrollBar {}

                ColumnLayout {
                    id: updatesCol
                    // Bind to the Flickable, NOT parent (the contentItem):
                    // with contentWidth unset the contentItem's width follows
                    // its children, so parent.width is circular.
                    width: updatesColFlick.width
                    spacing: Theme.sp.s7

                    SectionHeader { text: "Updates" }

                    // Status first, preferences second: "do I need to do
                    // anything" is why people open this pane.
                    //
                    // Same component the modal UpdateDialog uses, so the
                    // eight updater states are worded once. This replaced
                    // a "Current version" row whose description was a
                    // second, slightly different copy of that table plus
                    // a loose row of buttons underneath it.
                    //
                    // While Client Settings is open the modal suppresses
                    // itself (see UpdateDialog.qml and main.qml), so this
                    // panel — not a popup over the top of this dialog —
                    // is what reports the download.
                    UpdatePanel {
                        Layout.fillWidth: true
                        embedded: true
                    }

                    SettingRow {
                        title: "Check for updates automatically"
                        // Was: promises a one-click "Restart to install".
                        // True on macOS and Windows, false on Linux —
                        // where applyUpdate() opens the release page and
                        // the package manager does the rest — so the
                        // promise is now the prompt, not the mechanism.
                        description: "Polls GitHub Releases on launch and "
                                   + "every six hours afterwards. When a "
                                   + "newer build is published you get a "
                                   + "prompt, rather than having to come "
                                   + "here and look."
                        ThemedSwitch {
                            checked: appSettings.autoUpdateCheck
                            // A user toggle writes `checked` itself, which
                            // replaces the binding above — so put it back, or
                            // this switch stops tracking the setting the moment
                            // it is first touched (D-C1's rule, applied to
                            // every control in this dialog that can be written
                            // imperatively).
                            onToggled: {
                                appSettings.autoUpdateCheck = checked;
                                checked = Qt.binding(function() {
                                    return appSettings.autoUpdateCheck;
                                });
                            }
                        }
                    }

                    UpdateChannelSettings { Layout.fillWidth: true }

                    // The "Current version" row and the loose Check now /
                    // Download / Restart button row that used to live
                    // here are both inside UpdatePanel above now — same
                    // controls, grouped with the status they act on
                    // instead of floating below it.

                    InfoBanner {
                        icon: "shield"
                        text: "Updates are fetched directly from the BSFChat "
                            + "GitHub Releases over HTTPS. Linux users get "
                            + "redirected to the release page to use their "
                            + "distro's package manager rather than an "
                            + "in-place upgrade."
                    }

                    // No trailing fillHeight spacer: inside a Flickable the
                    // column is sized by its content.
                }
                }
            }

            // ── Advanced ────────────────────────────────────────
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.sp.s7 * 2
                    spacing: Theme.sp.s7

                    SectionHeader { text: "Advanced" }

                    SettingRow {
                        title: "Video diagnostics overlay"
                        description: "Shows live receive statistics on "
                                   + "incoming video streams: resolution, "
                                   + "decoded fps, bitrate, compression "
                                   + "ratio, and dropped frames. Handy "
                                   + "when reporting quality issues."
                        ThemedSwitch {
                            checked: appSettings.showVideoDiagnostics
                            // A user toggle writes `checked` itself, which
                            // replaces the binding above — so put it back, or
                            // this switch stops tracking the setting the moment
                            // it is first touched (D-C1's rule, applied to
                            // every control in this dialog that can be written
                            // imperatively).
                            onToggled: {
                                appSettings.showVideoDiagnostics = checked;
                                checked = Qt.binding(function() {
                                    return appSettings.showVideoDiagnostics;
                                });
                            }
                        }
                    }

                    SettingRow {
                        title: "Verbose voice logging"
                        description: "Writes detailed voice/video events "
                                   + "(peer setup, tracks, encoder, rate "
                                   + "control) to the log file. Applies "
                                   + "immediately; leave off unless "
                                   + "you're chasing a problem — the log "
                                   + "grows quickly."
                        ThemedSwitch {
                            checked: appSettings.verboseVoiceLogging
                            // A user toggle writes `checked` itself, which
                            // replaces the binding above — so put it back, or
                            // this switch stops tracking the setting the moment
                            // it is first touched (D-C1's rule, applied to
                            // every control in this dialog that can be written
                            // imperatively).
                            onToggled: {
                                appSettings.verboseVoiceLogging = checked;
                                checked = Qt.binding(function() {
                                    return appSettings.verboseVoiceLogging;
                                });
                            }
                        }
                    }

                    SettingRow {
                        title: "Log files"
                        description: "Rotating client log (5 MB × 4 "
                                   + "generations). Attach these when "
                                   + "reporting bugs."
                        Button {
                            text: "Open log folder"
                            onClicked: Qt.openUrlExternally(
                                appSettings.logDirectory().startsWith("/")
                                    ? "file://" + appSettings.logDirectory()
                                    : "file:///" + appSettings.logDirectory())
                            contentItem: Text {
                                text: parent.text
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.sm
                                font.weight: Theme.fontWeight.medium
                                color: Theme.fg0
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                color: parent.hovered ? Theme.bg3 : Theme.bg2
                                border.color: Theme.line
                                border.width: 1
                                radius: Theme.r2
                                implicitWidth: 140
                                implicitHeight: Theme.controlHeight.md
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }
        }
    }
}
