import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// Full-window image lightbox. A single instance lives in MessageView and
// is invoked via openFor(url, filename, size) when a MessageBubble emits
// imageOpenRequested.
//
// Controls:
//   • Scroll wheel               → zoom, anchored on the cursor
//   • Click-and-drag on image    → pan (when zoomed past the viewport)
//   • ⌘/Ctrl +/−                 → zoom in/out
//   • ⌘/Ctrl 0                   → reset fit + center
//   • Click outside the image    → close
//   • Esc                        → close
//   • Middle-click on the inline
//     image (not in here)        → opens in browser instead of launching
//
// Pan/zoom are tracked as plain `zoom`, `panX`, `panY` reals on the
// viewer root — no Flickable. A Flickable would eat wheel events before
// our handler got a chance, and its click-drag conventions fight with
// the "click empty space to close" affordance we want.
Popup {
    id: viewer

    property string imageUrl: ""
    property string filename: ""
    property real   fileSize: 0

    property real minZoom: 0.1
    property real maxZoom: 10.0
    property real zoom: 1.0
    property real panX: 0
    property real panY: 0

    function openFor(url, name, size) {
        imageUrl = url;
        filename = name || "";
        fileSize = size || 0;
        zoom = 1.0;
        panX = 0;
        panY = 0;
        open();
    }

    function resetView() {
        zoom = 1.0;
        panX = 0;
        panY = 0;
    }

    modal: true
    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    width:  parent ? parent.width  : 0
    height: parent ? parent.height : 0
    padding: 0
    focus: true
    closePolicy: Popup.CloseOnEscape

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0.0; to: 1.0;
                          duration: Theme.motion.fastMs;
                          easing.type: Easing.OutCubic }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1.0; to: 0.0;
                          duration: Theme.motion.fastMs;
                          easing.type: Easing.InCubic }
    }

    background: Rectangle { color: Qt.rgba(0, 0, 0, 0.88) }

    contentItem: Item {
        id: viewport
        anchors.fill: parent

        // Bottom layer — click-to-close on empty space. Declared FIRST
        // so items declared later (imageContainer) stack on top and
        // steal clicks within their own bounds. Clicks that land on
        // transparent areas fall through to this MouseArea and close.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onClicked: viewer.close()
            // Wheel events anywhere outside the image also zoom — feels
            // natural when there's a lot of black space around a small
            // image.
            onWheel: (w) => imageMouse.applyZoom(w.angleDelta.y,
                                                 w.x - imageContainer.x,
                                                 w.y - imageContainer.y)
        }

        // Image container — transformed by zoom (via `effectiveScale`)
        // and pan (via `viewer.panX`/`panY`). Centered in the viewport
        // when content fits; the pan offsets then push it off-center
        // for drag interactions.
        Item {
            id: imageContainer
            readonly property real imgW: mediaImage.sourceSize.width || 1
            readonly property real imgH: mediaImage.sourceSize.height || 1
            readonly property real fitScale: {
                if (imgW <= 0 || imgH <= 0) return 1.0;
                var sx = viewport.width  / imgW;
                var sy = viewport.height / imgH;
                return Math.min(sx, sy, 1.0);
            }
            readonly property real effectiveScale: fitScale * viewer.zoom
            width:  imgW * effectiveScale
            height: imgH * effectiveScale
            x: (viewport.width  - width)  / 2 + viewer.panX
            y: (viewport.height - height) / 2 + viewer.panY

            Image {
                id: mediaImage
                anchors.fill: parent
                source: viewer.imageUrl
                fillMode: Image.Stretch
                asynchronous: true
                cache: true
                smooth: true
                mipmap: true
            }

            // Loading placeholder.
            Rectangle {
                anchors.fill: parent
                color: Qt.rgba(0, 0, 0, 0.5)
                visible: mediaImage.status === Image.Loading
                Text {
                    anchors.centerIn: parent
                    text: "Loading…"
                    color: Theme.fg0
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                }
            }

            // Error state.
            Rectangle {
                anchors.fill: parent
                color: Theme.bg3
                visible: mediaImage.status === Image.Error
                ColumnLayout {
                    anchors.centerIn: parent
                    spacing: Theme.sp.s3
                    Icon {
                        Layout.alignment: Qt.AlignHCenter
                        name: "eye"; size: 32; color: Theme.danger
                    }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: "Couldn't load image"
                        color: Theme.fg0
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.semibold
                    }
                }
            }

            // Drag-to-pan + wheel-to-zoom + click-absorb on the image.
            // Because this MouseArea sits on top of the close-MouseArea,
            // it wins hit-tests within the image's bounds — clicks on
            // the image don't propagate to close the viewer.
            MouseArea {
                id: imageMouse
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                cursorShape: pressed      ? Qt.ClosedHandCursor
                           : containsMouse ? Qt.OpenHandCursor
                                           : Qt.ArrowCursor
                // Pan origin — captured on press so drag math is done
                // against the pan offset at mouse-down, not the live
                // value (otherwise small mouse jitter doubles up).
                property real panOriginX: 0
                property real panOriginY: 0
                property real pressX: 0
                property real pressY: 0

                // Mobile swipe-down-to-dismiss: only armed at (near-)fit
                // zoom, otherwise the user is panning a zoomed image.
                property real _dismissStartY: -1

                onPressed: (m) => {
                    pressX = m.x;
                    pressY = m.y;
                    panOriginX = viewer.panX;
                    panOriginY = viewer.panY;
                    if (Theme.isMobile && viewer.zoom <= 1.05)
                        _dismissStartY = m.y;
                }
                onPositionChanged: (m) => {
                    if (!pressed) return;
                    viewer.panX = panOriginX + (m.x - pressX);
                    viewer.panY = panOriginY + (m.y - pressY);
                }
                onReleased: (m) => {
                    if (Theme.isMobile && _dismissStartY >= 0) {
                        var dy = m.y - _dismissStartY;
                        if (dy > 120) viewer.close();
                        _dismissStartY = -1;
                    }
                }
                onWheel: (w) => applyZoom(w.angleDelta.y, w.x, w.y)

                // Mobile pinch zoom. Sits inside the MouseArea so it
                // cooperates with the drag-pan grab — PointerHandlers
                // run alongside MouseArea's gesture machinery.
                PinchHandler {
                    enabled: Theme.isMobile
                    target: null  // manual — keep our own zoom plumbing
                    onActiveScaleChanged: {
                        var newZoom = Math.max(viewer.minZoom,
                            Math.min(viewer.maxZoom,
                                viewer.zoom * activeScale));
                        if (newZoom === viewer.zoom) return;
                        viewer.zoom = newZoom;
                    }
                }

                // Wheel→zoom implementation, shared with the outer
                // close-MouseArea so scrolling over the empty border
                // around a small image also zooms.
                //
                // `deltaY` is Qt's angleDelta in 8ths of a degree; a
                // standard mouse wheel notch is 120. Dividing by 120
                // gives "wheel ticks," and trackpads naturally emit
                // smaller proportional ticks so scrolling feels smooth
                // instead of stepping in big leaps.
                //
                // Zoom-to-pointer: (localX, localY) is the cursor in
                // imageContainer-local coords. The image is scaled
                // around the container's top-left (0, 0), but the
                // container itself is centered in the viewport via
                // `x: (viewport.width - width) / 2 + panX`. So to keep
                // the cursor pixel under the cursor, pan has to move
                // by the scaled distance from the container CENTER,
                // not from 0 — that's the `- w/2` / `- h/2` term that
                // was missing before.
                function applyZoom(deltaY, localX, localY) {
                    if (deltaY === 0) return;
                    var ticks = deltaY / 120;
                    var step = Math.pow(1.12, ticks);
                    var newZoom = Math.max(viewer.minZoom,
                                           Math.min(viewer.maxZoom,
                                                    viewer.zoom * step));
                    if (newZoom === viewer.zoom) return;

                    var k = newZoom / viewer.zoom;
                    var oldW = imageContainer.width;
                    var oldH = imageContainer.height;
                    viewer.zoom = newZoom;
                    viewer.panX -= (localX - oldW / 2) * (k - 1);
                    viewer.panY -= (localY - oldH / 2) * (k - 1);
                }
            }
        }

        // Top-right action cluster — close + open-in-browser.
        RowLayout {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: Theme.sp.s5
            anchors.rightMargin: Theme.sp.s5
            spacing: Theme.sp.s2
            // z: keep above the close-MouseArea below.
            z: 2

            component GlassBtn: Rectangle {
                id: gbtn
                property string icon: ""
                property string tooltip: ""
                signal clicked()
                Layout.preferredWidth: 36
                Layout.preferredHeight: 36
                radius: Theme.r2
                color: gbtnMouse.containsMouse
                    ? Qt.rgba(1, 1, 1, 0.18) : Qt.rgba(1, 1, 1, 0.08)
                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                Icon {
                    anchors.centerIn: parent
                    name: gbtn.icon; size: 16; color: "#ffffff"
                }
                MouseArea {
                    id: gbtnMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: gbtn.clicked()
                }
                ToolTip.visible: gbtnMouse.containsMouse && gbtn.tooltip.length > 0
                ToolTip.text: gbtn.tooltip
                ToolTip.delay: 500
            }

            GlassBtn {
                icon: "link"
                tooltip: "Open in browser"
                onClicked: Qt.openUrlExternally(viewer.imageUrl)
            }
            GlassBtn {
                icon: "x"
                tooltip: "Close  (Esc)"
                onClicked: viewer.close()
            }
        }

        // Bottom caption strip.
        Rectangle {
            z: 2
            visible: viewer.filename.length > 0 || viewer.zoom !== 1.0
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottomMargin: Theme.sp.s5
            height: captionRow.implicitHeight + Theme.sp.s3 * 2
            width: Math.min(captionRow.implicitWidth + Theme.sp.s7 * 2,
                            parent.width - Theme.sp.s7 * 2)
            radius: Theme.r2
            color: Qt.rgba(0, 0, 0, 0.55)
            border.color: Qt.rgba(1, 1, 1, 0.10)
            border.width: 1

            RowLayout {
                id: captionRow
                anchors.centerIn: parent
                spacing: Theme.sp.s4
                Text {
                    visible: viewer.filename.length > 0
                    text: viewer.filename
                    color: "#ffffff"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    font.weight: Theme.fontWeight.semibold
                    elide: Text.ElideMiddle
                    Layout.maximumWidth: 360
                }
                Text {
                    visible: viewer.fileSize > 0
                    text: viewer.fileSize < 1024 ? viewer.fileSize + " B"
                        : viewer.fileSize < 1024 * 1024
                            ? (viewer.fileSize / 1024).toFixed(1) + " KB"
                        : viewer.fileSize < 1024 * 1024 * 1024
                            ? (viewer.fileSize / (1024 * 1024)).toFixed(1) + " MB"
                            : (viewer.fileSize / (1024 * 1024 * 1024)).toFixed(1) + " GB"
                    color: Qt.rgba(1, 1, 1, 0.6)
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSize.xs
                }
                Rectangle {
                    visible: viewer.zoom !== 1.0
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 14
                    color: Qt.rgba(1, 1, 1, 0.2)
                }
                Text {
                    visible: viewer.zoom !== 1.0
                    text: Math.round(viewer.zoom * 100) + "%"
                    color: Theme.accent
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSize.xs
                    font.weight: Theme.fontWeight.semibold
                }
                // Reset button — shows only when zoomed/panned so the
                // steady state stays clean.
                Rectangle {
                    visible: viewer.zoom !== 1.0
                         || viewer.panX !== 0 || viewer.panY !== 0
                    Layout.preferredWidth: resetText.implicitWidth + Theme.sp.s3
                    Layout.preferredHeight: 18
                    radius: Theme.r1
                    color: resetMouse.containsMouse
                        ? Qt.rgba(1, 1, 1, 0.18) : Qt.rgba(1, 1, 1, 0.08)
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    Text {
                        id: resetText
                        anchors.centerIn: parent
                        text: "Reset"
                        color: "#ffffff"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                        font.weight: Theme.fontWeight.semibold
                    }
                    MouseArea {
                        id: resetMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: viewer.resetView()
                    }
                }
            }
        }
    }

    // ⌘/Ctrl +/−/0 shortcuts. Esc is handled by closePolicy.
    Keys.onPressed: (e) => {
        var mod = (e.modifiers & Qt.ControlModifier)
               || (e.modifiers & Qt.MetaModifier);
        if (mod && (e.key === Qt.Key_Plus || e.key === Qt.Key_Equal)) {
            zoom = Math.min(maxZoom, zoom * 1.2);
            e.accepted = true;
        } else if (mod && e.key === Qt.Key_Minus) {
            zoom = Math.max(minZoom, zoom / 1.2);
            e.accepted = true;
        } else if (mod && e.key === Qt.Key_0) {
            resetView();
            e.accepted = true;
        }
    }
}
