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

    // How much horizontal room a scrolling view has to keep clear so that
    // nothing it lays out ends up underneath this bar.
    //
    // The owner's report: "I can't fully read the values of some settings
    // because they are 'behind' the scrollbar (the values are off to the
    // right)." A ScrollBar attached to a Flickable is always an overlay —
    // Qt gives it no layout box — and every settings pane bound its
    // content column to the FULL Flickable width, so the last ~10px of
    // each row sat under the bar. That is precisely where a SettingRow
    // puts its value.
    //
    // This is a CONSTANT on purpose, not `width` and not
    // `visible ? width : 0`:
    //   - the thumb grows 4 → 8px on hover, so a live binding would
    //     reflow — and re-wrap — the whole page every time the pointer
    //     crossed the bar;
    //   - `policy` below is derived from `size`, which comes from
    //     contentHeight, which for wrapping text depends on the width.
    //     Feeding the bar's visibility back into the content width closes
    //     that loop, and a page sitting one line from overflowing would
    //     oscillate in it.
    // The couple of reserved pixels on a page that doesn't scroll cost
    // nothing; these panes already carry a Theme.sp.s7 * 2 margin.
    //
    // 14 = the 8px hovered thumb + the bar's padding + a little air, so
    // even the widest state never touches the content.
    readonly property real reservedWidth: 14

    // ScrollBar.size is the scaled thumb size (0..1); it hits 1.0 when the
    // content fully fits the viewport. `AsNeeded` is *meant* to hide the
    // bar in that case but the track still rendered in some Qt builds —
    // gating visibility on size < 1 makes "no scroll → no chrome" honest
    // across platforms. Sub-pixel epsilon avoids a flicker when rounding
    // lands the thumb at exactly 1.0.
    policy: size < 0.999 ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff

    // On a touch screen this is an INDICATOR, not a control.
    //
    // Qt's ScrollBar defaults to interactive: true, which means the strip
    // it occupies — the bar plus its padding, ~10 px hard against the
    // right edge of whatever it is attached to — swallows presses and
    // turns them into thumb drags. On a phone that strip lies exactly
    // where a thumb lands when you flick the timeline near the bezel, and
    // exactly where MessageBubble's swipe-to-reply drag starts. The
    // symptom is a flick that doesn't scroll, or scrolls miles, at
    // seemingly random times near the edge of the screen.
    //
    // Neither iOS nor Android has a draggable scrollbar; both draw a
    // passive position indicator over the content and scroll by flicking
    // it. Matching that costs nothing — every scrolling surface in this
    // app is a Flickable, so the content itself is already the handle.
    interactive: !Theme.isMobile

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
