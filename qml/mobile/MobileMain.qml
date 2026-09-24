import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import BSFChat

// Mobile entry point — loaded instead of main.qml on iOS/Android.
// Reuses every desktop leaf component (MessageView, MessageBubble,
// MessageInput, MemberList, ChannelList) inside a phone-native chrome:
// left drawer for servers+channels, right drawer for members, chat
// occupies the main column.
//
// The three-panel desktop layout doesn't fit a phone; we swap it for
// an overlay-drawer pattern borrowed from Discord / Slack mobile.
// Server rail + channel list live in the left drawer; member list
// lives in the right drawer; everything else is the chat view.
ApplicationWindow {
    id: root
    visible: true
    visibility: Window.Maximized
    color: Theme.bg0

    // ── Safe-area handling ───────────────────────────────────────
    //
    // Read from Qt's own SafeArea attached property (QtQuick 6.9+), which
    // on iOS is UIKit's safeAreaInsets and on Android the window insets.
    // It replaces a hardcoded `topInset: 0`.
    //
    // Honest note on what this did and did not fix. On the device these
    // margins come back 0/0/0/0, and that is CORRECT rather than broken:
    // the window is 440x860 on a 440x956 screen, so Qt has already laid
    // it out inside the safe area and there is nothing left to inset. The
    // header was never under the Dynamic Island, and the report of a
    // message row under the status clock was the platform translating the
    // whole scene for the keyboard, not a missing inset.
    //
    // It stays because it is right rather than because it repaid itself:
    // Android does not inset for us the same way, a hardcoded zero cannot
    // be right on both, and the day anything here goes edge-to-edge these
    // are the numbers that keep the header out of the cutout.
    //
    // The margins are read off a probe Item rather than off the window,
    // because SafeArea reports the margins OF THE ITEM it is attached to:
    // attach it to something whose geometry these numbers then change and
    // you get a binding loop. ApplicationWindow's contentItem is exactly
    // such an item (the header height below is derived from topInset, and
    // the header height is what positions contentItem). The overlay is
    // not — it always fills the whole window, whatever the header does —
    // so the probe lives there and nothing it reports feeds back into it.
    //
    // It draws nothing and accepts nothing; it exists only to be measured.
    // Deliberately left visible rather than `visible: false` — SafeArea is
    // a geometry calculation and an invisible item is not worth betting
    // that it still runs.
    Item {
        id: safeAreaProbe
        parent: Overlay.overlay
        anchors.fill: parent
        enabled: false
    }
    readonly property int topInset:    Math.ceil(safeAreaProbe.SafeArea.margins.top)
    readonly property int leftInset:   Math.ceil(safeAreaProbe.SafeArea.margins.left)
    readonly property int rightInset:  Math.ceil(safeAreaProbe.SafeArea.margins.right)
    // Bottom inset for the iOS home indicator / Android gesture bar. The
    // 16px floor on Android is the old hardcoded value, kept because Qt
    // there does not draw edge-to-edge and so reports 0: dropping it would
    // change how the shipping Android build looks for no reason.
    readonly property int bottomInset:
        Math.max(Math.ceil(safeAreaProbe.SafeArea.margins.bottom),
                 Qt.platform.os === "android" ? 16 : 0)

    // ── Software keyboard avoidance ──────────────────────────────
    //
    // The comment here used to promise "a Binding below" that pushed the
    // layout up. There was no Binding, and nothing read keyboardHeight —
    // the whole strategy was Android's `adjustResize`
    // (android/AndroidManifest.xml), which shrinks the window so the
    // layout gets out of the keyboard's way by itself. iOS has no
    // equivalent: the window stays full-screen and the keyboard is simply
    // drawn on top of it, which in a chat app means on top of the
    // composer you are typing into. Hence the explicit push below.
    //
    // Units: QInputMethod::keyboardRectangle is documented as window
    // coordinates, but Android's platform plugin reports device pixels
    // (which is why the old expression divided by devicePixelRatio) while
    // iOS reports UIKit points, i.e. already-window coordinates. Rather
    // than hardcode either assumption, normalise by the one thing that is
    // true on both: a docked software keyboard spans the full width of
    // the window, so width tells us the scale factor. A floating keyboard
    // (Android) reports an empty rectangle, which falls through to 0.
    readonly property int keyboardHeight: {
        if (!Qt.inputMethod.visible) return 0;
        var kr = Qt.inputMethod.keyboardRectangle;
        if (!kr || kr.width <= 0 || kr.height <= 0) return 0;
        var scale = root.width / kr.width;
        // Guard against a keyboard that is not full-width after all
        // (an iPad floating keyboard; we do not ship iPad, but a wrong
        // scale here would shove the composer off-screen).
        if (!(scale > 0.2 && scale < 1.5)) scale = 1;
        return Math.ceil(Math.min(kr.height * scale, root.height * 0.7));
    }

    // How far the layout still has to move after everything else has
    // already moved it. Three terms, all measured, which is why there is
    // no per-platform branch here any more:
    //
    //   windowShrink    the window got shorter — Android's adjustResize
    //                   does this, and on iOS MobileKeyboard does it by
    //                   hand for the same reason.
    //   platformScroll  the iOS plugin translated the whole scene up
    //                   instead. Only happens now if the window resize
    //                   was refused; see src/core/MobileKeyboard.h.
    //   the remainder   is ours, and on a platform that does neither it
    //                   is the whole keyboard.
    //
    // Getting this wrong in both directions is what the device caught:
    // first a push added on top of the platform's scroll, which left the
    // composer a whole keyboard height in the air over a void, and then
    // one frame of the same before the correction landed.
    readonly property int keyboardPush:
        Math.max(0, keyboardHeight
                    - mobileKeyboard.platformScroll
                    - mobileKeyboard.windowShrink)

    // The gap between the bottom of the content and the bottom of the
    // window: the keyboard when it is up, the home indicator / gesture
    // bar when it is not. They are alternatives, never a sum — the
    // keyboard covers the home indicator.
    readonly property int bottomGap:
        Qt.inputMethod.visible ? keyboardPush : bottomInset

    // Deliberately NOT animated. An eased push looks nicer in isolation
    // and is actively harmful here: the platform measures where the
    // cursor is at the moment it is asked, so a gap that is still
    // travelling reads as a gap that is not there yet, and the plugin
    // scrolls the scene to compensate for a push that was about to
    // happen anyway. Instant also matches Android, where the window
    // resize has never been animated.
    onBottomGapChanged: keyboardSettle.kick()

    // The platform recomputes its scroll from whatever it sees when
    // asked, so asking once — before the QML layout pass, before the
    // scene graph sync that republishes the cursor rectangle, and long
    // before the keyboard has finished its own ~250ms slide — proves
    // nothing. Ask across that whole window instead and let it converge.
    Timer {
        id: keyboardSettle
        interval: 50
        repeat: true
        property int ticksLeft: 0
        function kick() { ticksLeft = 8; restart(); }
        onTriggered: {
            mobileKeyboard.settle(root._keyboardState());
            if (--ticksLeft <= 0) stop();
        }
    }

    // What gets logged next to the platform's own numbers under
    // `bsfchat.mobile.keyboard`. This is the only place the real values
    // can be seen: the keyboard rectangle's units differ per platform and
    // the insets belong to the device, so neither can be checked by
    // reading this file.
    function _keyboardState() {
        var kr = Qt.inputMethod.keyboardRectangle;
        var cr = Qt.inputMethod.cursorRectangle;
        return {
            "imVisible":  Qt.inputMethod.visible,
            // Position is NOT trustworthy — the platform converts this
            // rectangle through its own translated layer, so the y moves
            // with the scroll. Logged anyway precisely so that stays
            // visible. Only the size is load-bearing.
            "kbRect":     Math.round(kr.x) + "," + Math.round(kr.y)
                          + "," + Math.round(kr.width) + "x" + Math.round(kr.height),
            "kbScale":    (kr.width > 0 ? (root.width / kr.width).toFixed(3) : "n/a"),
            "kbHeight":   root.keyboardHeight,
            // The actual input to the platform's own decision, in Qt
            // window coordinates. If this is above the keyboard's true
            // top and it still scrolls, the containment test is comparing
            // spaces that do not line up.
            "curRect":    Math.round(cr.x) + "," + Math.round(cr.y)
                          + "," + Math.round(cr.width) + "x" + Math.round(cr.height),
            "push":       root.keyboardPush,
            "bottomGap":  root.bottomGap,
            "insetsTRBL": root.topInset + "/" + root.rightInset + "/"
                          + root.bottomInset + "/" + root.leftInset,
            "window":     Math.round(root.width) + "x" + Math.round(root.height),
            // Where the window sits on the screen, which is the offset
            // between the two coordinate spaces above.
            "winOrigin":  Math.round(root.x) + "," + Math.round(root.y),
            "screen":     Math.round(Screen.width) + "x" + Math.round(Screen.height),
            "dpr":        Screen.devicePixelRatio
        };
    }

    // Android hardware-back should cascade through the UI: close the
    // nearest drawer/popup, not blow past everything and quit the
    // app. Qt 6 fires Keys.onBackPressed on ApplicationWindow for
    // the Android hardware-back gesture. We intercept and dispatch.
    onClosing: (close) => {
        // Cascade: thread panel → right drawer → left drawer → modal
        // popups → flip out of voice-room view → default back-to-OS.
        // We don't leave the voice channel on back — the user can
        // keep listening while reading a text channel or another
        // app foreground — only the full-screen voice VIEW closes.
        if (threadPanelOpen()) {
            close.accepted = false;
            closeThread();
        } else if (rightDrawer.opened) {
            close.accepted = false;
            rightDrawer.close();
        } else if (leftDrawer.opened) {
            close.accepted = false;
            leftDrawer.close();
        } else if (searchPopupGlobal.opened) {
            close.accepted = false;
            searchPopupGlobal.close();
        } else if (serverManager.viewingDms
                   && (!serverManager.activeServer
                       || !serverManager.activeServer.activeRoomId)) {
            // DM view with no DM selected → back to normal
            // channel view; matches the "back unwinds DM mode"
            // expectation when there's no active conversation.
            close.accepted = false;
            serverManager.setViewingDms(false);
        } else if (reportDialogGlobal.opened) {
            close.accepted = false;
            reportDialogGlobal.close();
        } else if (deleteAccountGlobal.opened) {
            // Back cancels it, which also abandons any UIA challenge — see
            // DeleteAccountDialog.onClosed. Nothing is deleted by leaving.
            close.accepted = false;
            deleteAccountGlobal.close();
        } else if (blockedUsersGlobal.opened) {
            close.accepted = false;
            blockedUsersGlobal.close();
        } else if (clientSettingsGlobal.opened) {
            close.accepted = false;
            clientSettingsGlobal.close();
        } else if (userSettingsGlobal.opened) {
            close.accepted = false;
            userSettingsGlobal.close();
        } else if (serverManager.activeServer
                   && serverManager.activeServer.viewingVoiceRoom) {
            close.accepted = false;
            serverManager.activeServer.setActiveRoom(
                serverManager.activeServer.activeRoomId);
        }
    }
    // ThreadPanel doesn't have a global id to poke — use a helper.
    function threadPanelOpen() {
        return chatView && chatView.threadPanelOpen
            ? chatView.threadPanelOpen() : false;
    }
    function closeThread() {
        if (chatView && chatView.closeThread) chatView.closeThread();
    }

    // Reactive emptiness check — `servers.rowCount` is a function on
    // QAbstractListModel, not a property, so comparing directly
    // always reads as truthy. Poll via an invocation bound to
    // ListView.count on a hidden Instantiator so it updates live.
    property int _serverCount: 0
    Instantiator {
        active: serverManager && serverManager.servers
        model: serverManager ? serverManager.servers : null
        delegate: QtObject {}
        onObjectAdded: root._serverCount = count
        onObjectRemoved: root._serverCount = count
        onModelChanged: root._serverCount = model ? model.rowCount() : 0
    }
    readonly property bool _noServers: _serverCount === 0

    // Every global popup positions itself against THIS item rather than
    // against Overlay.overlay directly. A Popup's `parent` is only its
    // positioning frame — the popup item itself still lives in the
    // overlay, so the modal dim still covers the whole window — which
    // means shrinking the frame is all it takes for `anchors.centerIn:
    // parent` (LoginDialog, SearchPopup, ReportDialog, …) to centre in
    // what the user can actually see — above the keyboard and inside the
    // notch — without every one of those files growing its own copy of
    // the arithmetic. On Android the window resize does most of this
    // already; on iOS nothing did, so the sign-in dialog, which is the
    // first screen a reviewer sees, centred its password field behind
    // the keyboard.
    Item {
        id: popupSurface
        parent: Overlay.overlay
        anchors.fill: parent
        anchors.topMargin: root.topInset
        anchors.leftMargin: root.leftInset
        anchors.rightMargin: root.rightInset
        anchors.bottomMargin: root.bottomGap
    }

    // Toast host for every subsystem — reachable via Window.window.toast().
    ToastHost { id: toastHostGlobal; parent: popupSurface }
    // ToastHost's API is toast()/info()/success()/warn()/error(). This
    // called a `show()` that has never existed, so EVERY toast on mobile
    // threw a TypeError and nothing was ever shown (U-C2). Kept
    // name-for-name in step with main.qml:471-475 so a call site written
    // against one shell works in the other.
    function toast(t, kind)  { toastHostGlobal.toast(t, kind || "info"); }
    function toastInfo(t)    { toastHostGlobal.info(t); }
    function toastSuccess(t) { toastHostGlobal.success(t); }
    function toastWarn(t)    { toastHostGlobal.warn(t); }
    function toastError(t)   { toastHostGlobal.error(t); }

    // ── Top bar ──────────────────────────────────────────────────
    // Minimal: channel name + burger (drawers). Title taps open the
    // channel list drawer; the avatar on the right opens the member
    // list drawer. The bar's background extends up behind the status
    // bar / notch (height includes topInset) while its CONTENT is
    // inset by it, so the cutout sits on Theme.bg1 rather than on the
    // wallpaper-coloured window background. Left/right insets matter
    // in landscape, where the notch eats one side of the screen.
    header: Rectangle {
        color: Theme.bg1
        height: 48 + root.topInset

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width; height: 1; color: Theme.line
        }

        RowLayout {
            anchors.fill: parent
            anchors.topMargin: root.topInset
            // Theme.mobileGutter, not s4 — the header, the timeline and
            // the composer are all full-width surfaces on a phone and all
            // three now keep the same margin clear of the display's
            // rounded corners. See the token's comment for the arithmetic.
            // The insets are additive rather than alternative: the gutter
            // clears the corner radius, the inset clears a landscape
            // notch, and a device can present both.
            anchors.leftMargin: Theme.mobileGutter + root.leftInset
            anchors.rightMargin: Theme.mobileGutter + root.rightInset
            spacing: Theme.sp.s3

            // Burger → left drawer
            Rectangle {
                Layout.preferredWidth: Theme.touchTarget
                Layout.preferredHeight: Theme.touchTarget
                radius: Theme.r1
                color: burgerMouse.pressed ? Theme.bg3 : "transparent"
                // TalkBack / VoiceOver read this as "Channels, button".
                // Role=Button + name + clickable onPressAction gives
                // screen-reader users a usable navigation target.
                Accessible.role: Accessible.Button
                Accessible.name: "Channels"
                Accessible.description: "Open the server and channel drawer"
                Accessible.onPressAction: leftDrawer.open()
                Icon { anchors.centerIn: parent; name: "menu"; size: 20; color: Theme.fg0 }
                MouseArea {
                    id: burgerMouse
                    anchors.fill: parent
                    onClicked: leftDrawer.open()
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Text {
                    text: {
                        var s = serverManager.activeServer;
                        // In-DM always takes priority — the title
                        // shows the peer you're talking to.
                        if (s && s.activeRoomId
                            && s.isDirectRoom && s.isDirectRoom(s.activeRoomId)) {
                            return "@" + s.directRoomPeer(s.activeRoomId);
                        }
                        // DM view with nothing selected yet — show
                        // the section title so the user knows why
                        // they don't see channels.
                        if (serverManager.viewingDms) return "Direct Messages";
                        if (!s || !s.activeRoomId) return "BSFChat";
                        var n = s.roomListModel
                            ? s.roomListModel.roomDisplayName(s.activeRoomId)
                            : s.activeRoomId;
                        return "#" + n;
                    }
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.lg
                    font.weight: Theme.fontWeight.semibold
                    color: Theme.fg0
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                Text {
                    text: serverManager.activeServer
                        ? serverManager.activeServer.serverName : ""
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xs
                    color: Theme.fg3
                    visible: text.length > 0
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
            }

            Rectangle {
                Layout.preferredWidth: Theme.touchTarget
                Layout.preferredHeight: Theme.touchTarget
                radius: Theme.r1
                color: membersMouse.pressed ? Theme.bg3 : "transparent"
                Accessible.role: Accessible.Button
                Accessible.name: "Members"
                Accessible.description: "Open the member list"
                Accessible.onPressAction: rightDrawer.open()
                Icon { anchors.centerIn: parent; name: "users"; size: 20; color: Theme.fg0 }
                MouseArea {
                    id: membersMouse
                    anchors.fill: parent
                    onClicked: rightDrawer.open()
                }
            }

            Rectangle {
                Layout.preferredWidth: Theme.touchTarget
                Layout.preferredHeight: Theme.touchTarget
                radius: Theme.r1
                color: overflowMouse.pressed ? Theme.bg3 : "transparent"
                Icon {
                    anchors.centerIn: parent
                    name: "more-horizontal"
                    size: 20
                    color: Theme.fg0
                }
                MouseArea {
                    id: overflowMouse
                    anchors.fill: parent
                    onClicked: overflowMenu.popup(parent, parent.width - 200, parent.height)
                }

                // Overflow menu: settings, search, sign out. Adds back
                // the entry points that the desktop version scatters
                // across chat-header buttons + footer gear — none of
                // which are visible on mobile.
                Menu {
                    id: overflowMenu
                    background: Rectangle {
                        color: Theme.bg1
                        radius: Theme.r2
                        border.color: Theme.line
                        border.width: 1
                        implicitWidth: 200
                    }

                    component OverflowItem: MenuItem {
                        id: omi
                        implicitHeight: 40
                        property string iconName: ""
                        contentItem: RowLayout {
                            spacing: Theme.sp.s3
                            Icon {
                                name: omi.iconName
                                size: 14
                                color: omi.hovered ? Theme.fg0 : Theme.fg2
                                Layout.leftMargin: Theme.sp.s3
                            }
                            Text {
                                text: omi.text
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.md
                                color: Theme.fg0
                                Layout.fillWidth: true
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                        background: Rectangle {
                            color: omi.hovered ? Theme.bg2 : "transparent"
                            radius: Theme.r1
                        }
                    }

                    OverflowItem {
                        text: {
                            var s = serverManager.activeServer;
                            if (!s) return "Set status…";
                            var msg = s.selfStatusMessage();
                            return msg && msg.length > 0
                                ? "Status: " + msg : "Set status…";
                        }
                        iconName: "smile"
                        onTriggered: root.openStatusPicker()
                    }
                    OverflowItem {
                        text: "Direct messages"
                        iconName: "at"
                        onTriggered: root.openDirectMessages()
                    }
                    OverflowItem {
                        text: "Search messages"
                        iconName: "search"
                        onTriggered: root.openSearch()
                    }
                    // The desktop chat header carries a pin button; that
                    // whole header is hidden on mobile, which left the
                    // pinned list — and the only unpin control anywhere in
                    // the app — with no way in on a phone.
                    OverflowItem {
                        text: "Pinned messages"
                        iconName: "pin"
                        enabled: serverManager.activeServer !== null
                              && serverManager.activeServer.activeRoomId !== ""
                        onTriggered: root.openPinnedMessages()
                    }
                    OverflowItem {
                        text: "Client settings"
                        iconName: "settings"
                        onTriggered: root.openClientSettings()
                    }
                    OverflowItem {
                        text: "Your profile"
                        iconName: "at"
                        onTriggered: root.openUserSettings()
                    }

                    MenuSeparator { }

                    // Switch server / add another account. Opens the
                    // same LoginDialog used on first launch; users
                    // can pick an existing server from the list or
                    // add a new one without having to sign out first.
                    OverflowItem {
                        text: "Switch server…"
                        iconName: "forward"
                        onTriggered: root.openLoginDialog()
                    }

                    // The same act once you already have a server: someone
                    // hands you an address for a second one. Its own item
                    // rather than a step inside "Switch server…" — on a
                    // phone every step inside a dialog is a step somebody
                    // does not find.
                    OverflowItem {
                        text: "Join a server by address…"
                        iconName: "plus"
                        onTriggered: root.openJoinByAddress()
                    }

                    // Sign out of the active server. Preserves saved
                    // credentials for other servers — we remove just
                    // the current one.
                    OverflowItem {
                        text: "Sign out"
                        iconName: "phone-off"
                        enabled: serverManager.activeServerIndex >= 0
                        onTriggered: {
                            var idx = serverManager.activeServerIndex;
                            if (idx < 0) return;
                            serverManager.removeServer(idx);
                        }
                    }
                }
            }
        }
    }

    // ── Main content ─────────────────────────────────────────────
    // MessageView fills the screen; on no-active-channel, an empty
    // state promotes the user to open the drawer. When the user is
    // connected to a voice room and has flipped to the voice view
    // (via tapping the voice channel, VoiceStatusCard, or VoiceDock),
    // the voice surface takes over the main column.
    Rectangle {
        id: mainArea
        anchors.fill: parent
        // This is where the keyboard push lands: shrinking the content
        // area from the bottom lifts the composer (and the thread
        // composer, and the VoiceDock) clear of the keyboard, rather than
        // leaving Qt's iOS fallback to scroll the entire root view — which
        // takes the header off the top of the screen with it.
        anchors.bottomMargin: root.bottomGap
        anchors.leftMargin: root.leftInset
        anchors.rightMargin: root.rightInset
        color: Theme.bg0

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: (serverManager.activeServer
                               && serverManager.activeServer.viewingVoiceRoom) ? 1 : 0

                MessageView {
                    id: chatView
                    visible: serverManager.activeServer
                          && serverManager.activeServer.activeRoomId !== ""
                }
                VoiceRoom { id: voiceRoomView }
            }

            // Persistent VoiceDock — only rendered while connected to
            // a voice channel. Gives the user a one-tap mute/deafen
            // target and a way to flip back to the chat view.
            VoiceDock {
                Layout.fillWidth: true
                visible: serverManager.activeServer
                      && serverManager.activeServer.inVoiceChannel
            }
        }

        ColumnLayout {
            anchors.centerIn: parent
            visible: !chatView.visible
            spacing: Theme.sp.s5
            Icon {
                name: _noServers ? "plus" : "hash"
                size: 48
                color: Theme.fg3
                Layout.alignment: Qt.AlignHCenter
            }
            Text {
                text: _noServers
                    ? "No servers yet"
                    : "No channel selected"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xl
                font.weight: Theme.fontWeight.semibold
                color: Theme.fg1
                Layout.alignment: Qt.AlignHCenter
            }
            Text {
                text: _noServers
                    ? "Sign in with your BSFChat ID, or join a server by the "
                      + "address whoever runs it gave you."
                    : "Tap the menu button to pick a server and channel."
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.md
                color: Theme.fg3
                Layout.alignment: Qt.AlignHCenter
                horizontalAlignment: Text.AlignHCenter
                Layout.preferredWidth: Math.min(parent.width * 0.8, 320)
                wrapMode: Text.WordWrap
            }
            // Re-open the login dialog — first-launch opens it via
            // Component.onCompleted but if it closes we need a way
            // back in that doesn't require a force-quit.
            //
            // TWO buttons, not one behind the other. This screen is what a
            // store reviewer sees on a fresh install if they dismiss the
            // dialog, and "join a server by address" was, until now,
            // reachable only through a text link inside it. It is the
            // product's central act on a self-hosted chat system; it gets
            // a button.
            Button {
                id: signInCta
                visible: _noServers
                text: "Sign in with BSFChat ID"
                Layout.alignment: Qt.AlignHCenter
                onClicked: root.openLoginDialog()
                contentItem: Text {
                    text: parent.text
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    font.weight: Theme.fontWeight.semibold
                    color: Theme.onAccent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: signInCta.hovered ? Theme.accentDim : Theme.accent
                    radius: Theme.r2
                    implicitWidth: 240
                    implicitHeight: Theme.touchTarget
                }
            }
            Button {
                id: emptyJoinByAddressCta
                visible: _noServers
                text: "Join a server by address"
                Layout.alignment: Qt.AlignHCenter
                onClicked: root.openJoinByAddress()
                contentItem: Text {
                    text: parent.text
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    font.weight: Theme.fontWeight.semibold
                    color: Theme.fg0
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: emptyJoinByAddressCta.hovered ? Theme.bg3 : "transparent"
                    border.color: Theme.accent
                    border.width: 1
                    radius: Theme.r2
                    implicitWidth: 240
                    implicitHeight: Theme.touchTarget
                }
            }
        }
    }

    // ── Left drawer: servers + channels ──────────────────────────
    Drawer {
        id: leftDrawer
        width: Math.min(root.width * 0.85, 340)
        height: root.height
        edge: Qt.LeftEdge
        // Swipe-from-edge gesture area — Qt's Drawer defaults to a
        // 20-px hot edge which feels natural.

        // A drawer covers the whole window height, notch included, so it
        // carries its own safe-area padding: without it the first server
        // tile in the rail sits under the Dynamic Island and the last
        // channel row under the home indicator. Padding rather than
        // margins so the background still paints edge to edge.
        topPadding: root.topInset
        bottomPadding: root.bottomInset
        leftPadding: root.leftInset
        rightPadding: 0

        background: Rectangle { color: Theme.bg1 }

        RowLayout {
            anchors.fill: parent
            spacing: 0

            // Server rail — reused as-is (72px wide).
            ServerSidebar {
                Layout.preferredWidth: Theme.layout.serverRailW
                Layout.fillHeight: true
            }

            // Channel list — reused as-is.
            ChannelList {
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
        }

        // Auto-close after the user picks a channel so they drop
        // straight into the chat. Binding-as-trigger: cache the
        // last seen activeRoomId and close when it changes. More
        // reliable than Connections{} which wasn't firing here —
        // likely because `serverManager.activeServer` is a
        // property whose changes re-target the Connections without
        // re-wiring activeRoomIdChanged.
        property string _lastRoom: ""
        readonly property string _currentRoom: serverManager.activeServer
            ? serverManager.activeServer.activeRoomId : ""
        on_CurrentRoomChanged: {
            if (_currentRoom !== "" && _currentRoom !== _lastRoom) {
                _lastRoom = _currentRoom;
                leftDrawer.close();
            }
        }
    }

    // ── Right drawer: member list ────────────────────────────────
    Drawer {
        id: rightDrawer
        width: Math.min(root.width * 0.75, 280)
        height: root.height
        edge: Qt.RightEdge
        topPadding: root.topInset
        bottomPadding: root.bottomInset
        leftPadding: 0
        rightPadding: root.rightInset
        background: Rectangle { color: Theme.bg1 }

        MemberList { anchors.fill: parent }
    }

    // Global popups — reachable via Window.window.openXyz() helpers.
    UserSettings   { id: userSettingsGlobal;   parent: popupSurface }
    ClientSettings { id: clientSettingsGlobal; parent: popupSurface }
    RoleAssignPopup { id: roleAssignGlobal;    parent: popupSurface }
    // Member-facing self-assignable role picker. Mirrored from main.qml for
    // the reason the safety surfaces below are: ChannelList's "Your Roles"
    // item is shared source, it calls Window.window.openSelfRoles(), and a
    // helper that exists on one shell only is a TypeError on the other.
    // Sized exactly like RoleAssignPopup above, which already works here.
    SelfRolePicker { id: selfRolePickerGlobal; parent: popupSurface }
    SearchPopup {
        id: searchPopupGlobal
        parent: popupSurface
        onResultActivated: (roomId, eventId) => {
            // Switches channel first — a server-side hit is usually not in the
            // room currently on screen.
            if (serverManager.activeServer)
                serverManager.activeServer.jumpToRoomEvent(roomId, eventId);
        }
    }
    StatusPicker {
        id: statusPickerGlobal
        parent: popupSurface
    }

    // ── Safety surfaces: block, report, delete account ──
    //
    // Mirrored from main.qml, name for name. These are the App Store gates
    // (guideline 1.2 user-generated content, 5.1.1(v) account deletion) and
    // this is the shell the stores actually review, so a helper that exists
    // only on the desktop side would be a TypeError exactly where it matters
    // most (U-C2).
    ReportDialog        { id: reportDialogGlobal; parent: popupSurface }
    BlockedUsersDialog  { id: blockedUsersGlobal; parent: popupSurface }
    DeleteAccountDialog { id: deleteAccountGlobal; parent: popupSurface }

    function openUserSettings()   { userSettingsGlobal.open(); }
    function openSelfRoles()      { selfRolePickerGlobal.openPicker(); }
    // "Add a server" — the empty state of a fresh install, so on a phone this
    // is the only way into the app at all. Name-for-name with main.qml.
    function openLoginDialog()    { loginDialogGlobal.open(); }
    // The product's central act on a phone: somebody gives you an address,
    // you join their server. Name-for-name with main.qml.
    function openJoinByAddress() { loginDialogGlobal.openAtAddress(); }
    function openClientSettings() { clientSettingsGlobal.open(); }
    function openSearch()         { searchPopupGlobal.open(); }
    function openStatusPicker()   { statusPickerGlobal.open(); }
    // Drawer + sidebar need to be open for the DM list to be
    // visible; flipping `viewingDms` alone would leave the user
    // staring at "No channel selected" with no obvious next step.
    function openDirectMessages() {
        serverManager.setViewingDms(true);
        leftDrawer.open();
    }
    function openShortcutsDialog() { /* no-op on mobile */ }
    // Forwarded to the chat view, which owns the popover — same shape as
    // threadPanelOpen()/closeThread() above.
    function openPinnedMessages() {
        if (chatView && chatView.openPinnedMessages) chatView.openPinnedMessages();
    }
    function openRoleAssignment(userId, displayName) {
        roleAssignGlobal.openFor(userId, displayName);
    }
    // `kind` is "message" or "user". Kept name-for-name with main.qml so a
    // call site written against one shell works in the other.
    function openReportDialog(kind, userId, displayName, roomId, eventId, preview) {
        reportDialogGlobal.openFor(kind, userId, displayName, roomId, eventId, preview);
    }
    function openBlockedUsers() { blockedUsersGlobal.open(); }
    function openDeleteAccount() { deleteAccountGlobal.open(); }

    // showMemberList on mobile is always "the right drawer"; shim
    // the desktop-level property for components that peek at it.
    //
    // It is a BINDING, so it is read-only from the outside: assigning to
    // it (which MessageView's header button used to do) replaces the
    // binding with a static value and the drawer stops tracking it
    // forever after (U-M12). toggleMemberList drives the drawer itself.
    property bool showMemberList: rightDrawer.opened
    function toggleMemberList() {
        if (rightDrawer.opened) rightDrawer.close(); else rightDrawer.open();
    }

    // Login dialog when not authenticated to any server. Mobile builds
    // hit the same LoginDialog; the OIDC flow behind it is mobile-specific
    // in C++ (IdentityClient): iOS presents ASWebAuthenticationSession,
    // Android opens the browser and gets the redirect back through an
    // intent-filter. There is no WebView anywhere in this app — an earlier
    // version of this comment claimed one, and said the flow worked, at a
    // time when it could not complete on either platform.
    LoginDialog {
        id: loginDialogGlobal
        parent: popupSurface
    }
    Component.onCompleted: {
        if (!serverManager || !serverManager.servers
            || serverManager.servers.rowCount() === 0) {
            loginDialogGlobal.open();
        } else {
            _maybeAutoSelect();
        }
    }

    // Drop the user into a channel the moment we have one. The rule
    // (remembered channel for THIS server → first text channel → wait,
    // because sync may not have delivered the list yet) lives in
    // ServerConnection::restoreLastTextRoom, shared with the desktop
    // shell. Voice rooms are never restored: auto-rejoining voice would
    // push the user's mic onto the network the instant the app opens.
    function _maybeAutoSelect() {
        var s = serverManager ? serverManager.activeServer : null;
        if (!s) {
            if (serverManager && serverManager.activeServerIndex < 0
                && serverManager.servers
                && serverManager.servers.rowCount() > 0) {
                serverManager.setActiveServer(0);
                Qt.callLater(_maybeAutoSelect);
            }
            return;
        }
        s.restoreLastTextRoom();
    }

    // Component.onCompleted can beat the connection coming up; the
    // sync-side retry inside restoreLastTextRoom covers the rest.
    Connections {
        target: serverManager ? serverManager.activeServer : null
        ignoreUnknownSignals: true
        function onConnectedChanged() { _maybeAutoSelect(); }
    }

    // A sign-in that fails AFTER the login dialog has gone has to say so
    // somewhere, and on this shell it had nowhere to say it.
    //
    // The identity-first flow closes the dialog on identityLoginComplete —
    // the account sign-in worked and the servers are populating — and only
    // then runs the per-server BSFChat ID sign-in that produces the
    // audience-bound id_token. When THAT failed, ServerManager emitted
    // loginError and removed the connection; LoginDialog's handler set an
    // errorMessage on a dialog nobody was looking at, and the desktop shell's
    // toast fallback (main.qml) has no counterpart here. So the sidebar
    // emptied itself and the app said nothing at all — which is what the
    // owner saw on 2026-09-24 while the browser held a 403 he never got back
    // to. Mirrors main.qml's block, including the "only when the dialog is
    // not up" rule that keeps one failure to one message (D-H5).
    Connections {
        target: serverManager
        ignoreUnknownSignals: true
        function onLoginError(serverUrl, error) {
            if (!loginDialogGlobal.opened) root.toastError("Couldn't sign in: " + error);
        }
        function onIdentityLoginFailed(error) {
            if (!loginDialogGlobal.opened) root.toastError("BSFChat ID sign-in failed: " + error);
        }
        function onReauthFailed(index, serverUrl, error) {
            root.toastError("Couldn't sign in: " + error);
        }
        // Signing in with a BSFChat ID takes TWO browser trips, and the
        // second one is a surprise: the first proves who you are and fetches
        // your servers, the second is the one that produces the token for a
        // particular server — an id_token is audience-bound to exactly one
        // (identity/OidcRequest.h), so there is no way to fold them into one
        // trip. The dialog closes between them, so from the phone's side the
        // app simply threw the user back into the browser with no
        // explanation. Say what is happening before it does.
        function onIdentityLoginComplete(serverUrls) {
            if (serverUrls && serverUrls.length > 0) {
                root.toastInfo(serverUrls.length === 1
                    ? "Signing in to your server — approve it in your browser."
                    : "Signing in to your " + serverUrls.length
                      + " servers — approve each one in your browser.");
            }
        }
    }

    // Send-side feedback (rate limits, permission errors, …) as
    // toasts. ServerConnection pre-formats the copy + severity.
    Connections {
        target: serverManager ? serverManager.activeServer : null
        ignoreUnknownSignals: true
        function onSendFeedback(text, kind) {
            root.toast(text, kind || "error");
        }
    }

    // Mirrors the desktop shell's handler of the same name (main.qml). A
    // sign-in that cannot reach a browser has to say so on every platform:
    // on Android that is a device with no browser able to take the
    // ACTION_VIEW, which is rare and, unreported, indistinguishable from a
    // dead button. The LoginDialog — the same component both shells use —
    // shows the link inline; this is the surface when it is not up.
    Connections {
        target: serverManager
        ignoreUnknownSignals: true
        function onBrowserOpenFailed(authUrl) {
            serverManager.copyToClipboard(authUrl);
            root.toastError("Couldn't open a browser — the sign-in link is on "
                            + "your clipboard. Paste it into a browser to finish.");
        }
    }

    // (Persisting the active text channel moved into
    // ServerConnection::setActiveRoom — same write, but on the side of
    // the boundary that also owns the restore, and reached by every path
    // that opens a channel rather than only the ones this shell sees.)

    // Android "Share to BSFChat" handler — sends the shared payload
    // to the currently-active channel. Text payloads become normal
    // messages; files go through the media-upload pipeline.
    Connections {
        target: typeof urlHandler !== "undefined" ? urlHandler : null
        // The target is null on every build without the Android share
        // intent, and non-null builds may not carry this signal — either
        // way an unguarded Connections is a hard QML error at load (U-M6).
        ignoreUnknownSignals: true
        function onSharedPayloadReceived(payload, mimeType, isFile) {
            var s = serverManager.activeServer;
            if (!s || !s.activeRoomId || s.activeRoomId.length === 0) {
                // No channel open — stash for the user to retry once
                // they pick one. For now just toast.
                toast("Pick a channel first, then share again.", "info");
                return;
            }
            if (isFile) {
                // Same two lines, in the same order, as every other site that
                // starts a composer upload — see MessageInput.noteUploadStarted.
                // This one used to send without counting, which is the
                // ownership half of U-H6 reached from the producing side: the
                // upload's mediaSendCompleted / mediaSendFailed arrives at the
                // composer's Connections block regardless, so a share while an
                // attachment was uploading unlocked the composer with the
                // attachment still on the wire, and a share with nothing in
                // flight was an unmatched decrement the clamp ate in silence.
                s.sendMediaMessage(payload);
                chatView.noteUploadStarted();
                toast("Uploading shared file…", "info");
            } else {
                s.sendMessage(payload);
                toast("Shared text posted", "success");
            }
        }
    }
}
