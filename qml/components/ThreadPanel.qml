import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import BSFChat

// Side-drawer thread view. Slides in from the right when the user
// opens a thread off a parent message. Shows the parent + threaded
// replies and a scoped composer that tags outgoing messages with
// m.relates_to.rel_type=m.thread.
//
// Parent is MessageView (or its root) — the drawer is anchored to
// the right edge and takes roughly 380px. A semi-transparent backdrop
// over the main chat catches click-to-close; the panel itself is a
// bg1 rail with a 1px left border.
Item {
    id: threadPanel
    anchors.fill: parent
    visible: rootEventId !== ""

    // Root of the thread — populated when opened. Empty ⇒ panel hidden.
    property string rootEventId: ""

    // A @mention anchor inside a reply was clicked. The drawer has no
    // profile card of its own, so the host (MessageView) opens the one
    // it already owns.
    signal userLinkClicked(string userId, string displayName)

    // Live view of this thread over the room's MessageModel (U-M8).
    //
    // This used to be a plain JS array rebuilt from eventPreview() +
    // threadReplies() every time the room's message count ticked, which
    // meant an edit or a reaction inside a thread never appeared (neither
    // changes the count), and every rebuild handed the ListView a new
    // model object — destroying every delegate and throwing the drawer's
    // scroll position back to the top mid-conversation.
    //
    // A QSortFilterProxyModel is the same rows with the same roles, kept
    // in step by the source model's own dataChanged / rowsInserted /
    // rowsRemoved. An edit repaints one delegate and nothing moves.
    readonly property var threadModel: {
        var s = serverManager.activeServer;
        if (!s || !s.messageModel || threadPanel.rootEventId === "") return null;
        return s.messageModel.threadModel(threadPanel.rootEventId);
    }

    function openFor(eventId) {
        threadPanel._lastRoomKey = threadPanel.roomKey;
        rootEventId = eventId;
        composerField.forceActiveFocus();
    }
    function closePanel() {
        rootEventId = "";
        composerField.text = "";
    }

    // ── Close on room AND server change (U-H4) ───────────────────────
    //
    // The panel used to stay open across both. `_send()` posts through
    // whatever `serverManager.activeServer` is at the time, tagged with a
    // rootEventId from the room the thread was opened in — so switching
    // channel with the drawer open and hitting Enter filed the reply into
    // the new room, threaded onto an event that is not in it.
    //
    // Keyed on (server, room): a server switch leaves every connection's
    // activeRoomId untouched, so watching the room id alone misses it
    // entirely (same reasoning as MessageView's roomContextKey).
    readonly property string roomKey: {
        var s = serverManager.activeServer;
        if (!s) return "";
        return s.serverUrl + "\u001f" + s.activeRoomId;
    }
    property string _lastRoomKey: ""
    onRoomKeyChanged: {
        if (threadPanel.roomKey === threadPanel._lastRoomKey) return;
        threadPanel._lastRoomKey = threadPanel.roomKey;
        if (threadPanel.rootEventId !== "") threadPanel.closePanel();
    }

    // Click backdrop to dismiss (like a drawer).
    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: 0.25
        MouseArea { anchors.fill: parent; onClicked: threadPanel.closePanel() }
    }

    // Panel chrome — right-anchored rail.
    Rectangle {
        id: panelRoot
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        // 50% of the main area on desktop; full-width on mobile where
        // a side drawer over a narrow phone screen is unusable.
        width: Theme.isMobile ? parent.width : Math.min(420, parent.width * 0.5)
        color: Theme.bg1
        border.width: 0

        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 1
            color: Theme.line
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // Header — title + close button.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 48
                color: Theme.bg1

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.sp.s5
                    anchors.rightMargin: Theme.sp.s3
                    spacing: Theme.sp.s3

                    Icon { name: "forward"; size: 14; color: Theme.accent }
                    Text {
                        text: "THREAD"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                        font.weight: Theme.fontWeight.semibold
                        font.letterSpacing: Theme.trackWidest.xs
                        color: Theme.fg2
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        Layout.preferredWidth: 28
                        Layout.preferredHeight: 28
                        radius: Theme.r1
                        color: closeMouse.containsMouse ? Theme.bg3 : "transparent"
                        Icon {
                            anchors.centerIn: parent
                            name: "x"; size: 14
                            color: closeMouse.containsMouse ? Theme.fg0 : Theme.fg2
                        }
                        MouseArea {
                            id: closeMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: threadPanel.closePanel()
                        }
                    }
                }

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width; height: 1; color: Theme.line
                }
            }

            // Parent message preview + thread replies.
            ListView {
                id: threadList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: Theme.sp.s2
                ScrollBar.vertical: ThemedScrollBar {}
                boundsBehavior: Flickable.StopAtBounds

                // [root, ...replies], oldest-first, straight off the
                // model — the proxy accepts the thread's root message as
                // well as its children, so the ordering that used to be
                // assembled by hand falls out of the source order.
                model: threadPanel.threadModel

                delegate: Item {
                    id: threadRow
                    width: ListView.view.width
                    height: row.implicitHeight + Theme.sp.s3 * 2

                    required property string eventId
                    required property string senderDisplayName
                    required property string sender
                    required property string body
                    required property string formattedBody
                    required property double timestamp
                    required property bool mentionsMe
                    required property bool mentionsRoom

                    readonly property bool isParent:
                        threadRow.eventId === threadPanel.rootEventId

                    // Same rule the timeline uses (see MessageView.qml's
                    // MessageBubble): a reply naming you or the room is
                    // worth spotting at a glance. The root is the message
                    // the thread hangs off, not a reply, so it never gets
                    // the marker even when it does name you.
                    readonly property bool highlight:
                        !threadRow.isParent
                        && (threadRow.mentionsMe || threadRow.mentionsRoom)

                    Rectangle {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.sp.s4
                        anchors.rightMargin: Theme.sp.s4
                        anchors.topMargin: Theme.sp.s2
                        anchors.bottomMargin: Theme.sp.s2
                        radius: Theme.r1
                        color: threadRow.isParent ? Qt.rgba(Theme.accent.r,
                                   Theme.accent.g, Theme.accent.b, 0.06)
                                : threadRow.highlight
                                  ? Qt.rgba(Theme.warn.r, Theme.warn.g,
                                            Theme.warn.b, 0.10)
                                  : "transparent"
                        border.width: threadRow.isParent ? 1 : 0
                        border.color: Qt.rgba(Theme.accent.r, Theme.accent.g,
                                              Theme.accent.b, 0.5)

                        // Left bar, matching the timeline's mention marker.
                        Rectangle {
                            visible: threadRow.highlight
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 2
                            radius: Theme.r1
                            color: Theme.warn
                        }

                        ColumnLayout {
                            id: row
                            anchors.fill: parent
                            anchors.margins: Theme.sp.s3
                            spacing: 2

                            RowLayout {
                                spacing: Theme.sp.s3
                                Layout.fillWidth: true
                                Text {
                                    text: threadRow.senderDisplayName
                                          || threadRow.sender
                                          || threadRow.eventId
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.fontSize.base
                                    font.weight: Theme.fontWeight.semibold
                                    color: Theme.fg0
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Text {
                                    text: {
                                        var d = new Date(threadRow.timestamp);
                                        return d.toLocaleString(Qt.locale(), "h:mm ap");
                                    }
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.fontSize.xs
                                    color: Theme.fg3
                                }
                            }
                            Text {
                                id: bodyText
                                // MessageModel bakes the highlighted mention
                                // anchors into formattedBody, so rendering the
                                // plain body here would print the bare
                                // "@Name" token and lose the highlight the
                                // timeline shows. The parent preview carries
                                // no markup, so it stays PlainText.
                                readonly property string html:
                                    threadRow.formattedBody
                                text: html !== "" ? html : threadRow.body
                                textFormat: html !== "" ? Text.RichText
                                                        : Text.PlainText
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.sm
                                color: Theme.fg1
                                wrapMode: Text.Wrap
                                Layout.fillWidth: true
                                onLinkActivated: (link) => {
                                    // Same routing MessageBubble does, so a
                                    // link behaves identically in the drawer.
                                    if (link.indexOf("bsfchat://user/") === 0) {
                                        var uid = decodeURIComponent(link.substring(
                                            "bsfchat://user/".length));
                                        threadPanel.userLinkClicked(uid, uid);
                                    } else if (link.indexOf("bsfchat://channel/") === 0) {
                                        if (serverManager.activeServer)
                                            serverManager.activeServer.activateRoomByName(
                                                link.substring("bsfchat://channel/".length));
                                    } else if (link.indexOf("bsfchat://message/") === 0) {
                                        serverManager.openMessageLink(link);
                                    } else {
                                        Qt.openUrlExternally(link);
                                    }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.NoButton
                                    cursorShape: bodyText.hoveredLink
                                        ? Qt.PointingHandCursor : Qt.ArrowCursor
                                }
                            }
                        }
                    }
                }
            }

            // Thread-scoped composer — posts replies into the thread.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: composerField.implicitHeight
                    + Theme.sp.s4 * 2
                color: Theme.bg2
                border.width: 0

                Rectangle {
                    anchors.top: parent.top
                    width: parent.width; height: 1; color: Theme.line
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.sp.s3
                    spacing: Theme.sp.s3

                    TextField {
                        id: composerField
                        Layout.fillWidth: true
                        placeholderText: "Reply in thread…"
                        color: Theme.fg0
                        placeholderTextColor: Theme.fg3
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.base
                        selectByMouse: true
                        background: Rectangle {
                            color: Theme.bg0
                            radius: Theme.r2
                            border.color: composerField.activeFocus ? Theme.accent : Theme.line
                            border.width: 1
                        }
                        leftPadding: Theme.sp.s4
                        rightPadding: Theme.sp.s4
                        topPadding: Theme.sp.s3
                        bottomPadding: Theme.sp.s3
                        Keys.onReturnPressed: threadPanel._send()
                    }
                }
            }
        }
    }

    function _send() {
        var body = composerField.text.trim();
        if (body.length === 0) return;
        if (rootEventId === "") return;
        // Belt and braces alongside onRoomKeyChanged: if anything ever
        // re-opens the panel without going through openFor, a reply still
        // cannot be posted from a context the thread does not belong to.
        if (threadPanel.roomKey !== threadPanel._lastRoomKey) {
            threadPanel.closePanel();
            return;
        }
        if (!serverManager.activeServer) return;
        serverManager.activeServer.sendThreadReply(rootEventId, body);
        composerField.text = "";
    }
}
