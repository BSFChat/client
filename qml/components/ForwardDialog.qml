import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// Modal channel picker for "Forward message". Lists the current server's
// text channels grouped by category and forwards the source event into
// whichever the user clicks. MVP: single-server only — cross-server
// forwarding would need to hop through ServerManager.
Popup {
    id: forwardDialog

    property string sourceEventId: ""
    property string sourceBody: ""
    property string sourceSenderName: ""

    function openFor(eventId, body, senderName) {
        sourceEventId = eventId;
        sourceBody = body;
        sourceSenderName = senderName;
        open();
    }

    width: Math.min(420, (parent ? parent.width : 420) - 32)
    height: Math.min(520, (parent ? parent.height : 520) - 48)
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    x: (parent.width - width) / 2
    y: (parent.height - height) / 2

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0.0; to: 1.0; duration: 150; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: 0.95; to: 1.0; duration: 150; easing.type: Easing.OutCubic }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1.0; to: 0.0; duration: 100; easing.type: Easing.InCubic }
    }

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1
    }

    // Flattened list of channel rows built from categorizedRooms. Each
    // entry is either a category header (type === "cat") or a channel
    // row (type === "ch"). Voice channels are skipped — forwarding into
    // a voice room is nonsensical.
    property var _rows: []

    function _rebuildRows() {
        var out = [];
        if (!serverManager.activeServer) {
            _rows = out;
            return;
        }
        var cats = serverManager.activeServer.categorizedRooms;
        for (var i = 0; i < cats.length; ++i) {
            var cat = cats[i];
            var channels = cat.channels || [];
            var textChannels = [];
            for (var j = 0; j < channels.length; ++j) {
                if (!channels[j].isVoice && channels[j].roomType !== "category") {
                    textChannels.push(channels[j]);
                }
            }
            if (textChannels.length === 0) continue;
            out.push({ type: "cat", name: cat.categoryName || "Uncategorized" });
            for (var k = 0; k < textChannels.length; ++k) {
                out.push({ type: "ch",
                            roomId: textChannels[k].roomId,
                            name: textChannels[k].displayName });
            }
        }
        _rows = out;
    }

    onAboutToShow: _rebuildRows()

    contentItem: ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.sp.s7
        spacing: Theme.sp.s3

        // Title row + divider — SPEC §3.10 section-header treatment.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.sp.s3
            Text {
                text: "Forward message"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xl
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackTight.xl
                color: Theme.fg0
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.line }
        }

        // Preview of the message being forwarded — quoted style.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: previewCol.implicitHeight + Theme.sp.s3 * 2
            color: Theme.bg2
            radius: Theme.r1

            RowLayout {
                anchors.fill: parent
                anchors.margins: Theme.sp.s3
                spacing: Theme.sp.s1

                Rectangle {
                    Layout.preferredWidth: 3
                    Layout.fillHeight: true
                    color: Theme.accent
                    radius: 1.5
                }

                ColumnLayout {
                    id: previewCol
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        text: forwardDialog.sourceSenderName !== ""
                              ? forwardDialog.sourceSenderName : "unknown"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.accent
                    }
                    Text {
                        text: forwardDialog.sourceBody
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                        color: Theme.fg1
                        wrapMode: Text.Wrap
                        elide: Text.ElideRight
                        maximumLineCount: 4
                        Layout.fillWidth: true
                    }
                }
            }
        }

        Text {
            text: "DESTINATION CHANNEL"
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.xs
            font.weight: Theme.fontWeight.semibold
            font.letterSpacing: Theme.trackWidest.xs
            color: Theme.fg3
            Layout.topMargin: Theme.sp.s3
        }

        // Channel list
        ListView {
            id: channelList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            ScrollBar.vertical: ThemedScrollBar {}
            model: forwardDialog._rows
            spacing: 1

            delegate: Loader {
                width: channelList.width
                property var row: modelData
                sourceComponent: row.type === "cat" ? categoryHeaderComponent
                                                    : channelRowComponent
            }

            Component {
                id: categoryHeaderComponent
                Item {
                    height: 28
                    Text {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: Theme.sp.s1
                        text: (row.name || "").toUpperCase()
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                        font.weight: Theme.fontWeight.semibold
                        font.letterSpacing: Theme.trackWidest.xs
                        color: Theme.fg3
                    }
                }
            }

            Component {
                id: channelRowComponent
                Rectangle {
                    height: 32
                    radius: Theme.r1
                    color: chanHover.containsMouse ? Theme.bg3 : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.sp.s3
                        anchors.rightMargin: Theme.sp.s3
                        spacing: Theme.sp.s3

                        Icon {
                            name: "hash"
                            size: 14
                            color: chanHover.containsMouse ? Theme.accent : Theme.fg2
                        }
                        Text {
                            text: row.name
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.md
                            font.weight: chanHover.containsMouse
                                         ? Theme.fontWeight.medium
                                         : Theme.fontWeight.regular
                            color: Theme.fg0
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }

                    MouseArea {
                        id: chanHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (serverManager.activeServer
                                && forwardDialog.sourceEventId !== "") {
                                serverManager.activeServer.forwardMessage(
                                    forwardDialog.sourceEventId, row.roomId);
                            }
                            forwardDialog.close();
                        }
                    }
                }
            }
        }

        // Cancel button — ghost style so the channel list stays the focus.
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            Button {
                id: cancelBtn
                contentItem: Text {
                    text: "Cancel"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    font.weight: Theme.fontWeight.medium
                    color: Theme.fg1
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: cancelBtn.hovered ? Theme.bg3 : "transparent"
                    border.color: Theme.line
                    border.width: 1
                    radius: Theme.r2
                    implicitWidth: 100
                    implicitHeight: 32
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                }
                onClicked: forwardDialog.close()
            }
        }
    }
}
