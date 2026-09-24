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

    // Edge of the composer's send button. Declared up here, away from the
    // layout that uses it, because the composer strip sizes itself from
    // this number AND contains the button: reading the button's own
    // laid-out height from its container would close a binding loop.
    // 44 on touch is the Apple HIG / Material minimum; desktop stays
    // compact, matching MessageInput.
    readonly property int sendButtonSize: Theme.isMobile ? 44 : 28

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
                Layout.preferredHeight: Theme.isMobile ? 52 : 48
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
                        // The panel is full-screen on mobile, so this is
                        // the only way out of it; at 28 px it was well
                        // under the 44 pt minimum.
                        Layout.preferredWidth: Theme.isMobile ? Theme.touchTarget : 28
                        Layout.preferredHeight: Theme.isMobile ? Theme.touchTarget : 28
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
                // Whichever of the field and the send button is taller.
                // On a phone the button is 44 pt and the field is not, so
                // binding this to the field alone clipped the button.
                //
                // Against `threadPanel.sendButtonSize`, NOT against the
                // button's own `height`: the button is inside a RowLayout
                // that fills this rectangle, so reading its laid-out height
                // here would be a binding loop (height → layout → height).
                Layout.preferredHeight: Math.max(composerField.implicitHeight,
                                                 threadPanel.sendButtonSize)
                    + Theme.sp.s4 * 2
                color: Theme.bg2
                border.width: 0

                Rectangle {
                    anchors.top: parent.top
                    width: parent.width; height: 1; color: Theme.line
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.topMargin: Theme.sp.s3
                    anchors.bottomMargin: Theme.sp.s3
                    // The panel is full-width on a phone, so this composer is
                    // a full-width surface and keeps the same gutter as the
                    // main one — see Theme.mobileGutter.
                    anchors.leftMargin: Theme.isMobile ? Theme.mobileGutter
                                                       : Theme.sp.s3
                    anchors.rightMargin: Theme.isMobile ? Theme.mobileGutter
                                                        : Theme.sp.s3
                    spacing: Theme.sp.s3

                    TextField {
                        id: composerField
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
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
                        // The numeric keypad's Enter is a different key and
                        // raises a different signal; on a hardware keyboard
                        // it is the one a lot of people actually press.
                        Keys.onEnterPressed: threadPanel._send()
                    }

                    // Send — the ONLY visible way to post a thread reply.
                    //
                    // There was none: the composer's entire send path was
                    // Keys.onReturnPressed. That is fine on a desktop, where
                    // the main composer has taught you that Return sends and
                    // shows a send button next to it anyway. On a phone it is
                    // a dead end — the software keyboard's return key is a
                    // newline glyph, this is a single-line TextField so
                    // pressing it does something invisible, and there is
                    // nothing on screen that says "post this". A reviewer
                    // opening a thread finds a text box they cannot submit.
                    //
                    // Same shape, same arming rule and the same accent
                    // fade-in as MessageInput's send button, so the two read
                    // as the same control: preeditText counts, because an
                    // Android IME holds the first characters there and a
                    // button that stays greyed out while you type reads as
                    // broken.
                    Rectangle {
                        id: sendReplyBtn
                        readonly property bool armed:
                            (composerField.text.trim().length > 0
                             || composerField.preeditText.length > 0)
                            && threadPanel.rootEventId !== ""
                        Layout.preferredWidth: threadPanel.sendButtonSize
                        Layout.preferredHeight: threadPanel.sendButtonSize
                        Layout.alignment: Qt.AlignVCenter
                        radius: Theme.r1
                        color: sendReplyMouse.containsMouse && armed
                            ? Theme.accentDim : Theme.accent
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                        // Faded rather than hidden so the row does not
                        // pop-reflow on the first keystroke.
                        opacity: armed ? 1.0 : 0.0
                        scale:   armed ? 1.0 : 0.8
                        Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }
                        Behavior on scale {
                            NumberAnimation { duration: Theme.motion.fastMs
                                              easing.type: Easing.BezierSpline
                                              easing.bezierCurve: Theme.motion.bezier }
                        }

                        Accessible.role: Accessible.Button
                        Accessible.name: "Send reply"
                        Accessible.description: sendReplyBtn.armed
                            ? "Post this reply into the thread"
                            : "Nothing to send yet"
                        Accessible.onPressAction: if (sendReplyBtn.armed)
                            threadPanel._send()

                        Icon {
                            anchors.centerIn: parent
                            name: "send"
                            size: 14
                            color: Theme.onAccent
                        }

                        MouseArea {
                            id: sendReplyMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: sendReplyBtn.armed
                                ? Qt.PointingHandCursor : Qt.ArrowCursor
                            enabled: sendReplyBtn.armed
                            onClicked: threadPanel._send()
                        }
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
