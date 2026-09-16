import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// App-wide toast surface. One instance lives in main.qml and collects
// transient notifications from every subsystem that signals success /
// failure. Call sites reach it via `Window.window.toast(text, kind)`.
//
// Kind is "info" (default) | "success" | "warn" | "error". Each maps to
// an icon + a tint; the card chrome (bg1 + line border) stays constant
// so the stack reads as one family of alerts.
//
// Toasts stack bottom-right. Newer ones push older ones upward. Each
// auto-dismisses after a duration that scales with severity — info /
// success fade after 3.5s; warn / error hang for 6s so the user has
// time to read. Hovering a toast pauses its dismissal timer; mousing
// away restarts it, so you can read a long one without losing it.
Item {
    id: host
    anchors.fill: parent
    // A bare Item accepts no pointer input of its own, so the stack does
    // not block clicks on the content underneath — only the cards' own
    // MouseAreas do. This used to be `enabled: false`, which ANDed down
    // into every descendant and silently disabled the dismiss button and
    // the hover-to-pause MouseArea (D-H1): neither the X nor hovering
    // did anything, on any platform.
    visible: true

    // Backing store is a ListModel, not a re-assigned JS array (D-M1).
    // With an array, every enqueue and every dismiss handed the Repeater
    // a brand-new model object, so all four cards were destroyed and
    // rebuilt — restarting each surviving toast's dismiss timer from
    // zero and re-running its slide-in animation. With a ListModel the
    // Repeater sees an insert or a remove and leaves the others alone.
    ListModel { id: toastModel }
    property int _nextId: 1

    function toast(text, kind) {
        if (!text || text.length === 0) return;
        var k = kind || "info";
        toastModel.append({
            toastId:   host._nextId++,
            toastText: String(text),
            toastKind: k,
            // Errors + warnings linger; info / success fade sooner.
            toastDuration: (k === "error" || k === "warn") ? 6000 : 3500
        });
        // Cap the visible stack at 4 — older toasts drop off when a 5th
        // arrives, so a burst of failures doesn't wallpaper the screen.
        while (toastModel.count > 4) toastModel.remove(0);
    }

    function _dismiss(id) {
        for (var i = 0; i < toastModel.count; ++i) {
            if (toastModel.get(i).toastId === id) { toastModel.remove(i); return; }
        }
    }

    // Shortcut helpers — match the kind name so call sites read well.
    function info(text)    { toast(text, "info"); }
    function success(text) { toast(text, "success"); }
    function warn(text)    { toast(text, "warn"); }
    function error(text)   { toast(text, "error"); }

    // Stack container, bottom-right. Each child Repeater entry is its
    // own card; newer toasts sit at the bottom of the list and push
    // older ones upward via the natural Column flow.
    Column {
        id: stack
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: Theme.sp.s5
        anchors.bottomMargin: Theme.sp.s5
        spacing: Theme.sp.s3
        width: 360

        Repeater {
            model: toastModel

            delegate: Rectangle {
                id: card
                required property int toastId
                required property string toastText
                required property string toastKind
                required property int toastDuration
                width: stack.width
                implicitHeight: toastRow.implicitHeight + Theme.sp.s4 * 2
                radius: Theme.r2
                color: Theme.bg1
                border.color: tintColor
                border.width: 1
                opacity: 0
                scale: 0.95

                readonly property color tintColor: {
                    switch (card.toastKind) {
                        case "error":   return Theme.danger;
                        case "warn":    return Theme.warn;
                        case "success": return Theme.online;
                        default:        return Theme.accent;
                    }
                }
                readonly property string iconName: {
                    switch (card.toastKind) {
                        case "error":   return "x";
                        case "warn":    return "bolt";
                        case "success": return "check";
                        default:        return "eye";
                    }
                }

                // Slide-in animation — fade + scale, anchored on arrival.
                Component.onCompleted: {
                    opacity = 1.0;
                    scale = 1.0;
                }
                Behavior on opacity { NumberAnimation { duration: 180 } }
                Behavior on scale   { NumberAnimation { duration: 180
                                        easing.type: Easing.OutCubic } }

                // Left accent stripe — matches the InfoBanner pattern.
                Rectangle {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: 3
                    height: parent.height - Theme.sp.s4 * 2
                    color: card.tintColor
                    radius: 1.5
                }

                RowLayout {
                    id: toastRow
                    anchors.fill: parent
                    anchors.leftMargin: Theme.sp.s5
                    anchors.rightMargin: Theme.sp.s3
                    anchors.topMargin: Theme.sp.s4
                    anchors.bottomMargin: Theme.sp.s4
                    spacing: Theme.sp.s3

                    Icon {
                        name: card.iconName
                        size: 16
                        color: card.tintColor
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: 2
                    }
                    Text {
                        Layout.fillWidth: true
                        text: card.toastText
                        color: Theme.fg0
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                        wrapMode: Text.WordWrap
                        maximumLineCount: 4
                        elide: Text.ElideRight
                    }

                    // Dismiss button.
                    Rectangle {
                        Layout.alignment: Qt.AlignTop
                        Layout.preferredWidth: 20
                        Layout.preferredHeight: 20
                        radius: Theme.r1
                        color: dismissMouse.containsMouse
                            ? Qt.rgba(1, 1, 1, 0.08) : "transparent"
                        Icon {
                            anchors.centerIn: parent
                            name: "x"
                            size: 10
                            color: Theme.fg2
                        }
                        MouseArea {
                            id: dismissMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: host._dismiss(card.toastId)
                        }
                    }
                }

                // Hover-to-pause: while the cursor is on the toast, the
                // auto-dismiss timer stops; leaving restarts it with the
                // full original duration so the reading clock is fair.
                MouseArea {
                    id: cardHover
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.NoButton
                    onEntered: dismissTimer.stop()
                    onExited:  dismissTimer.restart()
                }

                Timer {
                    id: dismissTimer
                    interval: card.toastDuration
                    running: true
                    repeat: false
                    onTriggered: host._dismiss(card.toastId)
                }
            }
        }
    }
}
