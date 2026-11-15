import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// Per-link OpenGraph unfurl card. A MessageBubble instantiates one
// (or two) of these via a Repeater keyed on URLs detected in its body.
//
// The preview is fetched by the HOMESERVER (GET /_matrix/media/v3/preview_url
// via ServerConnection.requestLinkPreview), never by this app: no request
// from the viewer's machine reaches the linked site, so it cannot learn who
// read the message, a link into the reader's LAN is not fetched from inside
// that LAN, and no third-party response is buffered here (audit Q-S4). The
// cache, de-duplication and rate-limiting are C++ (net/LinkPreviewCache);
// og:image arrives as an mxc:// URI and loads through the same ticketed media
// path as every other image. A server without preview_url means no previews —
// there is deliberately no client-side fallback. The "Show link previews"
// setting turns the request off entirely.
Rectangle {
    id: preview

    property url url

    // Plain text from the server, already trimmed (LinkPreviewCache::parse).
    // ogImage is an mxc:// URI, never an http(s) URL.
    property string ogTitle: ""
    property string ogDescription: ""
    property string ogImage: ""
    property string ogSiteName: ""

    readonly property var _server: serverManager.activeServer
    readonly property bool _enabled: appSettings.showLinkPreviews
        && !!_server && _server.linkPreviewsSupported

    // For known video platforms (YouTube for now) we use a landscape
    // "video embed" layout with a hero thumbnail; the thumbnail is the
    // server's og:image like any other card. Click the play overlay to open
    // in browser.
    readonly property string _videoId: _detectVideoId(String(preview.url))
    readonly property bool isVideoEmbed: _videoId !== ""
    // Host extracted from `url` as a fallback site-name display.
    readonly property string hostName: {
        var s = String(preview.url);
        var m = s.match(/^https?:\/\/([^\/:]+)/i);
        return m ? m[1] : "";
    }

    // The og:image's download URL. Empty until a media ticket lands; the
    // epoch is read AND compared so the binding re-runs when one does (see
    // ServerConnection.mediaTicketEpoch).
    readonly property string _imageSource: {
        if (!_server || ogImage === "") return "";
        if (_server.mediaTicketEpoch < 0) return "";
        return _server.resolveMediaUrl(ogImage);
    }

    readonly property bool ready: isVideoEmbed
        ? ogTitle.length > 0   // video cards are ready once the title arrives
        : (ogTitle.length > 0 || ogDescription.length > 0 || ogImage.length > 0)
    readonly property bool hasImage: ogImage.length > 0 && !_imageFailed
    property bool _failed: false
    property bool _imageFailed: false

    // Video embeds are visible immediately — we know the full
    // geometry from the URL pattern alone (16:9 thumbnail + reserved
    // title rows), so we render the skeleton before the server's
    // preview resolves. This is what stops a YouTube card resolving mid-
    // settle from shoving the rest of the message list around and
    // parking the user above the bottom on channel open.
    //
    // OG cards (non-video) still gate on `ready` because we don't
    // know their geometry until fetch completes, but their height
    // delta is small (~120px) and they're rare enough that the
    // layout shift is tolerable.
    visible: _enabled && (isVideoEmbed
        ? !_failed
        : (ready && !_failed))
    implicitHeight: isVideoEmbed
        ? (videoCol.implicitHeight + Theme.sp.s4 * 2)
        : (!ready ? 0
            : (hasImage ? 120 : previewCol.implicitHeight + Theme.sp.s4 * 2))
    implicitWidth: isVideoEmbed ? 480 : 420

    radius: Theme.r2
    color: Theme.bg2
    border.color: Theme.line
    border.width: 1
    clip: true

    // The whole card is one activatable thing: every hit area inside it
    // opens the same URL, so the card is the accessible node and the
    // chrome inside it (stripe, scrim, thumbnail, text rows) is ignored.
    Accessible.role: Accessible.Button
    Accessible.name: {
        var t = preview.ogTitle.length > 0 ? preview.ogTitle
                                           : String(preview.url);
        var s = preview.ogSiteName.length > 0 ? preview.ogSiteName
                                              : preview.hostName;
        if (preview.isVideoEmbed) {
            return s.length > 0 ? qsTr("Video: %1, from %2").arg(t).arg(s)
                                : qsTr("Video: %1").arg(t);
        }
        return s.length > 0 ? qsTr("Link preview: %1, from %2").arg(t).arg(s)
                            : qsTr("Link preview: %1").arg(t);
    }
    Accessible.description: preview.ogDescription.length > 0
        ? preview.ogDescription
        : qsTr("Opens the link in your browser")
    Accessible.onPressAction: linkOpener.open(preview.url)

    // Left accent stripe — mirrors the reply-preamble chrome so
    // "quoted content" reads consistently whether it's another
    // message or a linked page. Hidden for video embeds; the card
    // there is hero-thumbnail-forward and doesn't need the bar.
    Rectangle {
        visible: !preview.isVideoEmbed
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        Accessible.ignored: true
        width: 3
        color: Theme.accent
    }

    // === Video embed layout (YouTube et al.) ===
    ColumnLayout {
        id: videoCol
        visible: preview.isVideoEmbed
        anchors.fill: parent
        anchors.margins: Theme.sp.s4
        spacing: Theme.sp.s3

        // 16:9 thumbnail tile with a big play overlay. Click opens
        // the URL in the browser; Qt MediaPlayer can't play YouTube
        // directly (DRM/stream resolution handled in the JS player)
        // so external open is the right default. Later iteration can
        // swap in a WebEngine view for in-app playback.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: width * 9 / 16
            Layout.maximumHeight: 270
            // Thumbnail + play overlay. Activating it does exactly what
            // activating the card does, so it is folded into the card's
            // single announcement rather than repeated.
            Accessible.ignored: true
            radius: Theme.r1
            // Letterbox behind a 16:9 thumbnail: a scrim, not a panel, so it
            // stays black in light mode rather than flashing a pale frame
            // around the image while it loads.
            color: Theme.scrim
            clip: true

            Image {
                id: videoThumb
                anchors.fill: parent
                Accessible.ignored: true
                source: preview._imageSource
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                cache: true
                // The tile is at most 270pt tall at 16:9, i.e. 480pt wide.
                // Width only, so the decode keeps the thumbnail's aspect.
                sourceSize.width: 480 * Screen.devicePixelRatio
            }

            // Dim scrim so the play button reads over any frame.
            Rectangle {
                anchors.fill: parent
                Accessible.ignored: true
                color: Qt.rgba(0, 0, 0, 0.25)
            }

            // Red circle play button in the YouTube brand colour.
            Rectangle {
                anchors.centerIn: parent
                Accessible.ignored: true
                width: 72; height: 72; radius: 36
                color: videoMouse.containsMouse
                    ? Qt.rgba(1, 0, 0, 0.95)
                    : Qt.rgba(Theme.scrim.r, Theme.scrim.g, Theme.scrim.b, 0.7)
                // Sits on the video thumbnail, so it is on a scrim in both
                // themes — see Theme.scrimFg.
                border.color: Theme.scrimFg; border.width: 2
                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                Icon {
                    anchors.centerIn: parent
                    anchors.horizontalCenterOffset: 3
                    name: "play"
                    size: 28
                    color: Theme.scrimFg
                }
            }

            MouseArea {
                id: videoMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: linkOpener.open(preview.url)
            }
        }

        // Title + channel name under the thumbnail. Heights are
        // reserved at the 2-line / 1-line maximum from the start so
        // `videoCol.implicitHeight` doesn't grow when ogTitle finally
        // arrives from the OG fetch — that's the whole point of
        // rendering video embeds eagerly: a known, stable footprint.
        Text {
            Layout.fillWidth: true
            Accessible.ignored: true
            Layout.preferredHeight: Math.ceil(
                Theme.fontSize.md * 1.3 * 2)
            textFormat: Text.PlainText
            text: preview.ogTitle
            color: Theme.accent
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.md
            font.weight: Theme.fontWeight.semibold
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
            verticalAlignment: Text.AlignTop
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: linkOpener.open(preview.url)
            }
        }
        Text {
            Layout.fillWidth: true
            Accessible.ignored: true
            Layout.preferredHeight: Math.ceil(
                Theme.fontSize.xs * 1.3)
            textFormat: Text.PlainText
            // Always render — `hostName` is derived from the URL
            // synchronously, so the row is populated before the OG
            // fetch resolves. ogSiteName replaces it if/when it
            // arrives. Avoid a `visible:` toggle here so we don't
            // collapse the row mid-load.
            text: preview.ogSiteName.length > 0
                ? preview.ogSiteName : preview.hostName
            color: Theme.fg3
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.xs
            elide: Text.ElideRight
            verticalAlignment: Text.AlignTop
        }
    }

    // === Standard OpenGraph card layout ===
    RowLayout {
        visible: !preview.isVideoEmbed
        anchors.fill: parent
        anchors.leftMargin: Theme.sp.s4 + 3
        anchors.rightMargin: Theme.sp.s4
        anchors.topMargin: Theme.sp.s4
        anchors.bottomMargin: Theme.sp.s4
        spacing: Theme.sp.s4

        ColumnLayout {
            id: previewCol
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: 2

            // Site name — quiet, small. Falls back to the URL's host.
            Text {
                Accessible.ignored: true
                textFormat: Text.PlainText
                text: preview.ogSiteName.length > 0
                    ? preview.ogSiteName : preview.hostName
                visible: text.length > 0
                color: Theme.fg3
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            // Title — accent-coloured, clickable like a link.
            Text {
                Accessible.ignored: true
                textFormat: Text.PlainText
                text: preview.ogTitle
                visible: text.length > 0
                color: Theme.accent
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.md
                font.weight: Theme.fontWeight.semibold
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
                Layout.fillWidth: true

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: linkOpener.open(preview.url)
                }
            }
            // Description — dimmer, up to 3 lines.
            Text {
                Accessible.ignored: true
                textFormat: Text.PlainText
                text: preview.ogDescription
                visible: text.length > 0
                color: Theme.fg2
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                wrapMode: Text.Wrap
                maximumLineCount: 3
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }

        // Thumbnail — fixed-size on the right. Click opens the full
        // image viewer, consistent with inline m.image handling.
        Rectangle {
            visible: preview.hasImage
            // Decorative relative to the card's own name; tapping it
            // opens the same URL the card does.
            Accessible.ignored: true
            Layout.preferredWidth: 96
            Layout.preferredHeight: 96
            Layout.alignment: Qt.AlignVCenter
            radius: Theme.r1
            color: Theme.bg3
            clip: true

            Image {
                id: thumb
                anchors.fill: parent
                Accessible.ignored: true
                source: preview._imageSource
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                cache: true
                // Decode at tile size, not at whatever the page's og:image
                // is — routinely 1200x630 and sometimes several thousand
                // pixels a side, for a 96pt square. Fixed numbers, not the
                // tile's live size, so a relayout never re-decodes.
                sourceSize.width: 96 * Screen.devicePixelRatio
                sourceSize.height: 96 * Screen.devicePixelRatio
                // If the thumbnail fails to load, hide the tile so the card
                // doesn't look broken — the text side still stands on its
                // own. A flag rather than `parent.visible = false`, which
                // would break the binding for the next URL.
                onStatusChanged: {
                    if (status === Image.Error) preview._imageFailed = true;
                }
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: linkOpener.open(preview.url)
            }
        }
    }

    onUrlChanged: _fetch()
    on_EnabledChanged: _fetch()
    on_ServerChanged: _fetch()

    // The server answered (or gave up on) some URL; ours, maybe.
    Connections {
        target: preview._server
        ignoreUnknownSignals: true
        function onLinkPreviewResolved(u) {
            if (u === String(preview.url)) preview._load();
        }
    }

    function _fetch() {
        // Start from a clean slate. `url` is a binding, and a live delegate
        // sees it change whenever the model row updates or the delegate is
        // recycled for a different message. Every property below describes
        // the *previous* URL: a `_failed` left over from a dead link would
        // keep the card hidden for a perfectly good successor, and stale og*
        // fields would briefly render the old page's card under the new link.
        _failed = false;
        _imageFailed = false;
        ogTitle = "";
        ogDescription = "";
        ogImage = "";
        ogSiteName = "";
        var u = String(preview.url);
        if (!preview._enabled || !/^https?:\/\//i.test(u)) {
            _failed = true;
            return;
        }
        // false = the answer is already cached (or none will come); read it
        // now. true = linkPreviewResolved(u) follows.
        if (!preview._server.requestLinkPreview(u)) _load();
    }

    function _load() {
        var s = preview._server;
        var r = s ? s.linkPreview(String(preview.url)) : null;
        if (r && r.state === "pending") return;
        if (!r || r.state !== "ready") {
            _failed = true;
            return;
        }
        ogTitle = r.title || "";
        ogDescription = r.description || "";
        ogImage = r.image || "";
        ogSiteName = r.siteName || "";
    }

    // Known-platform sniff. Returns the platform-native video ID if
    // `url` is a YouTube (youtu.be / youtube.com / m.youtube.com)
    // link in any of the common shapes: watch?v=, shorts/, embed/,
    // live/, or a youtu.be short URL. Easy to extend with vimeo.com
    // / twitch.tv clips / etc.
    function _detectVideoId(u) {
        if (!u) return "";
        // youtu.be short links — path is the id.
        var m = /^https?:\/\/youtu\.be\/([A-Za-z0-9_-]{6,})/i.exec(u);
        if (m) return m[1];
        // youtube.com/watch?v=…
        m = /^https?:\/\/(?:www\.|m\.)?youtube\.com\/watch\?[^#]*?\bv=([A-Za-z0-9_-]{6,})/i.exec(u);
        if (m) return m[1];
        // youtube.com/shorts/…, /embed/…, /live/…
        m = /^https?:\/\/(?:www\.|m\.)?youtube\.com\/(?:shorts|embed|live)\/([A-Za-z0-9_-]{6,})/i.exec(u);
        if (m) return m[1];
        return "";
    }
}
