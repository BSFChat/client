import QtQuick
import QtQuick.Controls
import BSFChat

// Themed toggle (SPEC §3.10 SettingsToggle: 40×22 pill).
//
// A drop-in replacement for `Switch` that uses the Designer accent palette
// instead of Qt Controls' default chrome. Binding is identical to Switch
// (`checked`, `onToggled`). Size is fixed at 40×22; if you want a bigger
// hit target wrap in an Item and extend its MouseArea.
Switch {
    id: sw

    implicitWidth: 40
    implicitHeight: 22
    padding: 0

    // Default accessible identity. The contentItem below is deliberately
    // an empty Item — settings rows put the label in their own left-hand
    // column — so without this every ThemedSwitch in the app announces as
    // an unnamed control. Plain bindings: a call site that sets
    // `Accessible.name` overrides this one. Role is CheckBox because that
    // is how a two-state control is reported to the platform.
    Accessible.role: Accessible.CheckBox
    Accessible.name: sw.text
    Accessible.checkable: true
    Accessible.checked: sw.checked
    // A pointer click goes through nextCheckState(), which flips
    // `checked` AND emits toggled(). Neither of the obvious shortcuts
    // does both: AbstractButton::toggle() sets checked without emitting
    // toggled(), and Qt's own default accessible handling writes the
    // `checked` property directly (qaccessiblequickitem.cpp, the
    // CheckBox branch of doAction). Either way the call site's
    // `onToggled` never runs, so the setting is not written and the
    // D-C1 re-binding never happens: the switch flips on screen and
    // stops tracking the value for the rest of the session. So do what
    // the click does, in the order the click does it.
    Accessible.onPressAction: {
        sw.checked = !sw.checked;
        sw.toggled();
    }
    Accessible.onToggleAction: {
        sw.checked = !sw.checked;
        sw.toggled();
    }

    indicator: Rectangle {
        implicitWidth: 40
        implicitHeight: 22
        radius: 11
        // Accent when on, bg3 when off. Hover subtly lightens the track
        // so the control feels live before you press it.
        color: sw.checked
               ? (sw.hovered ? Qt.lighter(Theme.accent, 1.08) : Theme.accent)
               : (sw.hovered ? Theme.bg4 : Theme.bg3)
        border.width: sw.checked ? 0 : 1
        border.color: Theme.line
        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

        Rectangle {
            // Thumb — slightly inset so you can see the track colour on
            // either side, animates across on toggle.
            x: sw.checked ? parent.width - width - 2 : 2
            y: 2
            width: 18
            height: 18
            radius: 9
            color: sw.checked ? Theme.onAccent : Theme.fg1
            Behavior on x {
                NumberAnimation {
                    duration: Theme.motion.fastMs
                    easing.type: Easing.BezierSpline
                    easing.bezierCurve: Theme.motion.bezier
                }
            }
            Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
        }
    }

    // Kill the default Qt Controls text element — settings rows put their
    // label on the left column, so the Switch doesn't need to carry text.
    contentItem: Item { visible: false }
}
