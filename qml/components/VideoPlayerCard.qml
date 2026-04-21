import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import QtMultimedia
import BSFChat

// Inline video player for m.video messages. Paused by default;
// click the play overlay to start. Transport bar fades in on hover
// and while playing, out at rest, so a paused video in a scrollback
// doesn't clutter the message row.
ColumnLayout {
    id: root

    property url source
    property string fileName: ""
    property real fileSize: 0

    // Cap dimensions to the same 400×300 envelope as the inline image
    // renderer — keeps a long conversation with mixed media visually
    // consistent. The real aspect ratio is preserved by VideoOutput's
    // PreserveAspectFit fillMode.
    property int maxWidth: 400
    property int maxHeight: 300

    // Audio state. Writable — the volume slider / mute button drive
    // these, and the AudioOutput binds to them. Starts at 1.0 so the
    // first play isn't silent.
    property real volume: 1.0
    property bool muted: false

    spacing: Theme.sp.s1

    Rectangle {
        id: videoCard
        Layout.preferredWidth: maxWidth
        Layout.preferredHeight: maxHeight
        Layout.maximumWidth: maxWidth
        Layout.maximumHeight: maxHeight
        color: "black"
        radius: Theme.r2
        clip: true

        MediaPlayer {
            id: mediaPlayer
            source: root.source
            videoOutput: videoOutput
            audioOutput: AudioOutput {
                id: audioOut
                // Persist user's last-picked video volume across cards
                // in this session. Starts at 100% so first play isn't
                // silent, toggleable via the speaker button.
                volume: root.volume
                muted: root.muted
            }
        }

        VideoOutput {
            id: videoOutput
            anchors.fill: parent
            fillMode: VideoOutput.PreserveAspectFit
        }

        // Dim scrim while paused so the play overlay reads clearly
        // against any first-frame. Fades out during playback.
        Rectangle {
            anchors.fill: parent
            color: Qt.rgba(0, 0, 0, 0.35)
            opacity: mediaPlayer.playbackState !== MediaPlayer.PlayingState ? 1.0 : 0.0
            Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }
        }

        // Big play overlay — visible whenever we aren't actively
        // playing (paused, stopped, loaded-but-unstarted, errored).
        // Hit target is the whole card via cardMouse below.
        Rectangle {
            anchors.centerIn: parent
            width: 64; height: 64; radius: 32
            color: Qt.rgba(0, 0, 0, 0.55)
            border.color: "white"
            border.width: 2
            visible: mediaPlayer.playbackState !== MediaPlayer.PlayingState
                  && mediaPlayer.error === MediaPlayer.NoError
            Icon {
                anchors.centerIn: parent
                // Nudge the triangle half a pixel right so it's
                // visually centred inside the circle — the glyph's
                // bounding box leans left-heavy.
                anchors.horizontalCenterOffset: 2
                name: "play"
                size: 24
                color: "white"
            }
        }

        // Error state — couldn't decode / fetch the video. We surface
        // Qt's errorString too: on macOS it usually reads "Could not
        // decode media" for unsupported codecs, or a network-shaped
        // message when the download itself failed.
        Rectangle {
            anchors.fill: parent
            color: Theme.bg2
            visible: mediaPlayer.error !== MediaPlayer.NoError
            ColumnLayout {
                anchors.centerIn: parent
                anchors.leftMargin: Theme.sp.s5
                anchors.rightMargin: Theme.sp.s5
                spacing: Theme.sp.s2
                Icon {
                    Layout.alignment: Qt.AlignHCenter
                    name: "eye"; size: 22; color: Theme.danger
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: "Couldn't play video"
                    color: Theme.fg0
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    font.weight: Theme.fontWeight.semibold
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.maximumWidth: root.maxWidth - Theme.sp.s5 * 2
                    horizontalAlignment: Text.AlignHCenter
                    text: mediaPlayer.errorString || ""
                    visible: text.length > 0
                    color: Theme.fg2
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSize.xs
                    wrapMode: Text.WordWrap
                    elide: Text.ElideRight
                    maximumLineCount: 2
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: "Middle-click to open in your browser"
                    color: Theme.fg3
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xs
                }
            }
        }

        // Transport bar — time elapsed + total duration + seek
        // slider + pause/play toggle. Fades in on hover + during
        // playback, out at rest.
        Rectangle {
            id: transport
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 40
            color: Qt.rgba(0, 0, 0, 0.65)
            opacity: cardMouse.containsMouse
                  || mediaPlayer.playbackState === MediaPlayer.PlayingState ? 1.0 : 0.0
            Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }
            visible: opacity > 0.01

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.sp.s3
                anchors.rightMargin: Theme.sp.s3
                spacing: Theme.sp.s3

                // Play/pause toggle.
                Rectangle {
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    radius: Theme.r1
                    color: playPauseMouse.containsMouse
                        ? Qt.rgba(1, 1, 1, 0.15) : "transparent"
                    Icon {
                        anchors.centerIn: parent
                        anchors.horizontalCenterOffset:
                            mediaPlayer.playbackState === MediaPlayer.PlayingState ? 0 : 1
                        name: mediaPlayer.playbackState === MediaPlayer.PlayingState
                            ? "pause" : "play"
                        size: 14
                        color: "white"
                    }
                    MouseArea {
                        id: playPauseMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.togglePlay()
                    }
                }

                // Time elapsed.
                Text {
                    text: formatTime(mediaPlayer.position)
                    color: "white"
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSize.xs
                    Layout.alignment: Qt.AlignVCenter
                }

                // Seek slider. Custom skin — filled-left track on
                // accent, thumb that scales on hover. Using Qt
                // Controls Slider rather than our ThemedSlider to
                // avoid leaking the app-level background tones into
                // the dark video chrome.
                Slider {
                    id: seekSlider
                    Layout.fillWidth: true
                    from: 0
                    to: Math.max(1, mediaPlayer.duration)
                    // Read-back is driven by mediaPlayer.position; only
                    // write the position when the user is dragging.
                    value: pressed ? value : mediaPlayer.position
                    onMoved: mediaPlayer.position = value
                    background: Rectangle {
                        x: seekSlider.leftPadding
                        y: seekSlider.topPadding
                            + seekSlider.availableHeight / 2 - height / 2
                        width: seekSlider.availableWidth
                        height: 3
                        radius: 1.5
                        color: Qt.rgba(1, 1, 1, 0.2)
                        Rectangle {
                            width: seekSlider.visualPosition * parent.width
                            height: parent.height
                            color: Theme.accent
                            radius: 1.5
                        }
                    }
                    handle: Rectangle {
                        x: seekSlider.leftPadding + seekSlider.visualPosition
                            * (seekSlider.availableWidth - width)
                        y: seekSlider.topPadding + seekSlider.availableHeight / 2 - height / 2
                        width: seekSlider.pressed ? 14 : (seekSlider.hovered ? 12 : 10)
                        height: width
                        radius: width / 2
                        color: "white"
                        Behavior on width { NumberAnimation { duration: Theme.motion.fastMs } }
                    }
                }

                // Duration.
                Text {
                    text: formatTime(mediaPlayer.duration)
                    color: "white"
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSize.xs
                    opacity: 0.6
                    Layout.alignment: Qt.AlignVCenter
                }

                // Volume toggle — click to mute/unmute. Hovering the
                // button reveals a vertical slider above it so you can
                // dial the level without leaving the card.
                Item {
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    Layout.alignment: Qt.AlignVCenter

                    Rectangle {
                        id: volumeBtn
                        anchors.fill: parent
                        radius: Theme.r1
                        color: volumeHover.containsMouse
                            ? Qt.rgba(1, 1, 1, 0.15) : "transparent"
                        Icon {
                            anchors.centerIn: parent
                            name: (root.muted || root.volume <= 0.001)
                                ? "volume-off" : "volume"
                            size: 14
                            color: "white"
                        }
                        MouseArea {
                            id: volumeHover
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.muted = !root.muted
                        }
                    }

                    // Hover-revealed vertical volume slider. Pops above
                    // the speaker button so the cursor can move into it
                    // without leaving the hover group — volumeGroupHover
                    // holds it visible while the cursor is on either
                    // the button or the slider.
                    Rectangle {
                        id: volumePopup
                        width: 32
                        height: 96
                        radius: Theme.r2
                        color: Qt.rgba(0, 0, 0, 0.85)
                        border.color: Qt.rgba(1, 1, 1, 0.10)
                        border.width: 1
                        // Sit 4px above the speaker icon.
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.top
                        anchors.bottomMargin: 4
                        visible: opacity > 0.01
                        opacity: (volumeHover.containsMouse
                                 || volumePopupHover.containsMouse
                                 || volumeSlider.pressed) ? 1.0 : 0.0
                        Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }

                        Slider {
                            id: volumeSlider
                            anchors.centerIn: parent
                            height: parent.height - 16
                            width: 24
                            orientation: Qt.Vertical
                            from: 0.0
                            to: 1.0
                            value: root.muted ? 0 : root.volume
                            onMoved: {
                                root.volume = value;
                                // Dragging into any non-zero position
                                // implicitly unmutes; dragging to zero
                                // mutes.
                                root.muted = value <= 0.001;
                            }
                            background: Rectangle {
                                x: volumeSlider.leftPadding
                                    + volumeSlider.availableWidth / 2 - width / 2
                                y: volumeSlider.topPadding
                                width: 3
                                height: volumeSlider.availableHeight
                                radius: 1.5
                                color: Qt.rgba(1, 1, 1, 0.2)
                                Rectangle {
                                    // Filled portion grows from the bottom.
                                    width: parent.width
                                    height: volumeSlider.visualPosition === 1
                                        ? 0 : (1 - volumeSlider.visualPosition)
                                                * parent.height
                                    y: parent.height - height
                                    radius: parent.radius
                                    color: Theme.accent
                                }
                            }
                            handle: Rectangle {
                                x: volumeSlider.leftPadding
                                    + volumeSlider.availableWidth / 2 - width / 2
                                y: volumeSlider.topPadding
                                    + volumeSlider.visualPosition
                                        * (volumeSlider.availableHeight - height)
                                width: volumeSlider.pressed ? 14
                                     : volumeSlider.hovered ? 12 : 10
                                height: width
                                radius: width / 2
                                color: "white"
                                Behavior on width { NumberAnimation { duration: Theme.motion.fastMs } }
                            }
                        }

                        MouseArea {
                            id: volumePopupHover
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.NoButton
                        }
                    }
                }

                // Fullscreen. Pauses the inline player + hands off the
                // current position to a full-window popup. On close the
                // inline seeks back to wherever the popup left off, so
                // playback feels continuous even though we're swapping
                // MediaPlayer instances.
                Rectangle {
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    Layout.alignment: Qt.AlignVCenter
                    radius: Theme.r1
                    color: fullscreenHover.containsMouse
                        ? Qt.rgba(1, 1, 1, 0.15) : "transparent"
                    Icon {
                        anchors.centerIn: parent
                        name: "expand"
                        size: 14
                        color: "white"
                    }
                    MouseArea {
                        id: fullscreenHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.openFullscreen()
                    }
                    ToolTip.visible: fullscreenHover.containsMouse
                    ToolTip.text: "Fullscreen"
                    ToolTip.delay: 500
                }
            }
        }

        // Whole-card click target — below the transport bar in z so
        // clicks on the play button / slider still reach those. Left-
        // click toggles play/pause; middle-click opens in browser
        // (same escape hatch as inline images).
        MouseArea {
            id: cardMouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton
            z: -1  // stays behind transport controls + play overlay
            // Single-click only. Having both onClicked AND onDoubleClicked
            // makes Qt delay onClicked by the system double-click interval
            // (~500ms on macOS) because it has to disambiguate — that
            // felt like "play is broken." Fullscreen is reachable via
            // the explicit expand button in the transport bar instead.
            onClicked: (m) => {
                if (m.button === Qt.MiddleButton) {
                    Qt.openUrlExternally(root.source);
                } else {
                    root.togglePlay();
                }
            }
        }
    }

    // Filename + size below. Same cue as the inline-image caption.
    RowLayout {
        spacing: Theme.sp.s1
        visible: root.fileName !== ""
        Text {
            text: root.fileName
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.sm
            color: Theme.fg2
            elide: Text.ElideMiddle
            Layout.maximumWidth: root.maxWidth - 80
        }
        Text {
            visible: root.fileSize > 0
            text: "· " + formatFileSize(root.fileSize)
            font.family: Theme.fontMono
            font.pixelSize: Theme.fontSize.xs
            color: Theme.fg3
        }
    }

    function togglePlay() {
        if (mediaPlayer.playbackState === MediaPlayer.PlayingState) {
            mediaPlayer.pause();
        } else {
            mediaPlayer.play();
        }
    }

    function formatTime(ms) {
        if (!ms || ms < 0) return "0:00";
        var s = Math.floor(ms / 1000);
        var m = Math.floor(s / 60);
        var sec = s % 60;
        if (m >= 60) {
            var h = Math.floor(m / 60);
            var min = m % 60;
            return h + ":" + (min < 10 ? "0" : "") + min
                     + ":" + (sec < 10 ? "0" : "") + sec;
        }
        return m + ":" + (sec < 10 ? "0" : "") + sec;
    }

    function formatFileSize(bytes) {
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB";
        if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + " MB";
        return (bytes / (1024 * 1024 * 1024)).toFixed(1) + " GB";
    }

    function openFullscreen() {
        // Hand the current position + playback state to the fullscreen
        // popup so it starts where the inline player is. We pause the
        // inline one so two copies aren't playing simultaneously.
        fullscreenPopup.seedPosition = mediaPlayer.position;
        fullscreenPopup.seedPlaying = mediaPlayer.playbackState === MediaPlayer.PlayingState;
        mediaPlayer.pause();
        fullscreenPopup.open();
    }

    // Full-window lightbox for video. Standalone MediaPlayer +
    // VideoOutput so QtMultimedia can honour fullscreen-worthy
    // sizing without fighting the inline card's layout. Shares the
    // inline card's volume/mute state by binding to `root.volume` /
    // `root.muted` directly.
    Popup {
        id: fullscreenPopup
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        width: parent ? parent.width : 0
        height: parent ? parent.height : 0
        modal: true
        padding: 0
        closePolicy: Popup.CloseOnEscape
        focus: true

        property real seedPosition: 0
        property bool seedPlaying: false

        background: Rectangle { color: Qt.rgba(0, 0, 0, 0.95) }

        onOpened: {
            fsPlayer.source = root.source;
            fsPlayer.position = seedPosition;
            if (seedPlaying) fsPlayer.play();
            else fsPlayer.pause();
        }
        onClosed: {
            // Seek inline to where the popup was, then resume if it was
            // playing — playback feels continuous across the toggle.
            mediaPlayer.position = fsPlayer.position;
            var wasPlaying = fsPlayer.playbackState === MediaPlayer.PlayingState;
            fsPlayer.stop();
            fsPlayer.source = "";   // free decoder / network handle
            if (wasPlaying) mediaPlayer.play();
        }

        contentItem: Item {
            anchors.fill: parent

            MediaPlayer {
                id: fsPlayer
                videoOutput: fsVideoOutput
                audioOutput: AudioOutput {
                    volume: root.volume
                    muted: root.muted
                }
            }

            VideoOutput {
                id: fsVideoOutput
                anchors.fill: parent
                fillMode: VideoOutput.PreserveAspectFit
            }

            // Click empty area / hit Esc to close.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                onClicked: fullscreenPopup.close()
                onDoubleClicked: fullscreenPopup.close()
                z: -1
            }

            // Big centered play overlay (same styling as inline).
            Rectangle {
                anchors.centerIn: parent
                width: 72; height: 72; radius: 36
                color: Qt.rgba(0, 0, 0, 0.55)
                border.color: "white"; border.width: 2
                visible: fsPlayer.playbackState !== MediaPlayer.PlayingState
                Icon {
                    anchors.centerIn: parent
                    anchors.horizontalCenterOffset: 2
                    name: "play"; size: 28; color: "white"
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: fsPlayer.play()
                }
            }

            // Bottom transport bar — same vocabulary as inline, but
            // spans the full window width for cinema feel.
            Rectangle {
                id: fsTransport
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 56
                color: Qt.rgba(0, 0, 0, 0.75)
                opacity: fsTransportHover.containsMouse
                    || fsPlayer.playbackState !== MediaPlayer.PlayingState ? 1.0 : 0.0
                Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }

                MouseArea {
                    id: fsTransportHover
                    anchors.fill: parent
                    anchors.topMargin: -60   // expanded hit area so a
                    hoverEnabled: true        // cursor near the bar keeps it visible
                    acceptedButtons: Qt.NoButton
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.sp.s5
                    anchors.rightMargin: Theme.sp.s5
                    spacing: Theme.sp.s4

                    Rectangle {
                        Layout.preferredWidth: 36
                        Layout.preferredHeight: 36
                        radius: Theme.r1
                        color: fsPlayPauseMouse.containsMouse
                            ? Qt.rgba(1, 1, 1, 0.15) : "transparent"
                        Icon {
                            anchors.centerIn: parent
                            anchors.horizontalCenterOffset:
                                fsPlayer.playbackState === MediaPlayer.PlayingState ? 0 : 1
                            name: fsPlayer.playbackState === MediaPlayer.PlayingState
                                ? "pause" : "play"
                            size: 18
                            color: "white"
                        }
                        MouseArea {
                            id: fsPlayPauseMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (fsPlayer.playbackState === MediaPlayer.PlayingState)
                                    fsPlayer.pause();
                                else fsPlayer.play();
                            }
                        }
                    }
                    Text {
                        text: formatTime(fsPlayer.position)
                        color: "white"
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSize.sm
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Slider {
                        id: fsSeekSlider
                        Layout.fillWidth: true
                        from: 0
                        to: Math.max(1, fsPlayer.duration)
                        value: pressed ? value : fsPlayer.position
                        onMoved: fsPlayer.position = value
                        background: Rectangle {
                            x: fsSeekSlider.leftPadding
                            y: fsSeekSlider.topPadding
                                + fsSeekSlider.availableHeight / 2 - height / 2
                            width: fsSeekSlider.availableWidth
                            height: 3
                            radius: 1.5
                            color: Qt.rgba(1, 1, 1, 0.2)
                            Rectangle {
                                width: fsSeekSlider.visualPosition * parent.width
                                height: parent.height
                                radius: 1.5
                                color: Theme.accent
                            }
                        }
                        handle: Rectangle {
                            x: fsSeekSlider.leftPadding + fsSeekSlider.visualPosition
                                * (fsSeekSlider.availableWidth - width)
                            y: fsSeekSlider.topPadding + fsSeekSlider.availableHeight / 2 - height / 2
                            width: fsSeekSlider.pressed ? 16
                                 : fsSeekSlider.hovered ? 14 : 12
                            height: width
                            radius: width / 2
                            color: "white"
                            Behavior on width { NumberAnimation { duration: Theme.motion.fastMs } }
                        }
                    }
                    Text {
                        text: formatTime(fsPlayer.duration)
                        color: "white"
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSize.sm
                        opacity: 0.6
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Rectangle {
                        Layout.preferredWidth: 36
                        Layout.preferredHeight: 36
                        radius: Theme.r1
                        color: fsMuteMouse.containsMouse
                            ? Qt.rgba(1, 1, 1, 0.15) : "transparent"
                        Icon {
                            anchors.centerIn: parent
                            name: (root.muted || root.volume <= 0.001)
                                ? "volume-off" : "volume"
                            size: 18
                            color: "white"
                        }
                        MouseArea {
                            id: fsMuteMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.muted = !root.muted
                        }
                    }
                    // Exit fullscreen.
                    Rectangle {
                        Layout.preferredWidth: 36
                        Layout.preferredHeight: 36
                        radius: Theme.r1
                        color: fsExitMouse.containsMouse
                            ? Qt.rgba(1, 1, 1, 0.15) : "transparent"
                        Icon {
                            anchors.centerIn: parent
                            name: "x"
                            size: 16
                            color: "white"
                        }
                        MouseArea {
                            id: fsExitMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: fullscreenPopup.close()
                        }
                        ToolTip.visible: fsExitMouse.containsMouse
                        ToolTip.text: "Exit fullscreen  (Esc)"
                        ToolTip.delay: 500
                    }
                }
            }
        }
    }
}
