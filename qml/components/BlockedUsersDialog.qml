import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// "Who am I blocking?" — the managed list, with an unblock on every row.
//
// A block has to be undoable somewhere the user can find without remembering
// who they blocked, because the whole point of blocking is that the person
// disappears: they are filtered out of sync, so there is no message to
// right-click and no member row to open. This list is the only way back.
//
// ── Why there is a Refresh button and a timestamp ────────────────────────
//
// The block list is account data, and this server sends no account_data
// section in /sync. Nothing pushes it. A block made on a phone does not
// appear on a desktop until the desktop asks, and it asks at three moments:
// when the session connects, after one of its own writes, and when this pane
// opens.
//
// That is a real limitation and this pane says so rather than implying a
// liveness it does not have. The line under the list is the honest version of
// "synced ✓": it says when this client last looked, and the button next to it
// is how you make it look again. Inventing a poll instead would spend a
// request a minute on a document that changes twice a year and STILL be
// stale between ticks.
Popup {
    id: blockedDialog

    readonly property var conn: serverManager.activeServer
    readonly property var blockModel: conn ? conn.blockedUsersModel : null

    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    width: Math.min(460, (parent ? parent.width : 460) - 32)
    height: Math.min(implicitHeight, (parent ? parent.height : 800) - 32)
    implicitHeight: 520
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: Theme.sp.s7

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0.0; to: 1.0; duration: 150; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: 0.95; to: 1.0; duration: 150; easing.type: Easing.OutCubic }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1.0; to: 0.0; duration: 100; easing.type: Easing.InCubic }
        NumberAnimation { property: "scale"; from: 1.0; to: 0.95; duration: 100; easing.type: Easing.InCubic }
    }

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1
    }

    // Opening the pane is one of the three moments the list is re-read. It is
    // also the one the user can trigger deliberately, which is why the button
    // below exists as well.
    onAboutToShow: if (blockedDialog.conn) blockedDialog.conn.refreshBlockedUsers()

    // "Checked 4 minutes ago". Recomputed off `lastRefreshedMs` rather than a
    // ticking timer: the only thing that can make it wrong is time passing
    // while the pane sits open, and a pane nobody is looking at does not need
    // a repaint every second.
    function agoText(ms) {
        if (!ms) return "not checked yet";
        var secs = Math.max(0, Math.round((Date.now() - ms) / 1000));
        if (secs < 45) return "just now";
        var mins = Math.round(secs / 60);
        if (mins < 60) return mins + (mins === 1 ? " minute ago" : " minutes ago");
        var hours = Math.round(mins / 60);
        return hours + (hours === 1 ? " hour ago" : " hours ago");
    }

    contentItem: ColumnLayout {
        spacing: Theme.sp.s4

        // ── Header ──
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.sp.s3
            Text {
                Layout.fillWidth: true
                text: "Blocked accounts"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xl
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackTight.xl
                color: Theme.fg0
            }
            Text {
                visible: blockedDialog.blockModel !== null
                    && blockedDialog.blockModel.loaded
                text: blockedDialog.blockModel
                    ? blockedDialog.blockModel.count + " of 1000"
                    : ""
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                color: Theme.fg3
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.line }

        Text {
            Layout.fillWidth: true
            text: "You do not see messages, mentions or notifications from "
                + "these accounts. They are not told that you blocked them, "
                + "and they can still see the channels you share."
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.sm
            color: Theme.fg2
            wrapMode: Text.WordWrap
        }

        // ── Refusals ──
        Text {
            Layout.fillWidth: true
            visible: blockedDialog.blockModel !== null
                && blockedDialog.blockModel.errorText.length > 0
            text: blockedDialog.blockModel ? blockedDialog.blockModel.errorText : ""
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.sm
            color: Theme.danger
            wrapMode: Text.WordWrap
        }

        // ── The list ──
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.bg0
            radius: Theme.r2
            border.color: Theme.line
            border.width: 1

            // "Nothing here" and "we have not asked yet" look identical from
            // an empty list, and they mean opposite things — one is reassuring
            // and the other is a connection that has not answered.
            Text {
                anchors.centerIn: parent
                width: parent.width - Theme.sp.s7 * 2
                visible: blockedDialog.blockModel !== null
                    && blockedDialog.blockModel.loaded
                    && blockedDialog.blockModel.count === 0
                text: "You have not blocked anyone."
                horizontalAlignment: Text.AlignHCenter
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg3
                wrapMode: Text.WordWrap
            }
            Text {
                anchors.centerIn: parent
                visible: blockedDialog.blockModel !== null
                    && !blockedDialog.blockModel.loaded
                text: blockedDialog.blockModel && blockedDialog.blockModel.busy
                    ? "Checking…" : "Not loaded."
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg3
            }

            ListView {
                id: blockedList
                anchors.fill: parent
                anchors.margins: Theme.sp.s2
                clip: true
                spacing: 2
                model: blockedDialog.blockModel
                    ? blockedDialog.blockModel.users : []
                ScrollBar.vertical: ThemedScrollBar {}

                delegate: Item {
                    id: blockedRow
                    required property var modelData
                    width: ListView.view.width
                    height: 44

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.sp.s4
                        anchors.rightMargin: Theme.sp.s3
                        spacing: Theme.sp.s3

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            // The id is the headline when there is no name,
                            // and there usually is not: a blocked account is
                            // filtered out of sync, so whatever name this
                            // client last saw may be months old. Showing the
                            // id is the honest answer, not a fallback.
                            Text {
                                Layout.fillWidth: true
                                text: blockedRow.modelData.displayName.length > 0
                                    ? blockedRow.modelData.displayName
                                    : blockedRow.modelData.userId
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.md
                                color: Theme.fg0
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: blockedRow.modelData.displayName.length > 0
                                text: blockedRow.modelData.userId
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.fontSize.xs
                                color: Theme.fg3
                                elide: Text.ElideRight
                            }
                        }

                        Button {
                            id: unblockBtn
                            enabled: !blockedRow.modelData.pending
                            implicitHeight: Theme.controlHeight.sm
                            contentItem: Text {
                                text: blockedRow.modelData.pending
                                    ? "Unblocking…" : "Unblock"
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.sm
                                font.weight: Theme.fontWeight.medium
                                color: unblockBtn.enabled ? Theme.fg1 : Theme.fg3
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                implicitWidth: 104
                                radius: Theme.r2
                                color: unblockBtn.hovered ? Theme.bg3 : Theme.bg2
                                border.color: Theme.line
                                border.width: 1
                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                            }
                            onClicked: {
                                if (blockedDialog.conn)
                                    blockedDialog.conn.unblockUser(
                                        blockedRow.modelData.userId);
                            }
                        }
                    }
                }
            }
        }

        // ── Currency, stated rather than implied ──
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.sp.s3
            Text {
                Layout.fillWidth: true
                text: {
                    if (!blockedDialog.blockModel) return "";
                    if (blockedDialog.blockModel.busy) return "Checking the server…";
                    return "Checked " + blockedDialog.agoText(
                        blockedDialog.blockModel.lastRefreshedMs)
                        + ". Blocks made on another device appear after a check.";
                }
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                color: Theme.fg3
                wrapMode: Text.WordWrap
            }
            Button {
                id: refreshBtn
                enabled: blockedDialog.blockModel !== null
                    && !blockedDialog.blockModel.busy
                implicitHeight: Theme.controlHeight.sm
                contentItem: Text {
                    text: "Refresh"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    font.weight: Theme.fontWeight.medium
                    color: refreshBtn.enabled ? Theme.fg1 : Theme.fg3
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    implicitWidth: 92
                    radius: Theme.r2
                    color: refreshBtn.hovered ? Theme.bg3 : "transparent"
                    border.color: Theme.line
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                }
                onClicked: if (blockedDialog.conn) blockedDialog.conn.refreshBlockedUsers()
            }
        }

        // ── Footer ──
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.sp.s3
            Item { Layout.fillWidth: true }
            Button {
                id: blockedCloseBtn
                implicitHeight: Theme.controlHeight.md
                contentItem: Text {
                    text: "Done"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    font.weight: Theme.fontWeight.medium
                    color: Theme.fg1
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    implicitWidth: 100
                    radius: Theme.r2
                    color: blockedCloseBtn.hovered ? Theme.bg3 : "transparent"
                    border.color: Theme.line
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                }
                onClicked: blockedDialog.close()
            }
        }
    }
}
