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

    // Trigger a re-fetch of thread replies when the MessageModel
    // ticks count. threadMessages is a function call, not a signal,
    // so we drive re-eval via this counter.
    property int _replyGen: 0
    Connections {
        target: serverManager.activeServer
            ? serverManager.activeServer.messageModel : null
        ignoreUnknownSignals: true
        function onCountChanged() { threadPanel._replyGen++; }
    }

    function openFor(eventId) {
        rootEventId = eventId;
        composerField.forceActiveFocus();
    }
    function closePanel() { rootEventId = ""; }

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
        width: Math.min(420, parent.width * 0.5)
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

                // Build [parent, ...replies] as a plain JS array.
                model: {
                    threadPanel._replyGen; // dependency
                    var s = serverManager.activeServer;
                    if (!s || !s.messageModel || !threadPanel.rootEventId)
                        return [];
                    var out = [];
                    var parent = s.messageModel.eventPreview(threadPanel.rootEventId);
                    if (parent && parent.sender !== undefined) {
                        parent.eventId = threadPanel.rootEventId;
                        parent._isParent = true;
                        out.push(parent);
                    }
                    var replies = s.messageModel.threadReplies(threadPanel.rootEventId);
                    for (var i = 0; i < replies.length; i++) {
                        replies[i]._isParent = false;
                        out.push(replies[i]);
                    }
                    return out;
                }

                delegate: Item {
                    width: ListView.view.width
                    height: row.implicitHeight + Theme.sp.s3 * 2

                    Rectangle {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.sp.s4
                        anchors.rightMargin: Theme.sp.s4
                        anchors.topMargin: Theme.sp.s2
                        anchors.bottomMargin: Theme.sp.s2
                        radius: Theme.r1
                        color: modelData._isParent ? Qt.rgba(Theme.accent.r,
                                   Theme.accent.g, Theme.accent.b, 0.06)
                                : "transparent"
                        border.width: modelData._isParent ? 1 : 0
                        border.color: Qt.rgba(Theme.accent.r, Theme.accent.g,
                                              Theme.accent.b, 0.5)

                        ColumnLayout {
                            id: row
                            anchors.fill: parent
                            anchors.margins: Theme.sp.s3
                            spacing: 2

                            RowLayout {
                                spacing: Theme.sp.s3
                                Layout.fillWidth: true
                                Text {
                                    text: modelData.sender
                                          || modelData.senderDisplayName
                                          || modelData.eventId || ""
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.fontSize.base
                                    font.weight: Theme.fontWeight.semibold
                                    color: Theme.fg0
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Text {
                                    text: {
                                        var d = new Date(modelData.timestamp);
                                        return d.toLocaleString(Qt.locale(), "h:mm ap");
                                    }
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.fontSize.xs
                                    color: Theme.fg3
                                }
                            }
                            Text {
                                text: modelData.body || ""
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.sm
                                color: Theme.fg1
                                wrapMode: Text.Wrap
                                Layout.fillWidth: true
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
        if (!serverManager.activeServer) return;
        serverManager.activeServer.sendThreadReply(rootEventId, body);
        composerField.text = "";
    }
}
