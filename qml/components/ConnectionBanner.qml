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
