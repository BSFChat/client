import QtQuick
import QtQuick.Controls
import BSFChat

// Subtle vertical scrollbar — a thin fg3-tinted stripe that widens on hover
// and brightens when dragged. Designed to read as "there's more below" but
// not as UI chrome. Attach via `ScrollBar.vertical: ThemedScrollBar {}` on
// any Flickable / ScrollView / ListView.
ScrollBar {
    id: bar
    minimumSize: 0.08

    // ScrollBar.size is the scaled thumb size (0..1); it hits 1.0 when the
    // content fully fits the viewport. `AsNeeded` is *meant* to hide the
    // bar in that case but the track still rendered in some Qt builds —
    // gating visibility on size < 1 makes "no scroll → no chrome" honest
    // across platforms. Sub-pixel epsilon avoids a flicker when rounding
    // lands the thumb at exactly 1.0.
    policy: size < 0.999 ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff

    contentItem: Rectangle {
        implicitWidth: bar.hovered || bar.pressed ? 8 : 4
        radius: width / 2
        color: bar.pressed ? Theme.accent
             : bar.hovered ? Theme.fg2
             : Theme.bg4
        opacity: bar.active ? 0.9 : 0.55
        Behavior on implicitWidth {
            NumberAnimation { duration: Theme.motion.fastMs
                              easing.type: Easing.BezierSpline
                              easing.bezierCurve: Theme.motion.bezier }
        }
        Behavior on color   { ColorAnimation { duration: Theme.motion.fastMs } }
        Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }
    }

    background: Rectangle { color: "transparent" }
}
