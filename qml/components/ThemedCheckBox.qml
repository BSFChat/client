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

    // Default accessible identity. The contentItem below is hidden when
    // `text` is empty, which is the normal case in settings rows (the
    // label lives in the row's left-hand column), so without this every
    // ThemedCheckBox announces as an unnamed control. Plain bindings: a
    // call site that sets `Accessible.name` simply overrides this one.
    Accessible.role: Accessible.CheckBox
    Accessible.name: cb.text
    Accessible.checkable: true
    Accessible.checked: cb.checked
    // Flip AND emit, in that order, because that is what a pointer
    // click does (nextCheckState) and what every call site's
    // `onToggled` handler is written against. AbstractButton::toggle()
    // sets `checked` without emitting toggled(), and Qt's default
    // accessible handling writes the property directly — both leave the
    // caller's handler unrun. See the longer note in ThemedSwitch.qml.
    Accessible.onPressAction: {
        cb.checked = !cb.checked;
        cb.toggled();
    }
    Accessible.onToggleAction: {
        cb.checked = !cb.checked;
        cb.toggled();
    }

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
        // The root already announces this string as its name; leaving
        // the label reachable too would read it twice.
        Accessible.ignored: true
        font.family: Theme.fontSans
        font.pixelSize: Theme.fontSize.md
        color: Theme.fg0
        verticalAlignment: Text.AlignVCenter
        leftPadding: cb.indicator.width + cb.spacing
    }
}
