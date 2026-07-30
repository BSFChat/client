import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// Message search. Bound to ⌘K.
//
// Backed by the server's FTS5 index (POST /_matrix/client/v3/search), so it
// spans every channel the user can see rather than only what the current room
// has paginated into memory. Permission filtering and query sanitising both
// happen server-side — there is deliberately nothing to escape here, and a user
// typing `"` or `*` gets zero matches rather than an error, because the server
// treats FTS5 metacharacters as word separators.
//
// Results carry a roomId, so activating one switches channel before scrolling.
Popup {
    id: searchPopup
    anchors.centerIn: Overlay.overlay
    // Near-fullscreen on mobile where a 70% popup leaves too little
    // room for results + the on-screen keyboard.
    width: Theme.isMobile
        ? (parent ? parent.width - 16 : 640)
        : Math.min(parent ? parent.width * 0.7 : 640, 640)
    height: Theme.isMobile
        ? (parent ? parent.height - 80 : 520)
        : Math.min(parent ? parent.height * 0.7 : 480, 520)
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    // (roomId, eventId) rather than eventId alone: a hit is usually in some
    // other channel, so the handler has to switch rooms first.
    signal resultActivated(string roomId, string eventId)

    property var results: []
    // Server's total match count, which can exceed results.length when the
    // result set is paginated.
    property int totalMatches: 0
    // Terms the server actually searched, for emphasis in the rendered bodies.
    // These are the SERVER's tokenisation, not the raw input.
    property var highlights: []
    // Opaque pagination token; "" means there is no further page.
    property string nextBatch: ""
    property bool searching: false
    // Non-empty when the last attempt failed. Rendered in place of the results.
    property string errorText: ""
    // Guards against a "load more" firing repeatedly while its page is in
    // flight (the ListView's atYEnd goes true for the whole scroll).
    property bool loadingMore: false

    readonly property var _server: serverManager.activeServer

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1
    }

    // Debounce: one request per keystroke would put a full-text query on the
    // server for every character of a word.
    Timer {
        id: debounce
        interval: 220
        onTriggered: searchPopup._issue(searchField.text, "")
    }

    function _issue(text, batch) {
        var s = searchPopup._server;
        if (!s || !s.searchMessages) {
            searchPopup.errorText = "Not connected to a server.";
            return;
        }
        var q = text.trim();
        if (q.length === 0) {
            searchPopup._reset();
            return;
        }
        if (batch === "") {
            // New query: drop the old page immediately so the list can't show
            // results from a term the user has already moved on from.
            searchPopup.results = [];
            searchPopup.totalMatches = 0;
            searchPopup.nextBatch = "";
            searchPopup.searching = true;
        } else {
            searchPopup.loadingMore = true;
        }
        searchPopup.errorText = "";
        s.searchMessages(q, 30, batch);
    }

    function _reset() {
        debounce.stop();
        searchPopup.results = [];
        searchPopup.totalMatches = 0;
        searchPopup.highlights = [];
        searchPopup.nextBatch = "";
        searchPopup.errorText = "";
        searchPopup.searching = false;
        searchPopup.loadingMore = false;
    }

    function runSearch(text) {
        if (text.trim().length === 0) {
            _reset();
            return;
        }
        debounce.restart();
    }

    function loadMore() {
        if (searchPopup.nextBatch === "") return;
        if (searchPopup.searching || searchPopup.loadingMore) return;
        _issue(searchField.text, searchPopup.nextBatch);
    }

    // Bound to the live connection so a server switch while the popup is open
    // rewires rather than leaking the old connection's results in.
    Connections {
        target: searchPopup._server
        ignoreUnknownSignals: true
        function onSearchResultsReady(rows, total, terms, batch, appended) {
            searchPopup.searching = false;
            searchPopup.loadingMore = false;
            searchPopup.errorText = "";
            searchPopup.results = appended
                ? searchPopup.results.concat(rows) : rows;
            searchPopup.totalMatches = total;
            searchPopup.highlights = terms;
            searchPopup.nextBatch = batch;
        }
        function onSearchErrored(message) {
            searchPopup.searching = false;
            searchPopup.loadingMore = false;
            searchPopup.results = [];
            searchPopup.totalMatches = 0;
            searchPopup.nextBatch = "";
            // The message is already user-facing — SearchParser maps the
            // server's errcode/error (and bare HTTP statuses) into plain
            // language, so the box never just goes silent on a failure.
            searchPopup.errorText = message;
        }
    }

    onOpened: {
        searchField.text = "";
        _reset();
        searchField.forceActiveFocus();
    }
    onClosed: _reset()

    // Wrap the search terms in <b> for emphasis. Escapes first, so a body
    // containing markup renders as text — the results list is never a place to
    // interpret someone else's HTML.
    function _renderBody(body) {
        var out = String(body)
            .replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
        var terms = searchPopup.highlights;
        for (var i = 0; i < terms.length; i++) {
            var t = String(terms[i]);
            if (t.length === 0) continue;
            // Escape regex metacharacters in the term itself. The server's
            // tokeniser strips them, but never rely on that from here.
            var safe = t.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
            out = out.replace(new RegExp("(" + safe + ")", "gi"),
                              "<b>$1</b>");
        }
        return out;
    }

    contentItem: ColumnLayout {
        spacing: 0

        // Search field — accent border on focus (also serves as our
        // "focus ring" prototype for the accessibility pass).
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            radius: Theme.r2
            color: Theme.bg2
            border.width: 1
            border.color: searchField.activeFocus ? Theme.accent : Theme.line
            Behavior on border.color { ColorAnimation { duration: Theme.motion.fastMs } }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.sp.s4
                anchors.rightMargin: Theme.sp.s4
                spacing: Theme.sp.s3

                Icon {
                    name: "search"
                    size: 16
                    color: Theme.fg2
                }

                TextField {
                    id: searchField
                    Layout.fillWidth: true
                    placeholderText: "Search all channels…"
                    background: Item {}
                    color: Theme.fg0
                    placeholderTextColor: Theme.fg3
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.base
                    selectByMouse: true
                    onTextChanged: searchPopup.runSearch(text)
                    // Enter re-issues immediately rather than waiting out the
                    // debounce, and opens the top hit once results are in.
                    Keys.onReturnPressed: {
                        if (searchPopup.results.length > 0) {
                            searchPopup._activate(0);
                        } else if (!searchPopup.searching) {
                            searchPopup._issue(searchField.text, "");
                        }
                    }
                    Keys.onDownPressed: resultsList.forceActiveFocus()
                }

                BusyIndicator {
                    running: searchPopup.searching
                    visible: running
                    implicitWidth: 18
                    implicitHeight: 18
                }

                Text {
                    visible: !searchPopup.searching
                             && searchPopup.errorText === ""
                             && searchPopup.totalMatches > 0
                    // Distinguish "showing all of them" from "showing the first
                    // page of many", so the count never looks like a lie.
                    text: searchPopup.results.length < searchPopup.totalMatches
                          ? searchPopup.results.length + " of "
                            + searchPopup.totalMatches
                          : searchPopup.totalMatches + " match"
                            + (searchPopup.totalMatches === 1 ? "" : "es")
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xs
                    font.letterSpacing: Theme.trackWide.xs
                    color: Theme.fg3
                }
            }
        }

        Item { Layout.preferredHeight: Theme.sp.s3 }

        // Results list.
        ListView {
            id: resultsList
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: searchPopup.results.length > 0
            clip: true
            spacing: 2
            model: searchPopup.results
            ScrollBar.vertical: ThemedScrollBar {}
            boundsBehavior: Flickable.StopAtBounds
            // Pagination: pull the next page as the user reaches the bottom.
            onAtYEndChanged: if (atYEnd) searchPopup.loadMore()

            footer: Item {
                width: ListView.view ? ListView.view.width : 0
                height: searchPopup.loadingMore ? 32 : 0
                visible: searchPopup.loadingMore
                BusyIndicator {
                    anchors.centerIn: parent
                    running: searchPopup.loadingMore
                    implicitWidth: 20
                    implicitHeight: 20
                }
            }

            delegate: Rectangle {
                width: ListView.view.width
                height: 62
                radius: Theme.r1
                color: resultMouse.containsMouse ? Theme.bg2 : "transparent"
                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.sp.s4
                    anchors.rightMargin: Theme.sp.s4
                    anchors.topMargin: Theme.sp.s2
                    anchors.bottomMargin: Theme.sp.s2
                    spacing: 2

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.sp.s3
                        Text {
                            text: modelData.sender
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.base
                            font.weight: Theme.fontWeight.semibold
                            color: Theme.fg0
                            elide: Text.ElideRight
                        }
                        // Which channel the hit is in — the whole point of a
                        // server-side search is that it isn't the current one.
                        Text {
                            visible: text.length > 1
                            text: modelData.roomName ? "#" + modelData.roomName : ""
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.xs
                            color: Theme.accent
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Text {
                            text: {
                                var d = new Date(modelData.timestamp);
                                return d.toLocaleString(Qt.locale(), "MMM d, h:mm ap");
                            }
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.xs
                            color: Theme.fg3
                        }
                    }
                    Text {
                        text: searchPopup._renderBody(modelData.body)
                        textFormat: Text.StyledText
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                        color: Theme.fg1
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        maximumLineCount: 2
                        wrapMode: Text.Wrap
                    }
                }

                MouseArea {
                    id: resultMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: searchPopup._activate(index)
                }
            }
        }

        // Error / empty / idle states. One slot, three messages — the error one
        // matters most: a failed search must say so rather than look like
        // "nothing matched".
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: searchPopup.results.length === 0
            ColumnLayout {
                anchors.centerIn: parent
                spacing: Theme.sp.s2
                width: parent.width - 2 * Theme.sp.s5

                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: {
                        if (searchPopup.errorText !== "") return searchPopup.errorText;
                        if (searchPopup.searching) return "Searching…";
                        if (searchField.text.trim().length > 0)
                            return "No messages matched.";
                        return "Search messages across every channel you can see.";
                    }
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    color: Theme.fg3
                }
                Button {
                    Layout.alignment: Qt.AlignHCenter
                    visible: searchPopup.errorText !== ""
                             && searchField.text.trim().length > 0
                    text: "Try again"
                    onClicked: searchPopup._issue(searchField.text, "")
                }
            }
        }
    }

    function _activate(idx) {
        if (idx < 0 || idx >= results.length) return;
        var r = results[idx];
        resultActivated(r.roomId, r.eventId);
        close();
    }
}
