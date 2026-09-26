import QtQuick
import QtQuick.Controls
import BSFChat

// Themed horizontal slider. 4px `bg3` track with an accent-filled
// progress segment; 14px thumb that scales up on hover / drag. Drop-in
// replacement for Qt Controls Slider.
Slider {
    id: slider
    padding: 0

    // Qt's Slider already reports value / from / to to the platform, so
    // the only missing piece is what the number is ABOUT. Slider carries
    // no `text` of its own to fall back on, so this is a last-resort
    // placeholder: every call site is expected to override
    // `Accessible.name` with the wording of its visible label, which is
    // what the settings panes do.
    Accessible.role: Accessible.Slider
    Accessible.name: qsTr("Slider")
    // Same trap as the toggles: Slider::increase()/decrease() set
    // `value` without emitting moved(), and Qt's default accessible
    // handling writes the value through the value interface, which does
    // not emit it either. Every call site persists the setting in
    // `onMoved` (and restores the D-C1 binding there), so a screen
    // reader stepping the slider would otherwise move the handle and
    // change nothing. Arrow keys already emit moved() themselves.
    Accessible.onIncreaseAction: {
        slider.increase();
        slider.moved();
    }
    Accessible.onDecreaseAction: {
        slider.decrease();
        slider.moved();
    }

    background: Rectangle {
        x: slider.leftPadding
        y: slider.topPadding + slider.availableHeight / 2 - height / 2
        implicitWidth: 200
        implicitHeight: 4
        width: slider.availableWidth
        height: implicitHeight
        radius: 2
        color: Theme.bg3

        // Accent-filled progress. `visualPosition` goes 0..1 left→right
        // regardless of RTL, so this works the same in any locale.
        Rectangle {
            width: slider.visualPosition * parent.width
            height: parent.height
            color: Theme.accent
            radius: 2
        }
    }

    handle: Rectangle {
        x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
        y: slider.topPadding + slider.availableHeight / 2 - height / 2
        implicitWidth: slider.pressed || slider.hovered ? 18 : 14
        implicitHeight: implicitWidth
        radius: implicitWidth / 2
        color: Theme.fg0
        border.color: Theme.accent
        border.width: slider.pressed || slider.hovered ? 3 : 2

        Behavior on implicitWidth {
            NumberAnimation { duration: Theme.motion.fastMs
                              easing.type: Easing.BezierSpline
                              easing.bezierCurve: Theme.motion.bezier }
        }
        Behavior on border.width {
            NumberAnimation { duration: Theme.motion.fastMs }
        }
    }
}
