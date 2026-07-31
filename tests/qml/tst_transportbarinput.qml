import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtTest
import "../../qml/js/PlaybackMath.js" as PlaybackMath

// Does the input structure VideoPlayerCard.qml uses for its control bar
// actually behave the way the fix assumes?
//
// READ THIS BEFORE TRUSTING IT: the scene below is a REPLICA of that
// structure, not the component itself — VideoPlayerCard imports the BSFChat
// module, which is compiled into the application binary and cannot be loaded
// by a test executable. So this proves the Qt behaviour the fix depends on:
//
//   1. a click on the bar's bare chrome (margins, inter-control gaps, the
//      strip above/below a short button) is absorbed by the bar and does NOT
//      reach the full-bleed MouseArea underneath — the bug that made a
//      mis-aimed scrub collapse the fullscreen video;
//   2. a click on the picture DOES reach that MouseArea;
//   3. a stationary click on the seek track seeks, which a bare Slider does
//      not do at all; and
//   4. adding that click-to-seek does not cost us dragging the handle.
//
// It cannot prove the shipped component is wired this way. If someone
// restructures the bar in VideoPlayerCard.qml, this test keeps passing and the
// bug comes back — the guard for that is the comment at the top of that file.
TestCase {
    id: tc
    name: "TransportBarInput"
    when: windowShown
    width: 400
    height: 200
    visible: true

    readonly property int durationMs: 60000

    Item {
        id: surface
        anchors.fill: parent

        property int surfaceClicks: 0
        property int seekMs: -1

        // The whole-picture click target: play/pause in the real component,
        // a counter here.
        MouseArea {
            id: surfaceMouse
            anchors.fill: parent
            z: -1
            onClicked: surface.surfaceClicks++
        }

        Rectangle {
            id: bar
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 56
            color: "#aa000000"

            // The event boundary under test.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
                onPressed: (m) => { m.accepted = true; }
                onClicked: (m) => { m.accepted = true; }
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 20
                spacing: 16

                // Stand-in for the play/pause button: shorter than the bar,
                // which is where the strips of bare chrome come from.
                Rectangle {
                    id: fakeButton
                    Layout.preferredWidth: 36
                    Layout.preferredHeight: 36
                    Layout.alignment: Qt.AlignVCenter
                    color: "#333"
                }

                Slider {
                    id: slider
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    from: 0
                    to: tc.durationMs
                    focusPolicy: Qt.NoFocus
                    onMoved: surface.seekMs = value
                    TapHandler {
                        gesturePolicy: TapHandler.DragThreshold
                        onTapped: (ep) => {
                            surface.seekMs = PlaybackMath.seekTargetMs(
                                ep.position.x, slider.leftPadding,
                                slider.availableWidth, tc.durationMs);
                        }
                    }
                }
            }
        }
    }

    function init() {
        surface.surfaceClicks = 0;
        surface.seekMs = -1;
        slider.value = 0;
    }

    // Every one of these lands on the bar but on no control: the left margin,
    // the gap between the button and the track, and the strip below a 36px
    // button in a 56px bar. Before the fix each of them reached the item
    // underneath.
    function test_bareChromeDoesNotReachSurface_data() {
        return [
            { tag: "left margin",  x: 6,  y: 28 },
            { tag: "gap after button", x: 20 + 36 + 8, y: 28 },
            { tag: "below button", x: 30, y: 52 },
            { tag: "above button", x: 30, y: 4 },
            { tag: "right margin", x: 394, y: 28 },
        ];
    }
    function test_bareChromeDoesNotReachSurface(d) {
        mouseClick(bar, d.x, d.y);
        compare(surface.surfaceClicks, 0,
                "a click on bare control-bar chrome reached the item below it");
    }

    // ...while the picture itself still gets its click (play/pause).
    function test_pictureReachesSurface() {
        mouseClick(surface, 200, 40);
        compare(surface.surfaceClicks, 1);
    }

    // A bare Slider ignores a stationary click on its track entirely
    // (QQuickSlider only moves the handle once a drag grab is taken), which
    // is why the scrubber "might" have been scrubbing. The TapHandler is what
    // makes a click seek.
    function test_clickOnTrackSeeks() {
        var mid = slider.leftPadding + slider.availableWidth / 2;
        mouseClick(slider, mid, slider.height / 2);
        verify(surface.seekMs >= 0, "a click on the seek track did not seek at all");
        fuzzyCompare(surface.seekMs, tc.durationMs / 2, tc.durationMs * 0.05);
    }

    // And the handle can still be dragged: the tap handler yields once the
    // drag threshold is passed, so the Slider keeps its grab.
    function test_dragStillSeeks() {
        var y = slider.height / 2;
        var startX = slider.leftPadding + 2;
        var endX = slider.leftPadding + slider.availableWidth - 2;
        mousePress(slider, startX, y);
        mouseMove(slider, (startX + endX) / 2, y);
        mouseMove(slider, endX, y);
        mouseRelease(slider, endX, y);
        verify(slider.value > tc.durationMs * 0.75,
               "dragging the handle to the right end left value at " + slider.value);
    }
}
