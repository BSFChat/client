import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import BSFChat

// VoiceDock (SPEC §3.5) — persistent 64h call-controls bar anchored below
// the main content. Visible only while in a voice channel.
//
// LEAVE IS ANCHORED TO THE DOCK'S RIGHT EDGE, NOT LAID OUT WITH THE REST.
// Everything else lives in a RowLayout that ends where that button begins.
// This is structural and it is the point of the arrangement, so do not
// "tidy" the button back into the control cluster.
//
// The comment that used to be here claimed this layout "never overflows
// when the main column is narrow". It was a single RowLayout with fixed
// margins, and a RowLayout neither wraps nor clips: when its children's
// preferred widths exceed the width available, it simply lays them out
// past the edge. Measured on a 412 dp Pixel 6 Pro with the full desktop
// control set, the row wanted 655 dp and the disconnect button — last
// child of the centre cluster — started at x = 569. It was 197 dp off the
// right of the screen. The owner reported a call he could not hang up.
//
// Three properties keep that from coming back, and each is load-bearing:
//
//   • the leave button's position is derived from the SCREEN EDGE, so no
//     number of controls added to its left can move it;
//   • the identity cluster is the only thing that grows, and it is the
//     only thing allowed to shrink (fillWidth on a phone, minimumWidth 0,
//     elided text), so it absorbs every squeeze;
//   • the row clips, so a future overrun is a truncated control rather
//     than one painted over the one control you always need.
//
// tests/test_qml_hygiene.cpp measures the worst-case width against the
// narrowest phone we support and fails if it stops fitting.
Rectangle {
    id: dock
    color: Theme.bg1
    implicitHeight: visible ? Theme.layout.voiceDockH : 0
    visible: serverManager.activeServer
             && serverManager.activeServer.inVoiceChannel

    // Top-edge divider.
    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 1
        color: Theme.line
    }

    // 40×40 dock button with a tintable SVG icon. Toggled state tints the
    // glyph red (mute/deafen engaged). `danger` variant is used for the
    // disconnect action — solid-fill red with an onAccent glyph.
    component DockButton: Rectangle {
        id: btn
        property string icon: ""
        property bool   toggled: false
        property bool   danger:  false
        property string tooltip: ""
        property bool   enabled2: true
        signal clicked()
        // Secondary action. Only the camera button uses it so far (its
        // per-share IP-privacy option); every other DockButton simply has no
        // handler and a right-click does nothing.
        signal rightClicked()

        // 44 on touch — Apple HIG / Material minimum, and these are the
        // controls a call is run from. Desktop stays at the compact 40.
        implicitWidth: Theme.isMobile ? 44 : 40
        implicitHeight: Theme.isMobile ? 44 : 40
        radius: Theme.r2
        color: danger     ? Theme.danger
             : toggled    ? Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.18)
             : hover.containsMouse && enabled2 ? Theme.bg3
             : Theme.bg2
        opacity: enabled2 ? 1.0 : 0.45
        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

        Icon {
            anchors.centerIn: parent
            name: btn.icon
            size: 18
            color: btn.danger  ? "white"
                 : btn.toggled ? Theme.danger
                 : Theme.fg1
        }

        MouseArea {
            id: hover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: btn.enabled2 ? Qt.PointingHandCursor : Qt.ArrowCursor
            enabled: btn.enabled2
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: function(mouse) {
                if (mouse.button === Qt.RightButton) btn.rightClicked();
                else btn.clicked();
            }
        }

        ToolTip.visible: hover.containsMouse && tooltip.length > 0
        ToolTip.text: tooltip
        ToolTip.delay: 500
    }

    // Hang up. Declared BEFORE the row so the row can anchor to it.
    //
    // Anchored, not laid out: its right edge is one gutter in from the
    // dock's, whatever else the dock contains. Nothing to its left can
    // push it anywhere, which is the one guarantee this control needs.
    DockButton {
        id: leaveBtn
        // Declared before the row, so paint order would put the row on top.
        // The row clips short of this button and cannot reach it, but the
        // whole point here is not depending on that arithmetic staying true.
        z: 1
        anchors.right: parent.right
        anchors.rightMargin: Theme.isMobile ? Theme.mobileGutter : Theme.sp.s7
        anchors.verticalCenter: parent.verticalCenter
        icon: "phone-off"
        danger: true
        tooltip: "Disconnect"
        // The one control in here a screen-reader user must be able to
        // find by name. It had no Accessible block at all.
        Accessible.role: Accessible.Button
        Accessible.name: "Leave voice channel"
        Accessible.description: "Disconnect from the call"
        Accessible.onPressAction: leaveBtn.clicked()
        onClicked: if (serverManager.activeServer)
                       serverManager.activeServer.leaveVoiceChannel()
    }

    RowLayout {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        // Ends where the leave button begins — that button is not in this
        // layout and this layout may not reach past it.
        anchors.right: leaveBtn.left
        anchors.leftMargin: Theme.isMobile ? Theme.mobileGutter : Theme.sp.s7
        anchors.rightMargin: Theme.sp.s5
        // A RowLayout does not wrap. If the controls ever outgrow the
        // space again, they are cut off here rather than drawn over the
        // hang-up button.
        clip: true
        spacing: Theme.sp.s5

        // ─── Left cluster: self identity + connection line ──────────
        // Click-to-focus: tapping anywhere in this cluster swaps the main
        // area back to the VoiceRoom view (handy when the user wandered
        // into a text channel mid-call and wants to see the participant
        // grid again). r1 hover tint gives visual confirmation it's live.
        Rectangle {
            id: leftCluster
            Layout.alignment: Qt.AlignVCenter
            // THE compressible item. On a phone it takes the slack and
            // gives it all back again when the controls need the room —
            // down to nothing, which is correct: you are holding your own
            // phone and already know who you are. On a desktop it keeps
            // its natural width so the two spacers still centre the
            // controls exactly as before.
            Layout.fillWidth: Theme.isMobile
            Layout.minimumWidth: 0
            Layout.preferredWidth: leftClusterRow.implicitWidth + Theme.sp.s3 * 2
            Layout.maximumWidth: Theme.isMobile
                ? Number.POSITIVE_INFINITY
                : leftClusterRow.implicitWidth + Theme.sp.s3 * 2
            clip: true
            implicitHeight: leftClusterRow.implicitHeight + Theme.sp.s2 * 2
            radius: Theme.r1
            color: leftClusterHover.containsMouse
                   && !serverManager.activeServer.viewingVoiceRoom
                   ? Theme.bg2 : "transparent"
            Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

            MouseArea {
                id: leftClusterHover
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: !serverManager.activeServer.viewingVoiceRoom
                             ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: if (serverManager.activeServer)
                               serverManager.activeServer.showVoiceRoom()
            }

            RowLayout {
                id: leftClusterRow
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: Theme.sp.s3
                // Bounded on the right too. Anchored only on the left, this
                // row kept its full implicit width and spilled straight out
                // of the cluster that was supposed to be clipping it, so
                // the elide below never engaged.
                anchors.right: parent.right
                anchors.rightMargin: Theme.sp.s3
                spacing: Theme.sp.s3

                Rectangle {
                    Layout.preferredWidth: Theme.avatar.md
                    Layout.preferredHeight: Theme.avatar.md
                    Layout.minimumWidth: 0
                    width: Theme.avatar.md; height: Theme.avatar.md
                    // Rounded-square to match every other avatar in the
                    // app (ServerRail / MemberList / UserSettings / profile
                    // card). We were still on the circular legacy here.
                    radius: Theme.r2
                    color: Theme.senderColor(serverManager.activeServer
                        ? (serverManager.activeServer.userId || "") : "")
                    Text {
                        anchors.centerIn: parent
                        text: {
                            if (!serverManager.activeServer) return "?";
                            var n = serverManager.activeServer.displayName
                                 || serverManager.activeServer.userId;
                            var stripped = n.replace(/^[^a-zA-Z0-9]+/, "");
                            return (stripped.length > 0
                                    ? stripped.charAt(0) : "?").toUpperCase();
                        }
                        font.family: Theme.fontSans
                        font.pixelSize: 14
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.onAccent
                    }
                }

                ColumnLayout {
                    spacing: 0
                    Layout.maximumWidth: 180
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0

                    Text {
                        // Your own name, on your own phone. It is the least
                        // informative thing in the dock and the widest, so on
                        // a phone it is the first thing to go — the channel
                        // line below it is the part that answers "which call
                        // am I in?" while you are off reading a text channel.
                        visible: !Theme.isMobile
                        text: serverManager.activeServer
                              ? serverManager.activeServer.displayName : ""
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.base
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.fg0
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                    }
                    Text {
                        text: {
                            var s = serverManager.activeServer;
                            if (!s || !s.activeVoiceRoomId) return "";
                            var name = s.roomListModel
                                       ? s.roomListModel.roomDisplayName(s.activeVoiceRoomId)
                                       : s.activeVoiceRoomId;
                            // "Connected to #general" is three words of
                            // preamble and one word of information. On a
                            // phone the dock is only ever visible while you
                            // ARE connected, so the preamble says nothing the
                            // dock's own presence has not already said.
                            return (Theme.isMobile ? "#" : "Connected to #") + name;
                        }
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                        color: Theme.fg2
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                    }
                }

                // ─── The IP-privacy shield ──────────────────────────
                //
                // Shown only while the local transport policy is relay-only,
                // and it does NOT claim an address is hidden until the
                // selected candidate pair actually is relayed — that decision
                // is voice::ipExposure()'s, made in C++, and both strings
                // below arrive through a Q_PROPERTY. Nothing about how a call
                // is routed may be written here: a label composed in QML is a
                // label nothing can check, which is exactly how the media
                // badge once came to claim a protocol this client does not
                // implement. See src/voice/IpPrivacy.h.
                Rectangle {
                    // Keyed off the PROPERTY, not off whether a sibling Text
                    // happens to be rendering. It used to read
                    // `ipShieldText.text.length > 0`, which coupled the
                    // badge's existence to that one label and meant the label
                    // could not simply be hidden on a phone — it had to be
                    // squashed to zero width instead, which in turn read to
                    // the touch-target scan as somebody sizing a 0 pt button.
                    visible: serverManager.activeServer
                             && serverManager.activeServer.voiceIpPrivacyBadge.length > 0
                    // Icon-only on a phone. The badge's four words are what
                    // made it wide; the tap-to-open detail below carries the
                    // whole sentence either way, so the shield keeps its
                    // meaning at a third of the width rather than being cut
                    // from the one surface that answers "can they see my IP?".
                    implicitWidth: ipShieldRow.implicitWidth + Theme.sp.s3
                    Layout.minimumWidth: 0
                    implicitHeight: 20
                    radius: Theme.r1
                    color: Theme.accentGlow
                    Layout.alignment: Qt.AlignVCenter

                    RowLayout {
                        id: ipShieldRow
                        anchors.centerIn: parent
                        spacing: Theme.sp.s1

                        Icon {
                            name: "shield"
                            size: 12
                            color: Theme.accent
                        }
                        Text {
                            id: ipShieldText
                            visible: !Theme.isMobile
                            text: serverManager.activeServer
                                ? serverManager.activeServer.voiceIpPrivacyBadge : ""
                            font.family: Theme.fontSans
                            font.pixelSize: 10
                            font.weight: Theme.fontWeight.semibold
                            color: Theme.accent
                        }
                    }

                    // The badge is four words; the cost is not. Same shape as
                    // the protection badge in VoiceRoom — detail verbatim from
                    // the property, `contentWidth` so it wraps instead of
                    // laying out as one screen-wide line.
                    //
                    // Tappable on touch, where nothing hovers: this badge
                    // is the client's answer to "can the other people in
                    // this call see my IP address?", and on a phone the
                    // answer itself — four words of badge — was all you
                    // could get. Same tap-to-open / tap-or-timeout-to-close
                    // treatment as VoiceRoom's protection badge.
                    MouseArea {
                        id: ipShieldHover
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            if (!Theme.isMobile) return;
                            ipShieldPin.pinned = !ipShieldPin.pinned;
                            if (ipShieldPin.pinned) ipShieldPin.restart();
                            else ipShieldPin.stop();
                        }
                    }
                    Timer {
                        id: ipShieldPin
                        property bool pinned: false
                        interval: 8000
                        onTriggered: pinned = false
                    }
                    ToolTip {
                        visible: (ipShieldHover.containsMouse
                                  || ipShieldPin.pinned) && text.length > 0
                        text: serverManager.activeServer
                            ? serverManager.activeServer.voiceIpPrivacyDetail : ""
                        delay: 400
                        contentWidth: 320
                    }
                }
            }
        }

        // Flexible spacers — these are what centre the control cluster on a
        // desktop. Hidden on a phone: there is no slack to distribute there,
        // and the identity cluster is the thing that flexes instead.
        Item { Layout.fillWidth: true; visible: !Theme.isMobile }

        // ─── Center cluster ─────────────────────────────────────────
        RowLayout {
            spacing: Theme.sp.s3
            Layout.alignment: Qt.AlignVCenter

            // PTT mode replaces the toggle-mute button with a
            // hold-to-talk variant. The underlying ServerConnection
            // already wires setPttPressed() into the AudioEngine's
            // effective-muted state — the dock button is just a
            // chrome: press = transmit, release = silence. The
            // button tints bright-accent while held so the user
            // has a clear "you're live" signal.
            DockButton {
                id: pttBtn
                visible: appSettings
                         && appSettings.voiceMode === "ptt"
                icon: "mic"
                toggled: serverManager.activeServer
                         && serverManager.activeServer.pttPressed
                tooltip: toggled ? "Transmitting" : "Hold to talk"
                // Override colours: while held, paint with the
                // accent (green) instead of the danger (red) tint
                // DockButton normally applies to `toggled`. Inline
                // the hover/normal branch so users see a calm
                // "armed, not yet transmitting" state.
                color: toggled ? Theme.accent
                     : Theme.bg2

                MouseArea {
                    anchors.fill: parent
                    onPressed: if (serverManager.activeServer)
                                   serverManager.activeServer.setPttPressed(true)
                    onReleased: if (serverManager.activeServer)
                                    serverManager.activeServer.setPttPressed(false)
                    // If the finger leaves the button entirely we
                    // also release — prevents a stuck-open PTT
                    // from dragging off the widget.
                    onCanceled: if (serverManager.activeServer)
                                    serverManager.activeServer.setPttPressed(false)
                }
            }

            DockButton {
                visible: !(appSettings && appSettings.voiceMode === "ptt")
                icon: serverManager.activeServer
                      && serverManager.activeServer.voiceMuted ? "mic-off" : "mic"
                toggled: serverManager.activeServer
                         && serverManager.activeServer.voiceMuted
                tooltip: toggled ? "Unmute" : "Mute"
                onClicked: if (serverManager.activeServer)
                               serverManager.activeServer.toggleMute()
            }

            DockButton {
                icon: serverManager.activeServer
                      && serverManager.activeServer.voiceDeafened
                      ? "headphones-off" : "headphones"
                toggled: serverManager.activeServer
                         && serverManager.activeServer.voiceDeafened
                tooltip: toggled ? "Undeafen" : "Deafen"
                onClicked: if (serverManager.activeServer)
                               serverManager.activeServer.toggleDeafen()
            }

            // Screen-share + camera context properties only get set
            // on platforms where their capture path is implemented
            // (macOS/Windows/Linux desktop + Android via its own
            // controller; not iOS). The `typeof … !== "undefined"`
            // guard hides the button cleanly on unsupported
            // platforms — see main.cpp for the platform gate.
            DockButton {
                visible: typeof screenShare !== "undefined"
                icon: "screen-share"
                tooltip: visible && screenShare.active
                    ? "Stop sharing screen"
                    : "Share screen or window…"
                toggled: visible && screenShare.active
                onClicked: {
                    if (!visible) return;
                    if (screenShare.active) {
                        screenShare.stop();
                        return;
                    }
                    // macOS has a native window/app/display picker
                    // (SCContentSharingPicker) behind showPicker();
                    // Windows/Linux get our QML picker, which lists
                    // displays (QScreenCapture) and individual
                    // windows (QWindowCapture). Always a dialog now —
                    // even one monitor leaves windows to choose from.
                    if (Qt.platform.os === "osx") {
                        screenShare.showPicker();
                    } else {
                        screenPickerDialog.open();
                    }
                }
                ScreenPickerDialog { id: screenPickerDialog }
                Connections {
                    target: typeof screenShare !== "undefined"
                            ? screenShare : null
                    // `dock.` is load-bearing. Connections is a QObject, not
                    // an Item, so an unqualified Window.window attaches to
                    // Connections itself — which Qt rejects ("Window.window
                    // does only support types deriving from Item") and
                    // evaluates to null. The error was then dropped on the
                    // floor: a screen-share failure raised no toast at all.
                    // Going through the dock, which is an Item, attaches it
                    // where it works.
                    function onLastErrorChanged() {
                        var err = screenShare.lastError;
                        var win = dock.Window.window;
                        if (err && err.length > 0 && win && win.toastError) {
                            win.toastError(err);
                        }
                    }
                }
            }

            DockButton {
                id: cameraBtn
                visible: typeof camera !== "undefined"
                icon: "video"
                tooltip: visible && camera.active
                    ? "Stop camera" : "Start camera"
                toggled: visible && camera.active
                // Right-click is where the per-share option lives for the
                // camera. There is no camera picker to put it in — the button
                // starts the camera on click — and a second permanently
                // visible control beside an already-crowded dock buys less
                // than a menu that is there when you look for it. The default
                // still follows the global setting, so somebody who never
                // opens this menu gets exactly what they asked for in
                // Settings.
                onRightClicked: {
                    if (!visible) return;
                    cameraHideIpItem.checked =
                        camera.hideIpForShare()
                        || appSettings.voiceRelayMode === "relayOnly";
                    cameraMenu.popup();
                }

                Menu {
                    id: cameraMenu
                    MenuItem {
                        id: cameraHideIpItem
                        text: "Hide my IP address while my camera is on"
                        checkable: true
                        // Shown disabled with no relay to use, for the same
                        // reason the picker's switch is: an option that is
                        // simply absent explains nothing.
                        enabled: typeof camera !== "undefined"
                                 && camera.canHideIpWhileSharing()
                        onTriggered: camera.setHideIpForShare(checked)
                    }
                }

                onClicked: {
                    if (!visible) return;
                    if (camera.active) {
                        camera.stop();
                        return;
                    }
                    // The menu's value is a per-share intent, so it is pushed
                    // fresh at every start rather than only when the menu item
                    // is clicked: a user who has never opened the menu still
                    // gets their global setting honoured here.
                    camera.setHideIpForShare(
                        cameraHideIpItem.checked
                        || appSettings.voiceRelayMode === "relayOnly");
                    // Mobile: ask for CAMERA runtime perm before the
                    // first start. Desktop's hasCamera() short-
                    // circuits to true so this branch is free there.
                    if (Theme.isMobile
                        && typeof androidPerms !== "undefined"
                        && !androidPerms.hasCamera()) {
                        var once = null;
                        once = function(granted) {
                            androidPerms.cameraResult.disconnect(once);
                            if (granted) {
                                camera.start();
                            } else if (dock.Window.window
                                       && dock.Window.window.toastError) {
                                dock.Window.window.toastError(
                                    "Camera permission is required.");
                            }
                        };
                        androidPerms.cameraResult.connect(once);
                        androidPerms.requestCamera();
                        return;
                    }
                    camera.start();
                }
                Connections {
                    target: typeof camera !== "undefined" ? camera : null
                    // Same as the screen-share Connections above: the window
                    // has to be reached through an Item, or it is null here.
                    function onLastErrorChanged() {
                        var err = camera.lastError;
                        var win = dock.Window.window;
                        if (err && err.length > 0 && win && win.toastError) {
                            win.toastError(err);
                        }
                    }
                }
            }

        }

        Item { Layout.fillWidth: true; visible: !Theme.isMobile }

        // ─── Right cluster: mic level meter ─────────────────────────
        // Six 3 px bars. Informative on a desktop you are sitting back
        // from; on a phone it is 38 dp of decoration competing with the
        // hang-up button, and the mute control already carries the state
        // that matters.
        RowLayout {
            visible: !Theme.isMobile
            spacing: Theme.sp.s1
            Layout.alignment: Qt.AlignVCenter

            Repeater {
                model: 6
                delegate: Rectangle {
                    required property int index
                    width: 3
                    height: 10 + index * 2
                    radius: 1.5
                    readonly property real level:
                        serverManager.activeServer
                        ? serverManager.activeServer.micLevel : 0
                    readonly property real threshold: (index + 1) / 7
                    color: {
                        if (serverManager.activeServer
                            && serverManager.activeServer.micSilent) return Theme.danger;
                        return level >= threshold ? Theme.accent : Theme.bg3;
                    }
                    Behavior on color { ColorAnimation { duration: 60 } }
                }
            }
        }
    }
}
