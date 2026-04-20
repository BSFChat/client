import QtQuick
import QtQuick.Controls
import BSFChat

// Themed checkbox. 18×18 rounded-square box, `accent` fill with an
// `onAccent`-tinted check-SVG when checked; `bg0` with `line` border
// when unchecked. `bg3` hover tint in the unchecked state. Drop-in
// replacement for Qt Controls CheckBox — `checked` and `onToggled`
// work the same way.
CheckBox {
    id: cb
    // Keep spacing tight between the box and any label text that gets
    // parented to the CheckBox (contentItem), if the caller chooses to
    // use one. Our settings rows generally put the label outside.
    spacing: Theme.sp.s3

    indicator: Rectangle {
        implicitWidth: 18
        implicitHeight: 18
        x: cb.leftPadding
        y: (cb.height - height) / 2
        radius: 5
        color: cb.checked ? Theme.accent
             : cb.hovered ? Theme.bg3
             : Theme.bg0
        border.color: cb.checked ? Theme.accent : Theme.line
        border.width: 1
        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
        Behavior on border.color { ColorAnimation { duration: Theme.motion.fastMs } }

        Icon {
            anchors.centerIn: parent
            name: "check"
            size: 12
            color: Theme.onAccent
            opacity: cb.checked ? 1.0 : 0.0
            scale: cb.checked ? 1.0 : 0.6
            Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }
            Behavior on scale {
                NumberAnimation { duration: Theme.motion.fastMs
                                  easing.type: Easing.BezierSpline
                                  easing.bezierCurve: Theme.motion.bezier }
            }
        }
    }

    contentItem: Text {
        // Inline label (if any). Default to hidden so settings rows can
        // use their own left-column label without double-labelling.
        text: cb.text
        visible: cb.text.length > 0
        font.family: Theme.fontSans
        font.pixelSize: Theme.fontSize.md
        color: Theme.fg0
        verticalAlignment: Text.AlignVCenter
        leftPadding: cb.indicator.width + cb.spacing
    }
}
