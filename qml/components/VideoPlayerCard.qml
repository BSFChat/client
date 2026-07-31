import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import QtMultimedia
import BSFChat
import "../js/PlaybackMath.js" as PlaybackMath

// Inline video player for m.video messages. Paused by default;
// click the play overlay to start. Transport bar fades in on hover
// and while playing, out at rest, so a paused video in a scrollback
// doesn't clutter the message row.
//
// ── INPUT RULES (the thing this component keeps getting wrong) ──
// Clicking the picture toggles play/pause. Clicking the control bar
// does whatever that control does — and NOTHING else. The second half
// is not automatic: the bar is a plain Rectangle, so any pixel of it
// not covered by a child that actually accepts the press falls through
// to the full-bleed MouseArea underneath (gaps between buttons, the
// RowLayout spacing, the strips above/below a 28px button in a 40px
// bar). That fall-through is why clicking beside the scrubber used to
// collapse the fullscreen view. Every bar therefore starts with an
// event-absorbing MouseArea (`*BarBlocker`) as its FIRST child, so the
// real controls sit above it and everything else stops there. Add a
// new floating panel over the video? It needs one too.
ColumnLayout {
    id: root

    property url source
    property string fileName: ""
    property real fileSize: 0

    // Effective URL fed to MediaPlayer. On desktop this is identical to
    // `source`. On Android we download first (see below) and swap in a
    // local file:// URL once the bytes are on disk — Android's native
    // MediaPlayer can't reliably stream several server-side containers
    // (Matroska especially) and returns "Could not open file" without
    // diagnostics for them. Streaming-via-local-file costs a one-time
    // download but actually plays.
    // Effective source is what we actually hand to the MediaPlayer.
    // Desktop: identical to `source` so streaming works as-is. Mobile:
    // starts empty and only becomes the local file:// URL once
    // MediaDownloader has a full copy on disk — if we let MediaPlayer
    // see the remote HTTPS URL it eagerly tries to open it, fails
    // with "Could not open file" on many containers (MKV in
    // particular), and stays wedged in the error state even after we
    // later swap in a local path. Keeping it empty until the bytes
    // land sidesteps that.
    property url effectiveSource: Theme.isMobile ? "" : source
    property double downloadProgress: 0
    property bool downloading: false
    property bool downloadFailed: false
    property string downloadError: ""

    Component.onCompleted: {
        if (!Theme.isMobile) effectiveSource = source;
    }
    onSourceChanged: {
        if (!Theme.isMobile) effectiveSource = source;
    }

    // Mobile: kicked off the first time the user asks to play.
    // Pre-downloading every inline video on channel-load would thrash
    // cellular data and chew through Android's per-app cache quota.
    function _ensureDownloaded() {
        if (!Theme.isMobile) return true;
        var s = source.toString();
        if (s.length === 0 || s.startsWith("file:") || s.startsWith("qrc:")) {
            effectiveSource = source;
            return true;
        }
        if (effectiveSource.toString().startsWith("file:")) return true;
        if (downloading) return false;
        downloadFailed = false;
        downloading = true;
        downloadProgress = 0;
        mediaDownloader.request(s);
        return false;
    }

    Connections {
        target: typeof mediaDownloader !== "undefined"
                && Theme.isMobile ? mediaDownloader : null
        function onCompleted(remoteUrl, localFileUrl) {
            if (remoteUrl === root.source.toString()) {
                root.effectiveSource = localFileUrl;
                root.downloading = false;
                root.downloadProgress = 1.0;
                if (root._pendingFullscreen) {
                    root._pendingFullscreen = false;
                    // Fall through to auto-play when the popup opens.
                    root._pendingAutoPlay = false;
                    root.openFullscreen();
                } else if (root._pendingAutoPlay) {
                    root._pendingAutoPlay = false;
                    mediaPlayer.play();
                }
            }
        }
        function onFailed(remoteUrl, err) {
            if (remoteUrl === root.source.toString()) {
                root.downloading = false;
                root.downloadFailed = true;
                root.downloadError = err;
                console.warn("[VideoPlayer] download failed:", err,
                             "url=" + remoteUrl);
            }
        }
        function onProgressChanged(remoteUrl, p) {
            if (remoteUrl === root.source.toString())
                root.downloadProgress = p;
        }
    }

    // Intrinsic media dimensions from the m.video event's info block,
    // forwarded by MessageBubble.qml. 0 = the server didn't say.
    //
    // These are what stop the card perturbing the message list. The row
    // is laid out the moment the event arrives, long before the media
    // resolves; if the card's height depends on the media, the row grows
    // afterwards, contentHeight grows under the viewport and the channel
    // looks like it scrolled up on its own. Sized from the event, the
    // box is right on the first frame and never changes again.
    property int mediaWidth: 0
    property int mediaHeight: 0

    // Cap dimensions to the same 400×300 envelope as the inline image
    // renderer — keeps a long conversation with mixed media visually
    // consistent. The real aspect ratio is preserved by VideoOutput's
    // PreserveAspectFit fillMode.
    // Mobile bubbles are ~260px wide (matches MessageBubble.qml's image
    // cap); on a narrow phone a 400×300 card overflows the bubble and
    // pushes off the right edge. Desktop keeps the original envelope.
    property int maxWidth:  PlaybackMath.cardMaxWidth(Theme.isMobile)
    property int maxHeight: PlaybackMath.cardMaxHeight(Theme.isMobile)

    // The actual box, aspect-fitted inside that envelope. Same scale as
    // the inline-image path, from the same helpers MessageBubble.qml
    // uses to reserve the row — one arithmetic, two callers, no drift.
    readonly property int boxWidth:
        PlaybackMath.cardBoxWidth(mediaWidth, mediaHeight, Theme.isMobile)
    readonly property int boxHeight:
        PlaybackMath.cardBoxHeight(mediaWidth, mediaHeight, Theme.isMobile)

    // Audio state. Writable — the volume slider / mute button drive
    // these, and the AudioOutput binds to them. Starts at 1.0 so the
    // first play isn't silent.
    property real volume: 1.0
    property bool muted: false

    spacing: Theme.sp.s1

    Rectangle {
        id: videoCard
        Layout.preferredWidth: root.boxWidth
        Layout.preferredHeight: root.boxHeight
        Layout.maximumWidth: root.maxWidth
        Layout.maximumHeight: root.maxHeight
        color: "black"
        radius: Theme.r2
        clip: true

        // Single MediaPlayer drives BOTH the inline VideoOutput and
        // the fullscreen one. Two MediaPlayer instances on the same
        // local file fight over Android MediaCodec's tiny decoder
        // pool ("Cannot create codec, Failed to open FFmpeg codec
        // context") — sharing one player sidesteps the race entirely.
        // The `videoOutput` property is re-bound when the fullscreen
        // popup opens / closes.
        MediaPlayer {
            id: mediaPlayer
            source: root.effectiveSource
            videoOutput: root._videoOutputTarget
            audioOutput: AudioOutput {
                id: audioOut
                // Persist user's last-picked video volume across cards
                // in this session. Starts at 100% so first play isn't
                // silent, toggleable via the speaker button.
                volume: root.volume
                muted: root.muted
            }
            // Surface errors to logcat so we can diagnose mobile /
            // codec / network problems without the user having to
            // read small text from the error state.
            onErrorOccurred: (error, errorString) => {
                console.warn("[VideoPlayer]", error, errorString,
                             "src=" + root.source);
            }
        }

        VideoOutput {
            id: videoOutput
            anchors.fill: parent
            fillMode: VideoOutput.PreserveAspectFit
            Component.onCompleted: {
                if (root._videoOutputTarget === null)
                    root._videoOutputTarget = videoOutput;
            }
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
                  && !root.downloading
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

        // Download-in-progress overlay (mobile). We pre-download
        // videos before feeding the native MediaPlayer a file:// URL,
        // so there's a measurable gap between tapping the channel and
        // the player being ready. An accent progress bar across the
        // bottom + percentage label makes the wait legible.
        Rectangle {
            anchors.fill: parent
            color: Qt.rgba(0, 0, 0, 0.55)
            visible: root.downloading
            ColumnLayout {
                anchors.centerIn: parent
                spacing: Theme.sp.s2
                Icon {
                    Layout.alignment: Qt.AlignHCenter
                    name: "paperclip"; size: 22; color: "white"
                    opacity: 0.85
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: "Buffering video… "
                        + Math.round(root.downloadProgress * 100) + "%"
                    color: "white"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    font.weight: Theme.fontWeight.semibold
                }
            }
            Rectangle {
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                height: 3
                width: parent.width * root.downloadProgress
                color: Theme.accent
                Behavior on width { NumberAnimation { duration: 120 } }
            }
        }

        // Error state — couldn't decode / fetch the video. We surface
        // Qt's errorString too: on macOS it usually reads "Could not
        // decode media" for unsupported codecs, or a network-shaped
        // message when the download itself failed.
        Rectangle {
            anchors.fill: parent
            color: Theme.bg2
            visible: (mediaPlayer.error !== MediaPlayer.NoError
                      && !root.downloading)
                  || root.downloadFailed
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
            // Same reasoning as the fullscreen bar: don't assume a
            // hoverEnabled MouseArea on top leaves cardMouse's hover
            // intact, or the bar fades out from under the cursor the
            // moment it reaches a button.
            readonly property bool _hovered: cardMouse.containsMouse
                  || playPauseMouse.containsMouse || seekSlider.hovered
                  || seekSlider.pressed || volumeHover.containsMouse
                  || volumePopupHover.containsMouse || volumeSlider.pressed
                  || fullscreenHover.containsMouse
            opacity: _hovered
                  || mediaPlayer.playbackState === MediaPlayer.PlayingState ? 1.0 : 0.0
            Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }
            visible: opacity > 0.01

            // Event boundary for the bar. Declared FIRST so every real
            // control is above it; anything that lands on bare chrome
            // (margins, inter-button spacing, the strip above/below a
            // 28px button) stops here instead of reaching cardMouse and
            // pausing the video the user was aiming a scrub at.
            // hoverEnabled stays false on purpose — cardMouse below is
            // what decides whether this bar is shown, and stealing its
            // hover would make the bar fade out from under the cursor.
            MouseArea {
                id: transportBarBlocker
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
                cursorShape: Qt.ArrowCursor
                onClicked: (m) => { m.accepted = true; }
                onPressed: (m) => { m.accepted = true; }
            }

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
                    // Fill the bar's height so the whole strip around the
                    // 3px groove is a seek target rather than a hole that
                    // leaks clicks to whatever is underneath.
                    Layout.fillHeight: true
                    from: 0
                    to: Math.max(1, mediaPlayer.duration)
                    // Read-back is driven by mediaPlayer.position; only
                    // write the position when the user is dragging.
                    value: pressed ? value : mediaPlayer.position
                    onMoved: mediaPlayer.position = value
                    // Never take keyboard focus: `value` is bound to
                    // mediaPlayer.position, and Slider's own arrow-key
                    // handling assigns to `value` imperatively, which
                    // would break that binding for good — the bar would
                    // freeze mid-video with no visible cause.
                    focusPolicy: Qt.NoFocus

                    // Click-to-seek. Slider alone only moves for a drag,
                    // so a plain click on the track does nothing at all.
                    // DragThreshold keeps this passive: move past the
                    // threshold and the Slider's own drag takes over.
                    TapHandler {
                        gesturePolicy: TapHandler.DragThreshold
                        onTapped: (ep) => {
                            if (mediaPlayer.duration <= 0) return;
                            mediaPlayer.position = PlaybackMath.seekTargetMs(
                                ep.position.x, seekSlider.leftPadding,
                                seekSlider.availableWidth, mediaPlayer.duration);
                        }
                    }
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

                        // Same event boundary as the bar: this panel
                        // floats over the picture, so a click on its
                        // padding would otherwise reach cardMouse and
                        // pause the video while you were setting volume.
                        MouseArea {
                            id: volumeBarBlocker
                            anchors.fill: parent
                            acceptedButtons: Qt.AllButtons
                            cursorShape: Qt.ArrowCursor
                            onClicked: (m) => { m.accepted = true; }
                            onPressed: (m) => { m.accepted = true; }
                        }

                        Slider {
                            id: volumeSlider
                            focusPolicy: Qt.NoFocus
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

                // Fullscreen. Takes the WINDOW fullscreen (same call as
                // VoiceRoom's fullscreen toggle) and raises the video
                // over it — the button says "fullscreen", so it has to
                // mean fullscreen, not "fill the app window". The same
                // MediaPlayer keeps decoding; only its VideoOutput moves.
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
    //
    // Pinned to the height MessageBubble.qml reserved for it rather than
    // measured from the text: the row has to be worth a known number of
    // pixels before the card exists, and a caption that settled one pixel
    // off after the font resolved would move every message below it.
    RowLayout {
        id: captionRow
        spacing: Theme.sp.s1
        visible: root.fileName !== ""
        Layout.preferredHeight: PlaybackMath.captionRowHeight(
            root.fileName !== "", Theme.fontSize.sm)
        Layout.maximumHeight: Layout.preferredHeight
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

    // True when a tap-to-play should auto-play as soon as the download
    // settles. Set when the user taps while download is in flight;
    // cleared once we actually call play().
    property bool _pendingAutoPlay: false

    function togglePlay() {
        // On mobile the first tap may just kick off the download; we
        // set a pending-play flag so the download-completed signal
        // auto-starts playback rather than making the user tap twice.
        if (!_ensureDownloaded()) {
            _pendingAutoPlay = true;
            return;
        }
        if (mediaPlayer.playbackState === MediaPlayer.PlayingState) {
            mediaPlayer.pause();
        } else {
            mediaPlayer.play();
        }
    }

    // Relative seek, used by the arrow keys in fullscreen. Clamping
    // lives in PlaybackMath so it can be tested without a decoder.
    function seekBy(deltaMs) {
        if (mediaPlayer.duration <= 0) return;
        mediaPlayer.position = PlaybackMath.clampSeek(
            mediaPlayer.position, deltaMs, mediaPlayer.duration);
    }

    function formatTime(ms) { return PlaybackMath.formatTime(ms); }

    function formatFileSize(bytes) { return PlaybackMath.formatFileSize(bytes); }

    // ── Real fullscreen ───────────────────────────────────────────
    // "Fullscreen" means the window goes fullscreen, the way it does
    // in every other player; the popup on top of it is only how we get
    // a full-window surface for the picture. Same mechanism as
    // VoiceRoom's fullscreen toggle (Window.visibility), so the two
    // can't drift apart.
    //
    // Both halves hang off the popup's open/closed signals rather than
    // off the button, so that EVERY exit path — the button, Escape,
    // anything closing the popup later — puts the window back. A
    // restore hung off the button only would leave the app stuck
    // fullscreen the first time someone pressed Escape.
    property bool fullscreen: false
    property int _savedVisibility: Window.AutomaticVisibility

    function _enterWindowFullscreen() {
        var w = Window.window;
        if (!w) return;
        root._savedVisibility = w.visibility;
        w.visibility = Window.FullScreen;
        root.fullscreen = true;
    }

    function _restoreWindowVisibility() {
        root.fullscreen = false;
        var w = Window.window;
        if (!w) return;
        w.visibility = PlaybackMath.visibilityToRestore(
            root._savedVisibility, Window.Hidden, Window.Minimized,
            Window.AutomaticVisibility);
    }

    // Set when openFullscreen() is invoked before the mobile download
    // has finished. Once the download completes we open the popup and
    // auto-play.
    property bool _pendingFullscreen: false

    function openFullscreen() {
        // Mobile: don't pop the fullscreen until we have a playable
        // file. Otherwise the popup opens on top of an errored / empty
        // MediaPlayer and the user stares at a black rectangle.
        if (Theme.isMobile && !_ensureDownloaded()) {
            _pendingFullscreen = true;
            return;
        }

        // Single-player strategy: instead of spinning up a separate
        // mediaPlayer (which hit "Cannot create codec" on Android
        // because MediaCodec's decoder pool can't satisfy two
        // instances of the same stream, and on desktop failed for
        // analogous FFmpeg-context reasons), we hand the existing
        // MediaPlayer's frames to the fullscreen VideoOutput by
        // swapping `_videoOutputTarget`.
        _videoOutputTarget = fsVideoOutput;
        fullscreenPopup.open();
    }

    // Which VideoOutput the single MediaPlayer currently renders to.
    // Defaults to the inline card's VideoOutput; openFullscreen()
    // repoints it at the fullscreen one, fullscreenPopup.onClosed
    // points it back.
    property Item _videoOutputTarget: null

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

        background: Rectangle { color: Qt.rgba(0, 0, 0, 0.95) }

        onOpened: {
            console.log("[VideoPlayer] fullscreen opened, src="
                        + root.effectiveSource
                        + " pos=" + mediaPlayer.position
                        + " state=" + mediaPlayer.playbackState);
            // Take the window fullscreen for real. Deliberately before
            // the play() below so a slow visibility change can't be
            // mistaken for a slow start.
            root._enterWindowFullscreen();
            fsKeys.forceActiveFocus();
            // Fullscreen implies intent to watch — start playing if
            // we weren't already (e.g. user tapped fullscreen on a
            // paused / never-played card).
            if (mediaPlayer.playbackState !== MediaPlayer.PlayingState)
                mediaPlayer.play();
        }
        onClosed: {
            // Every close path lands here: the exit button, Escape, or
            // anything closing the popup programmatically.
            root._restoreWindowVisibility();
            // Return video frames to the inline card.
            root._videoOutputTarget = videoOutput;
        }

        contentItem: Item {
            id: fsKeys
            anchors.fill: parent
            focus: true

            // Keyboard transport, matching what people already have in
            // their fingers from every other player. Escape is left to
            // the popup's CloseOnEscape policy.
            Keys.onPressed: (e) => {
                switch (e.key) {
                case Qt.Key_Space:
                case Qt.Key_K:
                    root.togglePlay(); e.accepted = true; break;
                case Qt.Key_Left:
                    root.seekBy(-5000); e.accepted = true; break;
                case Qt.Key_Right:
                    root.seekBy(5000); e.accepted = true; break;
                case Qt.Key_Up:
                    root.volume = Math.min(1.0, root.volume + 0.05);
                    root.muted = false; e.accepted = true; break;
                case Qt.Key_Down:
                    root.volume = Math.max(0.0, root.volume - 0.05);
                    e.accepted = true; break;
                case Qt.Key_M:
                    root.muted = !root.muted; e.accepted = true; break;
                case Qt.Key_F:
                    fullscreenPopup.close(); e.accepted = true; break;
                default: break;
                }
            }

            // Fullscreen has its OWN VideoOutput. The shared
            // mediaPlayer's `videoOutput` is swapped to point here
            // when the popup opens (see openFullscreen), so the same
            // decoder keeps running — we just redirect its frames.
            VideoOutput {
                id: fsVideoOutput
                anchors.fill: parent
                fillMode: VideoOutput.PreserveAspectFit
            }

            // Clicking the picture toggles play/pause — the same thing
            // it does on the inline card, and what a click on a video
            // means everywhere else. It used to close fullscreen, which
            // is how a mis-aimed scrub ended up collapsing the video.
            // Exit is Escape, or the button on the bar.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                onClicked: root.togglePlay()
                z: -1
            }

            // Big centered play overlay (same styling as inline).
            Rectangle {
                anchors.centerIn: parent
                width: 72; height: 72; radius: 36
                color: Qt.rgba(0, 0, 0, 0.55)
                border.color: "white"; border.width: 2
                visible: mediaPlayer.playbackState !== MediaPlayer.PlayingState
                Icon {
                    anchors.centerIn: parent
                    anchors.horizontalCenterOffset: 2
                    name: "play"; size: 28; color: "white"
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: mediaPlayer.play()
                }
            }

            // Reveal zone: the bar's own footprint plus 60px above it,
            // so a cursor heading for the controls brings them back
            // before it gets there. It lives OUTSIDE the bar because
            // the bar is `visible: false` while hidden — a hover
            // detector inside it could never bring it back.
            MouseArea {
                id: fsTransportHover
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 116
                hoverEnabled: true
                acceptedButtons: Qt.NoButton
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
                // Hover is tracked on every control as well as on the
                // reveal zone: whether a hoverEnabled MouseArea on top
                // masks the one underneath is a Qt version detail we
                // shouldn't be betting the visibility of the controls on.
                readonly property bool _hovered: fsTransportHover.containsMouse
                    || fsPlayPauseMouse.containsMouse || fsMuteMouse.containsMouse
                    || fsExitMouse.containsMouse || fsSeekSlider.hovered
                    || fsSeekSlider.pressed
                opacity: _hovered
                    || mediaPlayer.playbackState !== MediaPlayer.PlayingState ? 1.0 : 0.0
                Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }
                // A faded-out bar must also stop being clickable. Without
                // this, an invisible pause button and an invisible
                // scrubber sit over the bottom of the picture and eat
                // clicks meant for the video.
                visible: opacity > 0.01

                // Event boundary — see the note at the top of the file.
                MouseArea {
                    id: fsTransportBarBlocker
                    anchors.fill: parent
                    acceptedButtons: Qt.AllButtons
                    cursorShape: Qt.ArrowCursor
                    onClicked: (m) => { m.accepted = true; }
                    onPressed: (m) => { m.accepted = true; }
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
                                mediaPlayer.playbackState === MediaPlayer.PlayingState ? 0 : 1
                            name: mediaPlayer.playbackState === MediaPlayer.PlayingState
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
                                if (mediaPlayer.playbackState === MediaPlayer.PlayingState)
                                    mediaPlayer.pause();
                                else mediaPlayer.play();
                            }
                        }
                    }
                    Text {
                        text: formatTime(mediaPlayer.position)
                        color: "white"
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSize.sm
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Slider {
                        id: fsSeekSlider
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        from: 0
                        to: Math.max(1, mediaPlayer.duration)
                        value: pressed ? value : mediaPlayer.position
                        onMoved: mediaPlayer.position = value
                        // Keep the keys for the fullscreen key handler,
                        // and keep Slider's arrow handling from
                        // overwriting the `value` binding above.
                        focusPolicy: Qt.NoFocus

                        // Click-to-seek; see the inline slider.
                        TapHandler {
                            gesturePolicy: TapHandler.DragThreshold
                            onTapped: (ep) => {
                                if (mediaPlayer.duration <= 0) return;
                                mediaPlayer.position = PlaybackMath.seekTargetMs(
                                    ep.position.x, fsSeekSlider.leftPadding,
                                    fsSeekSlider.availableWidth, mediaPlayer.duration);
                            }
                        }
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
                        text: formatTime(mediaPlayer.duration)
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
                        ToolTip.text: "Exit fullscreen  (Esc or F)"
                        ToolTip.delay: 500
                    }
                }
            }
        }
    }
}
