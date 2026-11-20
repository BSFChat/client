import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import BSFChat
import "../js/PlaybackMath.js" as PlaybackMath

// Inline player for m.audio messages: play/pause, a scrubber, position and
// duration, and a download button. An uploaded mp3 used to render as an
// empty row, because nothing handled m.audio at all.
//
// Same media plumbing as VideoPlayerCard: the ticketed URL streams directly
// on desktop; on mobile the bytes are fetched through MediaDownloader first
// (Android's native player can't reliably stream several containers) and
// played from the local copy. Download/open goes through the mxc, never the
// ticketed URL, exactly as the file card does.
Rectangle {
    id: root

    property url source
    property string sourceMxc: ""
    property string eventId: ""
    property string fileName: ""
    property real fileSize: 0

    property url effectiveSource: Theme.isMobile ? "" : source
    property bool downloading: false
    property bool downloadFailed: false
    property bool _pendingPlay: false

    onSourceChanged: if (!Theme.isMobile) effectiveSource = source

    implicitWidth: Math.min(360, PlaybackMath.cardMaxWidth(Theme.isMobile))
    implicitHeight: col.implicitHeight + Theme.sp.s4 * 2
    width: implicitWidth
    color: Theme.bg2
    radius: Theme.r2
    border.color: Theme.line
    border.width: 1

    readonly property bool playing: player.playbackState === MediaPlayer.PlayingState

    function togglePlay() {
        if (playing) { player.pause(); return; }
        if (!_ensureLocal()) { _pendingPlay = true; return; }
        player.play();
    }
    function _ensureLocal() {
        if (!Theme.isMobile) return true;
        var s = source.toString();
        if (s.length === 0 || s.startsWith("file:") || s.startsWith("qrc:")) {
            effectiveSource = source;
            return true;
        }
        if (effectiveSource.toString().startsWith("file:")) return true;
        if (downloading) return false;
        if (typeof mediaDownloader === "undefined") return false;
        downloadFailed = false;
        downloading = true;
        mediaDownloader.request(s);
        return false;
    }

    Connections {
        target: typeof mediaDownloader !== "undefined" && Theme.isMobile ? mediaDownloader : null
        function onCompleted(remoteUrl, localFileUrl) {
            if (remoteUrl !== root.source.toString()) return;
            root.effectiveSource = localFileUrl;
            root.downloading = false;
            if (root._pendingPlay) { root._pendingPlay = false; player.play(); }
        }
        function onFailed(remoteUrl, err) {
            if (remoteUrl !== root.source.toString()) return;
            root.downloading = false;
            root.downloadFailed = true;
            root._pendingPlay = false;
        }
    }

    MediaPlayer {
        id: player
        source: root.effectiveSource
        audioOutput: AudioOutput {}
    }

    Accessible.role: Accessible.Grouping
    Accessible.name: root.fileName !== "" ? qsTr("Audio %1").arg(root.fileName) : qsTr("Audio")

    ColumnLayout {
        id: col
        anchors.fill: parent
        anchors.margins: Theme.sp.s4
        spacing: Theme.sp.s2

        Text {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            text: root.fileName !== "" ? root.fileName : qsTr("Audio")
            elide: Text.ElideMiddle
            color: Theme.fg0
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.sm
            font.weight: Theme.fontWeight.semibold
            Accessible.ignored: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.sp.s3

            Rectangle {
                id: playBtn
                Layout.preferredWidth: Theme.isMobile ? Theme.touchTarget : 32
                Layout.preferredHeight: Layout.preferredWidth
                radius: width / 2
                color: playMouse.containsMouse || playBtn.activeFocus ? Theme.accentDim : Theme.accent
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: root.playing ? qsTr("Pause") : qsTr("Play")
                Accessible.onPressAction: root.togglePlay()
                Keys.onReturnPressed: root.togglePlay()
                Keys.onSpacePressed: root.togglePlay()
                Icon {
                    anchors.centerIn: parent
                    name: root.downloading ? "chevron-down" : (root.playing ? "pause" : "play")
                    size: 14
                    color: Theme.onAccent
                }
                MouseArea {
                    id: playMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.togglePlay()
                }
            }

            ThemedSlider {
                id: scrubber
                Layout.fillWidth: true
                from: 0
                to: Math.max(1, player.duration)
                enabled: player.seekable && player.duration > 0
                value: pressed ? value : player.position
                Accessible.name: qsTr("Playback position")
                onMoved: player.position = value
            }

            Text {
                textFormat: Text.PlainText
                text: PlaybackMath.formatTime(player.position) + " / "
                      + PlaybackMath.formatTime(player.duration)
                color: Theme.fg2
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSize.xs
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("%1 of %2").arg(PlaybackMath.formatTime(player.position))
                                                 .arg(PlaybackMath.formatTime(player.duration))
            }

            // Download / open with the system player — the file card's path,
            // from the mxc.
            Rectangle {
                id: dlBtn
                Layout.preferredWidth: Theme.isMobile ? Theme.touchTarget : 28
                Layout.preferredHeight: Layout.preferredWidth
                radius: Theme.r1
                color: dlMouse.containsMouse || dlBtn.activeFocus ? Theme.bg3 : "transparent"
                visible: root.sourceMxc !== ""
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Download %1").arg(root.fileName)
                Accessible.description: root.fileSize > 0
                    ? PlaybackMath.formatFileSize(root.fileSize) : ""
                Accessible.onPressAction: _open()
                Keys.onReturnPressed: _open()
                Keys.onSpacePressed: _open()
                function _open() {
                    if (serverManager.activeServer)
                        serverManager.activeServer.openMediaExternally(root.sourceMxc, false,
                                                                       root.eventId);
                }
                Icon {
                    anchors.centerIn: parent
                    name: "chevron-down"
                    size: 14
                    color: Theme.fg1
                }
                MouseArea {
                    id: dlMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: dlBtn._open()
                }
            }
        }

        Text {
            visible: root.downloadFailed || player.error !== MediaPlayer.NoError
            Layout.fillWidth: true
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            text: qsTr("Can't play this here. Use download to open it.")
            color: Theme.danger
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.xs
        }
    }
}
