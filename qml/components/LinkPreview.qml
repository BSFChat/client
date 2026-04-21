import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// Per-link OpenGraph unfurl card. A MessageBubble instantiates one
// (or two) of these via a Repeater keyed on URLs detected in its body.
//
// Fetches the target page with XMLHttpRequest, regex-extracts the
// standard OpenGraph meta tags (og:title / og:description / og:image /
// og:site_name), and renders a Discord-style preview. Falls back to
// the <title> element if og:title is missing; hides the card entirely
// if the response carries nothing preview-worthy.
//
// Fetch is client-side — each user's app hits the target URL. Good
// enough for v1; if fingerprinting or cache duplication becomes an
// issue we'll route through a server-side unfurler later. Until then
// the cache is in-process (LinkPreview._cache) keyed by URL so two
// messages linking the same page don't refetch.
Rectangle {
    id: preview

    property url url

    // Parsed metadata. Populated by _fetch().
    property string ogTitle: ""
    property string ogDescription: ""
    property string ogImage: ""
    property string ogSiteName: ""

    // For known video platforms (YouTube for now — Vimeo/Twitch
    // trivial to extend) we short-circuit the OG fetch path with a
    // derived thumbnail + a landscape "video embed" layout. Click the
    // play overlay to open in browser; inline playback via WebEngine
    // is a follow-up.
    readonly property string _videoId: _detectVideoId(String(preview.url))
    readonly property bool isVideoEmbed: _videoId !== ""
    // 16:9 thumbnail pulled straight from YouTube's CDN — no fetch
    // needed. `hqdefault` always exists; maxres is spottier on older
    // videos. Prefer maxres with hqdefault as the Image fallback.
    readonly property string videoThumbUrl: isVideoEmbed
        ? "https://img.youtube.com/vi/" + _videoId + "/maxresdefault.jpg"
        : ""
    // Host extracted from `url` as a fallback site-name display.
    readonly property string hostName: {
        var s = String(preview.url);
        var m = s.match(/^https?:\/\/([^\/:]+)/i);
        return m ? m[1] : "";
    }

    readonly property bool ready: isVideoEmbed
        ? ogTitle.length > 0   // video cards are ready once the title arrives
        : (ogTitle.length > 0 || ogDescription.length > 0 || ogImage.length > 0)
    readonly property bool hasImage: ogImage.length > 0
    property bool _failed: false

    // Simple module-scoped JS cache. A failed fetch also caches (as
    // null) so we don't hammer the URL on every re-render.
    property var _cache: ({})

    visible: ready && !_failed
    // Two different shapes: video-embed cards are landscape with the
    // thumbnail as the hero; standard og cards are horizontal with a
    // square thumbnail on the right.
    implicitHeight: !ready ? 0
        : isVideoEmbed
            ? (videoCol.implicitHeight + Theme.sp.s4 * 2)
            : (hasImage ? 120 : previewCol.implicitHeight + Theme.sp.s4 * 2)
    implicitWidth: isVideoEmbed ? 480 : 420

    radius: Theme.r2
    color: Theme.bg2
    border.color: Theme.line
    border.width: 1
    clip: true

    // Left accent stripe — mirrors the reply-preamble chrome so
    // "quoted content" reads consistently whether it's another
    // message or a linked page. Hidden for video embeds; the card
    // there is hero-thumbnail-forward and doesn't need the bar.
    Rectangle {
        visible: !preview.isVideoEmbed
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
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
            radius: Theme.r1
            color: "black"
            clip: true

            Image {
                id: videoThumb
                anchors.fill: parent
                source: preview.videoThumbUrl
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                cache: true
                // YouTube returns a small "video thumbnail not found"
                // grey placeholder as a 200 for missing maxres — we
                // can't easily detect that without pixel-peeping,
                // but the difference is subtle enough to live with.
                // Older / unlisted videos fall back to hqdefault.
                onStatusChanged: {
                    if (status === Image.Error && preview._videoId !== "") {
                        source = "https://img.youtube.com/vi/"
                            + preview._videoId + "/hqdefault.jpg";
                    }
                }
            }

            // Dim scrim so the play button reads over any frame.
            Rectangle {
                anchors.fill: parent
                color: Qt.rgba(0, 0, 0, 0.25)
            }

            // Red circle play button in the YouTube brand colour.
            Rectangle {
                anchors.centerIn: parent
                width: 72; height: 72; radius: 36
                color: videoMouse.containsMouse
                    ? Qt.rgba(1, 0, 0, 0.95) : Qt.rgba(0, 0, 0, 0.7)
                border.color: "white"; border.width: 2
                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                Icon {
                    anchors.centerIn: parent
                    anchors.horizontalCenterOffset: 3
                    name: "play"
                    size: 28
                    color: "white"
                }
            }

            MouseArea {
                id: videoMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: Qt.openUrlExternally(preview.url)
            }
        }

        // Title + channel name under the thumbnail.
        Text {
            Layout.fillWidth: true
            text: preview.ogTitle
            color: Theme.accent
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.md
            font.weight: Theme.fontWeight.semibold
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: Qt.openUrlExternally(preview.url)
            }
        }
        Text {
            Layout.fillWidth: true
            text: preview.ogSiteName.length > 0
                ? preview.ogSiteName : preview.hostName
            visible: text.length > 0
            color: Theme.fg3
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.xs
            elide: Text.ElideRight
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
                    onClicked: Qt.openUrlExternally(preview.url)
                }
            }
            // Description — dimmer, up to 3 lines.
            Text {
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
            Layout.preferredWidth: 96
            Layout.preferredHeight: 96
            Layout.alignment: Qt.AlignVCenter
            radius: Theme.r1
            color: Theme.bg3
            clip: true

            Image {
                id: thumb
                anchors.fill: parent
                source: preview.ogImage
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                cache: true
                // If the thumbnail itself 404s or the origin blocks
                // hotlinking, hide the tile so the card doesn't look
                // broken — the text side still stands on its own.
                onStatusChanged: {
                    if (status === Image.Error) parent.visible = false;
                }
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: Qt.openUrlExternally(preview.url)
            }
        }
    }

    onUrlChanged: _fetch()

    function _fetch() {
        var u = String(preview.url);
        if (!u || u.indexOf("http") !== 0) {
            _failed = true;
            return;
        }
        // Cache hit.
        if (_cache[u] !== undefined) {
            var c = _cache[u];
            if (!c) { _failed = true; return; }
            _apply(c);
            return;
        }
        // Known video platforms get the oEmbed fast-path — returns
        // tiny JSON with title + author name, no HTML scanning. The
        // thumbnail for the video card is derived from the video id
        // directly so we don't need it from oEmbed.
        if (isVideoEmbed) {
            _fetchYoutubeOEmbed(u);
            return;
        }
        // Up to 3 redirect hops — covers http→https→www-prefix chains
        // without risking redirect loops.
        _fetchOne(u, u, 3);
    }

    // YouTube oEmbed (https://oembed.com/). No auth required, returns
    // `{title, author_name, author_url, thumbnail_url, ...}`. Much
    // more reliable than scraping the rendered HTML — YouTube's own
    // page puts og tags past byte 600KB so a bounded regex scan
    // misses them entirely.
    function _fetchYoutubeOEmbed(origUrl) {
        var api = "https://www.youtube.com/oembed?format=json&url="
            + encodeURIComponent(origUrl);
        var xhr = new XMLHttpRequest();
        xhr.open("GET", api);
        xhr.setRequestHeader("Accept", "application/json");
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return;
            if (xhr.status < 200 || xhr.status >= 400) {
                _cache[origUrl] = null;
                _failed = true;
                return;
            }
            try {
                var j = JSON.parse(xhr.responseText);
                var parsed = {
                    title: j.title || "",
                    siteName: j.author_name || "YouTube",
                    description: "",
                    image: "",
                    ready: !!j.title
                };
                _cache[origUrl] = parsed.ready ? parsed : null;
                if (parsed.ready) _apply(parsed);
                else _failed = true;
            } catch (e) {
                _cache[origUrl] = null;
                _failed = true;
            }
        };
        xhr.send();
    }

    function _fetchOne(origUrl, currentUrl, redirectsLeft) {
        var xhr = new XMLHttpRequest();
        xhr.open("GET", currentUrl);
        xhr.setRequestHeader("Accept", "text/html,application/xhtml+xml");
        // Some sites (Reddit, LinkedIn, Twitter) return a completely
        // different body — or a block — to non-browser User-Agents.
        // Claim a modern Safari UA so we get the OpenGraph-rich HTML
        // and not a login wall or raw JSON.
        xhr.setRequestHeader(
            "User-Agent",
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
            + "AppleWebKit/605.1.15 (KHTML, like Gecko) "
            + "Version/17.0 Safari/605.1.15");
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return;

            // Follow 3xx manually. Qt's QML XHR doesn't follow
            // redirects by default, so reddit.com (→ www.reddit.com)
            // and every other root-domain short link would otherwise
            // land us with an empty body.
            if (xhr.status >= 300 && xhr.status < 400 && redirectsLeft > 0) {
                var loc = xhr.getResponseHeader("Location")
                       || xhr.getResponseHeader("location");
                if (loc) {
                    var next = _absolutize(loc, currentUrl);
                    _fetchOne(origUrl, next, redirectsLeft - 1);
                    return;
                }
            }

            if (xhr.status < 200 || xhr.status >= 400) {
                _cache[origUrl] = null;
                _failed = true;
                return;
            }
            var ct = xhr.getResponseHeader("content-type") || "";
            if (ct.indexOf("text/html") < 0 && ct.indexOf("xhtml") < 0) {
                _cache[origUrl] = null;
                _failed = true;
                return;
            }
            // Limit the regex-scan prefix to avoid running big regexes
            // over a whole megabyte of body. 64KB used to be enough
            // for most sites' <head>, but modern JS-heavy pages
            // (YouTube, Reddit) push og tags past 500KB; widen to
            // 512KB so we still catch them. YouTube specifically
            // takes the oEmbed fast-path above and never lands here.
            var html = xhr.responseText.substring(0, 524288);

            // Detect bot-challenge / verification pages. Client-side
            // fetches from a desktop app don't pass Cloudflare
            // Turnstile, PerimeterX, etc. — the response is a short
            // "please wait" shim with a placeholder title but no real
            // content. Render nothing rather than a misleading card.
            // Fix is a server-side unfurler; this is the interim stop.
            if (_looksLikeChallenge(html)) {
                _cache[origUrl] = null;
                _failed = true;
                return;
            }

            var parsed = _parseOg(html, currentUrl);
            _cache[origUrl] = parsed.ready ? parsed : null;
            if (parsed.ready) _apply(parsed);
            else _failed = true;
        };
        xhr.send();
    }

    function _apply(data) {
        ogTitle = data.title || "";
        ogDescription = data.description || "";
        ogImage = data.image || "";
        ogSiteName = data.siteName || "";
    }

    // Regex-extract the OpenGraph set plus sensible fallbacks. This is
    // deliberately not a real HTML parser — it just picks out the
    // handful of <meta> tags we care about. Most modern sites emit
    // them in the same `<meta property="og:*" content="...">` shape
    // with quote variation that the regex tolerates.
    function _parseOg(html, urlStr) {
        function pick(prop) {
            // Property-first or content-first ordering both seen in
            // the wild. Match either.
            var re = new RegExp(
                '<meta[^>]+(?:property|name)=["\']' + prop
                + '["\'][^>]*content=["\']([^"\']+)["\']', 'i');
            var m = re.exec(html);
            if (m) return _decode(m[1]);
            re = new RegExp(
                '<meta[^>]+content=["\']([^"\']+)["\'][^>]*(?:property|name)=["\']'
                + prop + '["\']', 'i');
            m = re.exec(html);
            return m ? _decode(m[1]) : "";
        }
        var out = {};
        out.title = pick("og:title");
        out.description = pick("og:description");
        out.image = pick("og:image");
        out.siteName = pick("og:site_name");

        // Fallbacks: <title> tag + <meta name="description">.
        if (!out.title) {
            var tm = /<title[^>]*>([^<]+)<\/title>/i.exec(html);
            if (tm) out.title = _decode(tm[1].trim());
        }
        if (!out.description) out.description = pick("description");

        // Relative or protocol-relative image URLs — turn into
        // absolute against the requested url.
        if (out.image) out.image = _absolutize(out.image, urlStr);

        out.ready = !!(out.title || out.description || out.image);
        return out;
    }

    // Minimal HTML entity decode — covers the common ampersand /
    // quote / apostrophe / less / greater entities that show up in
    // og:title values.
    function _decode(s) {
        return s
            .replace(/&amp;/g, "&")
            .replace(/&quot;/g, '"')
            .replace(/&#39;/g, "'")
            .replace(/&apos;/g, "'")
            .replace(/&lt;/g, "<")
            .replace(/&gt;/g, ">");
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

    // Returns true when the fetched HTML is obviously a bot-challenge
    // shim (Cloudflare, PerimeterX, DataDome, generic "please wait")
    // rather than real page content. Matching is deliberately broad —
    // a false positive at worst means no preview, which is fine since
    // the alternative is showing "Please wait for verification" as a
    // title. A properly-signed server-side fetcher won't hit these.
    function _looksLikeChallenge(html) {
        var t = /<title[^>]*>([^<]+)<\/title>/i.exec(html);
        if (t) {
            var title = t[1].toLowerCase();
            if (title.indexOf("just a moment") >= 0) return true;
            if (title.indexOf("please wait") >= 0) return true;
            if (title.indexOf("verify you are human") >= 0) return true;
            if (title.indexOf("attention required") >= 0) return true;
            if (title.indexOf("access denied") >= 0) return true;
            if (title.indexOf("verification") >= 0) return true;
            if (title.indexOf("checking your browser") >= 0) return true;
        }
        // Body-level giveaways that survive even when the title is
        // generic or missing.
        if (/cf-chl-page|cf_chl_opt|_cf_chl_|challenge-platform/i.test(html)) return true;
        if (/perimeterx\.net|pxhd-captcha|_pxAppId/i.test(html)) return true;
        if (/datadome\.co|ddkey|dd_protected/i.test(html)) return true;
        return false;
    }

    function _absolutize(maybeRel, baseUrl) {
        if (/^https?:\/\//i.test(maybeRel)) return maybeRel;
        if (maybeRel.indexOf("//") === 0) {
            var scheme = baseUrl.match(/^(https?:)/i);
            return (scheme ? scheme[1] : "https:") + maybeRel;
        }
        var base = baseUrl.match(/^(https?:\/\/[^\/]+)/i);
        if (!base) return maybeRel;
        if (maybeRel.indexOf("/") === 0) return base[1] + maybeRel;
        // Strip trailing filename from the base path before appending.
        var path = baseUrl.replace(base[1], "").replace(/[^\/]*$/, "");
        return base[1] + path + maybeRel;
    }
}
