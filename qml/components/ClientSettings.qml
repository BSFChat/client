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
    // On a phone this is a full-screen pane, not a dialog. 0.9 × 0.85 of a
    // 390 pt viewport left a 351 pt box that then spent 180 of it on a
    // fixed-width nav rail and 64 more on page margins — 107 pt for rows
    // whose controls are 220 to 260 wide. Every one of them ran off the
    // right edge. Phones give settings the whole screen; so do we.
    width: Theme.isMobile
        ? (parent ? parent.width - 2 * Theme.mobileGutter : 360)
        : Math.min(parent ? parent.width * 0.9 : 720, 900)
    height: Theme.isMobile
        ? (parent ? parent.height - 2 * Theme.mobileGutter : 600)
        : Math.min(parent ? parent.height * 0.85 : 600, 640)
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    property int section: 0
    // One list, read by the desktop nav rail and by the phone's section
    // dropdown. Two copies of it is how a section gets added to one and
    // not the other.
    readonly property var sections: ["Appearance", "Audio", "Screen Share",
                                     "Notifications", "Updates", "Advanced"]

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
            // 44 pt on a phone. This pane takes the whole screen there,
            // Esc does not exist and click-outside has nowhere to land,
            // so this X is the ONLY way back out — at 28 it was well
            // under the touch minimum.
            width: Theme.isMobile ? 44 : 28
            height: Theme.isMobile ? 44 : 28
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
    // A GridLayout rather than a RowLayout so the same declaration is a
    // row on a desktop and a STACK on a phone: `columns: 1` puts the
    // control underneath its title instead of beside it.
    //
    // Side by side is wrong on a phone whatever the pane is doing. The
    // controls handed to these rows are 220–260 pt wide combos and
    // sliders, sized for a desktop dialog, and a phone pane simply does
    // not have that much width LEFT once a title column has taken its
    // share — so the row's implicit minimum exceeded the viewport and the
    // right-hand control hung off the edge of the dialog. Stacked, the
    // control gets the full width of the pane and the title gets its own
    // line, which is also what every phone settings screen does.
    component SettingRow: GridLayout {
        property string title: ""
        property string description: ""
        default property alias rightControl: rightContainer.children
        Layout.fillWidth: true
        columns: Theme.isMobile ? 1 : 2
        columnSpacing: Theme.sp.s7
        rowSpacing: Theme.sp.s3

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
            // Stacked on a phone, so it starts at the left margin under
            // the title rather than floating in the middle of its own row.
            Layout.alignment: Theme.isMobile ? Qt.AlignLeft : Qt.AlignVCenter
            implicitWidth: childrenRect.width
            implicitHeight: childrenRect.height
        }
    }

    // GridLayout, not RowLayout, so this is a row on a desktop and a stack
    // on a phone without the tree being written twice. `columns: 1` on
    // mobile, and the two children that do not belong to that form factor
    // are `visible: false` — which a layout skips entirely, so each form
    // factor sees exactly two children in the order it wants them.
    contentItem: GridLayout {
        columns: Theme.isMobile ? 1 : 2
        rowSpacing: 0
        columnSpacing: 0

        // Left nav.
        //
        // Gone on a phone, where 180 pt of fixed rail out of a ~360 pt
        // viewport is half the screen spent on navigation. The phone gets
        // the dropdown below instead, and this whole column collapses.
        Rectangle {
            visible: !Theme.isMobile
            Layout.fillHeight: true
            // Unconditional. A QtQuick layout skips `visible: false`
            // children outright — they get no cell and their size hints are
            // never read — which is the same fact the GridLayout above
            // relies on to stack the two form factors. So a
            // `Theme.isMobile ? 0 : 180` here was inert, and it read to
            // aMobileSizeBranchIsNeverBelowTheTouchMinimum as somebody
            // sizing a 0 pt touch target. Do not put the branch back.
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
                    model: clientSettingsPopup.sections
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

        // Phone section picker — the replacement for the nav rail, and the
        // reason the container above is a GridLayout. On a phone the rail
        // is `visible: false`, a layout skips invisible children, and
        // `columns: 1` stacks what is left: picker on top, pages below. On
        // a desktop this one is the invisible child and the rail and the
        // pages sit side by side exactly as before.
        //
        // A dropdown rather than a row of chips: six section names do not
        // fit across a phone without either scrolling sideways (which
        // nothing else in this app does, so nobody would think to try) or
        // shrinking the labels past the point where they can be hit.
        ThemedComboBox {
            id: mobileSectionPicker
            visible: Theme.isMobile
            Layout.fillWidth: true
            Layout.margins: Theme.mobileGutter
            Layout.bottomMargin: 0
            // The pane's close X floats over the top-right corner of the
            // background at z:10 and does not participate in this layout, so
            // the picker has to step around it by hand: 12 (its margin) + 44
            // (its size) + a gap.
            Layout.rightMargin: Theme.sp.s5 + 44 + Theme.sp.s3
            // 44 pt: Apple's HIG / Material touch minimum. This is the one
            // control on the pane that every other one is reached through.
            implicitHeight: 44
            model: clientSettingsPopup.sections
            currentIndex: clientSettingsPopup.section
            // Re-established with Qt.binding, not left as the plain value
            // ComboBox just wrote (D-C1 — the rule this file states for
            // every control that is both bound and written imperatively).
            //
            // Selecting a row makes ComboBox assign `currentIndex` itself,
            // which DESTROYS the binding above. Nothing resets `section` today, so this one is
            // latent rather than live — but it is the same defect one line
            // of reset away, and the desktop rail writes the same property.
            // From then on this field is a one-shot value: it shows whatever
            // was last picked while the pages behind it show something else,
            // and the only way back is to pick a different section and
            // return. Read off `currentIndex` rather than the injected
            // signal argument, matching the other combos in the tree.
            onActivated: {
                clientSettingsPopup.section = currentIndex;
                currentIndex = Qt.binding(function() {
                    return clientSettingsPopup.section;
                });
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
                    // 32 pt of gutter on each side is a desktop dialog margin; on a
                    // phone it is a sixth of the screen. See Theme.mobileGutter.
                    anchors.margins: Theme.isMobile ? Theme.mobileGutter : Theme.sp.s7 * 2
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
                    // 32 pt of gutter on each side is a desktop dialog margin; on a
                    // phone it is a sixth of the screen. See Theme.mobileGutter.
                    anchors.margins: Theme.isMobile ? Theme.mobileGutter : Theme.sp.s7 * 2
                    contentHeight: audioCol.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ThemedScrollBar { id: audioScrollBar }

                ColumnLayout {
                    id: audioCol
                    // Bind to the Flickable, NOT parent (the contentItem):
                    // with contentWidth unset the contentItem's width follows
                    // its children, so parent.width is circular.
                    width: audioColFlick.width - audioScrollBar.reservedWidth
                    spacing: Theme.sp.s7

                    SectionHeader { text: "Audio" }

                    SettingRow {
                        title: "Input device"
                        description: "Microphone used in voice channels."
                        Column {
                            spacing: 2
                            ThemedComboBox {
                                id: inputCombo
                                implicitWidth: 260
                                model: appSettings.audioInputDevices
                                textRole: "description"
                                Component.onCompleted: selectByDescription(appSettings.audioInputDevice)
                                // The list is re-enumerated on every dialog open
                                // AND whenever QMediaDevices reports a change, so
                                // it can now move while the dialog is open — which
                                // is the whole point: a Bluetooth headset that
                                // connects right now should appear right now. When
                                // it moves, currentIndex points into the OLD list
                                // and would silently select a different device, so
                                // re-resolve.
                                onModelChanged: selectByDescription(appSettings.audioInputDevice)
                                onActivated: {
                                    var item = model[currentIndex];
                                    // The id rides along as a tie-breaker for
                                    // devices that share a description; the
                                    // description stays the stored key.
                                    appSettings.selectAudioInputDevice(
                                        item.systemDefault ? "" : item.description,
                                        item.systemDefault ? "" : item.id);
                                }
                                // "" means "follow the system default", which is
                                // the flagged first entry. Matched on the flag and
                                // not on the label, because the label now carries
                                // the current default's name inside it.
                                function selectByDescription(desc) {
                                    for (var i = 0; i < model.length; i++) {
                                        if ((desc === "" && model[i].systemDefault === true)
                                            || (desc !== "" && model[i].description === desc)) {
                                            currentIndex = i;
                                            return;
                                        }
                                    }
                                    currentIndex = 0;
                                }
                            }
                            // What the pipeline is ACTUALLY on, which the
                            // preference above does not tell you: it may say
                            // "system default", or name a device that has since
                            // gone away and been fallen back from. That gap is
                            // exactly what someone who cannot hear anything is
                            // trying to see.
                            Text {
                                width: 260
                                visible: appSettings.audioInputInUse.length > 0
                                text: "In use: " + appSettings.audioInputInUse
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.xs
                                color: Theme.fg3
                                elide: Text.ElideRight
                            }
                        }
                    }

                    // Input volume and automatic gain control. Both were
                    // missing in effect until 2026-09-21: this slider existed,
                    // persisted, and changed nothing (it was hidden in edff6cf
                    // for exactly that reason), and there was no AGC at all, so
                    // every sender was transmitted at whatever level their mic
                    // produced — which is why calls sounded quiet. The worker
                    // now applies AGC -> this gain -> a limiter; see
                    // src/voice/VoiceGain.h and src/core/AudioVolume.h.
                    SettingRow {
                        title: "Automatic gain control"
                        description: "Evens out your microphone level so others hear you at a consistent volume. Turn off only if you already level your mic with other software or hardware."
                        // Android runs the OS's own voice processing instead,
                        // and the worker never applies ours there; a switch
                        // that does nothing is the thing this page stopped
                        // shipping.
                        visible: Qt.platform.os !== "android"
                        ThemedSwitch {
                            checked: appSettings.autoGainControl
                            // Re-bind after the user's write (D-C1).
                            onToggled: {
                                appSettings.autoGainControl = checked;
                                checked = Qt.binding(function() {
                                    return appSettings.autoGainControl;
                                });
                            }
                        }
                    }

                    // Echo cancellation. macOS and iOS only — everywhere else
                    // the switch would be a switch that does nothing, which is
                    // the thing this page stopped shipping (see the AGC row
                    // above).
                    SettingRow {
                        title: "Echo cancellation"
                        description: "Lets the system remove your speakers from what your microphone picks up, so others do not hear themselves. Also applies noise suppression and gain control. Turn off if you use headphones and want your microphone untouched. Applies the next time you join voice."
                        visible: appSettings.voiceProcessingAvailable
                        ThemedSwitch {
                            checked: appSettings.voiceProcessing
                            // Re-bind after the user's write (D-C1).
                            onToggled: {
                                appSettings.voiceProcessing = checked;
                                checked = Qt.binding(function() {
                                    return appSettings.voiceProcessing;
                                });
                            }
                        }
                    }

                    SettingRow {
                        title: "Input volume"
                        description: "How loud you are to others, applied after automatic gain control. 100% leaves it unchanged."
                        RowLayout {
                            spacing: Theme.sp.s3
                            ThemedSlider {
                                id: inputVolSlider
                                // Narrower on a phone — see the screen-share sliders below.
                                implicitWidth: Theme.isMobile ? 180 : 220
                                from: 0; to: 200; stepSize: 5
                                value: appSettings.inputVolume
                                // Moving the slider writes `value` itself,
                                // replacing the binding — restore it (D-C1).
                                onMoved: {
                                    appSettings.inputVolume = Math.round(value);
                                    value = Qt.binding(function() {
                                        return appSettings.inputVolume;
                                    });
                                }
                            }
                            Text {
                                Layout.preferredWidth: 40
                                text: appSettings.inputVolume + "%"
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.sm
                                color: Theme.fg2
                                horizontalAlignment: Text.AlignRight
                            }
                        }
                    }

                    SettingRow {
                        title: "Output device"
                        description: "Speakers / headphones used for voice + notification sounds."
                        Column {
                            spacing: 2
                            ThemedComboBox {
                                id: outputCombo
                                implicitWidth: 260
                                model: appSettings.audioOutputDevices
                                textRole: "description"
                                Component.onCompleted: selectByDescription(appSettings.audioOutputDevice)
                                // The list is re-enumerated on every dialog open
                                // AND whenever QMediaDevices reports a change, so
                                // it can now move while the dialog is open — which
                                // is the whole point: a Bluetooth headset that
                                // connects right now should appear right now. When
                                // it moves, currentIndex points into the OLD list
                                // and would silently select a different device, so
                                // re-resolve.
                                onModelChanged: selectByDescription(appSettings.audioOutputDevice)
                                onActivated: {
                                    var item = model[currentIndex];
                                    // The id rides along as a tie-breaker for
                                    // devices that share a description; the
                                    // description stays the stored key.
                                    appSettings.selectAudioOutputDevice(
                                        item.systemDefault ? "" : item.description,
                                        item.systemDefault ? "" : item.id);
                                }
                                // "" means "follow the system default", which is
                                // the flagged first entry. Matched on the flag and
                                // not on the label, because the label now carries
                                // the current default's name inside it.
                                function selectByDescription(desc) {
                                    for (var i = 0; i < model.length; i++) {
                                        if ((desc === "" && model[i].systemDefault === true)
                                            || (desc !== "" && model[i].description === desc)) {
                                            currentIndex = i;
                                            return;
                                        }
                                    }
                                    currentIndex = 0;
                                }
                            }
                            // What the pipeline is ACTUALLY on, which the
                            // preference above does not tell you: it may say
                            // "system default", or name a device that has since
                            // gone away and been fallen back from. That gap is
                            // exactly what someone who cannot hear anything is
                            // trying to see.
                            Text {
                                width: 260
                                visible: appSettings.audioOutputInUse.length > 0
                                text: "In use: " + appSettings.audioOutputInUse
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.xs
                                color: Theme.fg3
                                elide: Text.ElideRight
                            }
                        }
                    }

                    // Output volume, 0-200%. Above 100% is a boost, which is
                    // the answer to "are we not offering 100% output volume?":
                    // we were, and the useful fix for a quiet call is being
                    // able to go past it. Boosted peaks go through a limiter,
                    // so they are turned down smoothly instead of clipping.
                    SettingRow {
                        title: "Output volume"
                        description: "Applied on top of your system volume. 100% is unchanged; up to 200% boosts quiet voices, with loud peaks smoothly limited instead of distorting."
                        RowLayout {
                            spacing: Theme.sp.s3
                            ThemedSlider {
                                id: outputVolSlider
                                // Narrower on a phone — see the screen-share sliders below.
                                implicitWidth: Theme.isMobile ? 180 : 220
                                from: 0; to: 200; stepSize: 5
                                value: appSettings.outputVolume
                                onMoved: {
                                    appSettings.outputVolume = Math.round(value);
                                    value = Qt.binding(function() {
                                        return appSettings.outputVolume;
                                    });
                                }
                            }
                            Text {
                                Layout.preferredWidth: 40
                                text: appSettings.outputVolume + "%"
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.sm
                                color: Theme.fg2
                                horizontalAlignment: Text.AlignRight
                            }
                        }
                    }

                    InfoBanner {
                        icon: "signal"
                        tint: Theme.warn
                        text: "Picking a specific device here applies the next time you join a voice channel. \"System default\" is live: BSFChat follows the system output as you change it mid-call, and falls back to it if the device you chose disappears."
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
                    // 32 pt of gutter on each side is a desktop dialog margin; on a
                    // phone it is a sixth of the screen. See Theme.mobileGutter.
                    anchors.margins: Theme.isMobile ? Theme.mobileGutter : Theme.sp.s7 * 2
                    contentHeight: screenShareCol.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ThemedScrollBar { id: screenShareScrollBar }

                    ColumnLayout {
                    id: screenShareCol
                    // Bind to the Flickable itself, NOT parent (the
                    // contentItem) — contentItem width follows its
                    // children when contentWidth is unset, so
                    // `parent.width` is circular and the column blows
                    // out to its implicit width, clipping the row
                    // trailers off the right edge.
                    width: screenShareFlick.width - screenShareScrollBar.reservedWidth
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
                                // Narrower on a phone. Stacked under its title the slider has the
                                // whole pane, but it shares that row with a value readout and a
                                // server-cap badge, and at 200 the badge ran off the edge.
                                implicitWidth: Theme.isMobile ? 132 : 200
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
                                // Narrower on a phone. Stacked under its title the slider has the
                                // whole pane, but it shares that row with a value readout and a
                                // server-cap badge, and at 200 the badge ran off the edge.
                                implicitWidth: Theme.isMobile ? 132 : 200
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
                                // Narrower on a phone. Stacked under its title the slider has the
                                // whole pane, but it shares that row with a value readout and a
                                // server-cap badge, and at 200 the badge ran off the edge.
                                implicitWidth: Theme.isMobile ? 132 : 200
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
                                // Narrower on a phone. Stacked under its title the slider has the
                                // whole pane, but it shares that row with a value readout and a
                                // server-cap badge, and at 200 the badge ran off the edge.
                                implicitWidth: Theme.isMobile ? 132 : 200
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
                                // Narrower on a phone. Stacked under its title the slider has the
                                // whole pane, but it shares that row with a value readout and a
                                // server-cap badge, and at 200 the badge ran off the edge.
                                implicitWidth: Theme.isMobile ? 132 : 200
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

                    // The one RECEIVE-side knob on this page: how much
                    // delay the playout buffer may add to other people's
                    // video to show it at an even pace. Worded as the
                    // trade-off it is rather than as a buffer size; the
                    // number is shown because it is a real latency cost.
                    // Default and range are argued at
                    // Settings::videoSmoothingMs().
                    SettingRow {
                        title: "Smoothness of others' video"
                        description: "Holds incoming video back by up to this "
                                   + "much so it plays at an even pace instead "
                                   + "of stuttering when the network delivers "
                                   + "frames in bursts. Only as much as needed "
                                   + "is used. Off shows every frame the moment "
                                   + "it arrives — lowest delay, choppiest. "
                                   + "Camera video is limited to 100 ms so lips "
                                   + "stay in sync with voices."
                        RowLayout {
                            spacing: Theme.sp.s3
                            ThemedSlider {
                                id: smoothingSlider
                                // Narrower on a phone. Stacked under its title the slider has the
                                // whole pane, but it shares that row with a value readout and a
                                // server-cap badge, and at 200 the badge ran off the edge.
                                implicitWidth: Theme.isMobile ? 132 : 200
                                from: 0; to: 400; stepSize: 10
                                value: appSettings.videoSmoothingMs
                                onMoved: appSettings.videoSmoothingMs = Math.round(value)
                            }
                            Text {
                                Layout.preferredWidth: 120
                                text: appSettings.videoSmoothingMs === 0
                                    ? "Off · lowest delay"
                                    : "up to " + appSettings.videoSmoothingMs + " ms"
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.fontSize.sm
                                color: Theme.fg0
                                horizontalAlignment: Text.AlignRight
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
                    // 32 pt of gutter on each side is a desktop dialog margin; on a
                    // phone it is a sixth of the screen. See Theme.mobileGutter.
                    anchors.margins: Theme.isMobile ? Theme.mobileGutter : Theme.sp.s7 * 2
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
                    // 32 pt of gutter on each side is a desktop dialog margin; on a
                    // phone it is a sixth of the screen. See Theme.mobileGutter.
                    anchors.margins: Theme.isMobile ? Theme.mobileGutter : Theme.sp.s7 * 2
                    contentHeight: updatesCol.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ThemedScrollBar { id: updatesScrollBar }

                ColumnLayout {
                    id: updatesCol
                    // Bind to the Flickable, NOT parent (the contentItem):
                    // with contentWidth unset the contentItem's width follows
                    // its children, so parent.width is circular.
                    width: updatesColFlick.width - updatesScrollBar.reservedWidth
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
                    // 32 pt of gutter on each side is a desktop dialog margin; on a
                    // phone it is a sixth of the screen. See Theme.mobileGutter.
                    anchors.margins: Theme.isMobile ? Theme.mobileGutter : Theme.sp.s7 * 2
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
