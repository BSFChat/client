import QtQuick
import QtQuick.Controls
import BSFChat

// Subtle vertical scrollbar — a thin fg3-tinted stripe that widens on hover
// and brightens when dragged. Designed to read as "there's more below" but
// not as UI chrome. Attach via `ScrollBar.vertical: ThemedScrollBar {}` on
// any Flickable / ScrollView / ListView.
ScrollBar {
    id: bar
    policy: ScrollBar.AsNeeded
    minimumSize: 0.08

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
