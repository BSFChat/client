import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat
import "../js/ConnectionBanner.js" as Banner

// Connection / sync / re-auth strip — 28h, full width, at the top of the
// shell's main column. Reconnecting = amber warn; disconnected or expired =
// danger red. Small animated dot on the left signals live state.
//
// A LAYOUT ROW, mounted by both shells as a sibling of the main StackLayout
// (qml/main.qml, qml/mobile/MobileMain.qml). It must not be anchored over the
// column: an item over the column composites over live video, which is the
// bug MainSurface.js documents. Being a row, it takes its 28px off whichever
// surface is showing — timeline, voice room, or the mobile empty state — and
// so it is on screen during a call, which is the whole reason it moved out of
// MessageView.qml.
//
// Rules in qml/js/ConnectionBanner.js; see that file for why.
Rectangle {
    id: root

    // Plain snapshot of the four ServerConnection properties the rules read,
    // built here so the bindings' dependencies are these four reads rather
    // than something a shared .js file did out of sight. Same shape and same
    // reasoning as MobileMain's `surfaceView`.
    readonly property var connView: {
        var s = serverManager.activeServer;
        if (!s) return null;
        return {
            connectionStatus: s.connectionStatus,
            syncErrorMessage: s.syncErrorMessage,
            needsReauth:      s.needsReauth,
            reauthInProgress: s.reauthInProgress
        };
    }

    Layout.fillWidth: true
    Layout.preferredHeight: visible ? 28 : 0
    visible: Banner.isShowing(connView)

    // ── Accessibility ────────────────────────────────────────────────
    //
    // This strip is the single surface a screen-reader user most needs and
    // is least able to notice: it appears without being asked for, says
    // something the rest of the UI does not, and is 28px of colour at the
    // top of the window. Two halves, per docs/accessibility.md §8.
    //
    // (a) It is a named node, so it can be found and re-read afterwards.
    //     The whole strip is one announcement — the pulse dot and the
    //     message Text below are both Accessible.ignored, because reading
    //     "dot, Reconnecting…" is worse than reading "Reconnecting…".
    Accessible.role: Accessible.AlertMessage
    Accessible.name: Banner.message(root.connView)

    // (b) It announces itself when it appears, because by the time the user
    //     next sweeps the screen the thing they needed to know is that
    //     messages have not been sending for the last thirty seconds.
    //     Danger (disconnected, expired session) interrupts; warn
    //     (reconnecting) waits its turn.
    //
    //     Both triggers matter and neither subsumes the other: the strip
    //     appearing at all, and the strip changing what it says while it
    //     stays up — reconnecting → disconnected is a new fact, on an item
    //     that never became invisible in between.
    function _announceState() {
        if (!root.visible) return;
        var msg = Banner.message(root.connView);
        if (!msg || msg.length === 0) return;
        root.Accessible.announce(
            msg,
            Banner.tone(root.connView) === Banner.Danger
                ? Accessible.AnnouncementPoliteness.Assertive
                : Accessible.AnnouncementPoliteness.Polite);
    }
    onVisibleChanged: _announceState()
    Accessible.onNameChanged: _announceState()
    color: {
        var t = Banner.tone(connView);
        if (t === Banner.Warn) return Theme.warn;
        if (t === Banner.Danger) return Theme.danger;
        return "transparent";
    }

    RowLayout {
        anchors.centerIn: parent
        spacing: Theme.sp.s3
        // Bounded, so the longest line ("Disconnected — messages won't send
        // until the server is reachable") elides on a phone instead of
        // running out through the rounded corners of the display. The gutter
        // is the one every other full-width surface keeps.
        width: Math.min(implicitWidth, parent.width - Theme.mobileGutter * 2)

        // Pulse dot — loops opacity so the banner reads as "live state, not
        // static warning."
        Rectangle {
            Layout.alignment: Qt.AlignVCenter
            // Layout.preferred*, not width/height: this is a RowLayout child,
            // and a plain width on one is undefined behaviour that qmllint
            // flags (Quick.layout-positioning). It was written that way when
            // the strip lived in MessageView and carried the warning with it.
            Layout.preferredWidth: 6
            Layout.preferredHeight: 6
            radius: 3
            color: Theme.onAccent
            // Ornament. "Live state, not static warning" is a visual idea
            // with no spoken equivalent, and the strip's own name already
            // carries the state.
            Accessible.ignored: true
            SequentialAnimation on opacity {
                loops: Animation.Infinite
                running: root.visible
                NumberAnimation { to: 0.3; duration: 600; easing.type: Easing.InOutQuad }
                NumberAnimation { to: 1.0; duration: 600; easing.type: Easing.InOutQuad }
            }
        }

        Text {
            Layout.alignment: Qt.AlignVCenter
            Layout.fillWidth: true
            text: Banner.message(root.connView)
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.sm
            font.weight: Theme.fontWeight.semibold
            font.letterSpacing: Theme.trackTight.sm
            // onAccent works here because warn/danger are both
            // high-saturation colours that contrast with both the dark-mode
            // near-black and the light-mode white.
            color: Theme.onAccent
            // Already spoken as the strip's Accessible.name. Reading it
            // again as its own node would announce every banner twice.
            Accessible.ignored: true
        }

        // The way out. Until this existed the banner named the problem
        // ("Sign in again to reconnect") and offered nothing to press: the
        // only affordance in the product was the server rail's Reconnect,
        // which redialled /sync with the same dead token and never reached
        // /login. Recovery meant removing the server and adding it back.
        //
        // And until the banner itself moved out here it was unreachable from
        // the voice room on either shell — the rail's item is not on a phone
        // at all, so on mobile there was no way out of an expired session
        // during a call.
        Button {
            id: reauthButton
            Layout.alignment: Qt.AlignVCenter
            visible: Banner.showsReauth(root.connView)
            enabled: Banner.reauthEnabled(root.connView)
            text: Banner.reauthLabel(root.connView)
            padding: 0
            background: null
            // The only way out of an expired session on a phone during a
            // call. It has to be reachable by name, not by knowing there is
            // something pressable at the right-hand end of a coloured strip.
            Accessible.role: Accessible.Button
            Accessible.name: reauthButton.text
            Accessible.description: qsTr("Sign in again to restore this connection")
            Accessible.onPressAction: if (reauthButton.enabled) reauthButton.clicked()
            // A phone needs a thumb-sized target; the strip is 28h, so the
            // width is what there is to give.
            implicitHeight: Math.max(implicitContentHeight, 28)
            contentItem: Text {
                text: reauthButton.text
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                font.weight: Theme.fontWeight.semibold
                font.underline: reauthButton.enabled && reauthButton.hovered
                color: Theme.onAccent
                opacity: reauthButton.enabled ? 1.0 : 0.6
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: serverManager.reauthenticateServer(
                           serverManager.activeServerIndex)
        }
    }

    Behavior on Layout.preferredHeight {
        NumberAnimation { duration: Theme.motion.normalMs
                          easing.type: Easing.BezierSpline
                          easing.bezierCurve: Theme.motion.bezier }
    }
}
