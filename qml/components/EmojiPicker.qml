import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat
import "../data/EmojiData.js" as EmojiData

Popup {
    id: emojiPicker

    // Cap to viewport on narrow phone screens.
    width: Math.min(350, (parent ? parent.width : 350) - 24)
    height: Math.min(400, (parent ? parent.height : 400) - 48)
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    signal emojiSelected(string emoji)

    // Recently used emoji storage (in-memory for this session)
    property var recentEmoji: []
    property int maxRecent: 20
    property string currentCategory: recentEmoji.length > 0 ? "frequent" : "people"
    property string searchQuery: ""

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1

        // Drop shadow effect via layered rectangle
        Rectangle {
            anchors.fill: parent
            anchors.margins: -1
            z: -1
            radius: Theme.r3 + 1
            color: "#40000000"
        }
    }

    contentItem: ColumnLayout {
        spacing: 0

        // Search bar
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            Layout.margins: Theme.sp.s3
            Layout.bottomMargin: 0
            color: Theme.bg2
            radius: Theme.r1

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.sp.s3
                anchors.rightMargin: Theme.sp.s3
                spacing: Theme.sp.s1

                Icon {
                    name: "search"
                    size: 14
                    color: Theme.fg2
                    Layout.alignment: Qt.AlignVCenter
                }

                TextInput {
                    id: searchField
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    color: Theme.fg0
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    clip: true
                    onTextChanged: emojiPicker.searchQuery = text

                    Text {
                        anchors.fill: parent
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Search emoji…"
                        color: Theme.fg3
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        visible: searchField.text.length === 0
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                // Clear search button — ghost X that surfaces only when
                // there's something to clear.
                Rectangle {
                    Layout.preferredWidth: 22
                    Layout.preferredHeight: 22
                    Layout.alignment: Qt.AlignVCenter
                    radius: Theme.r1
                    color: clearSearchMouse.containsMouse ? Theme.bg3 : "transparent"
                    visible: searchField.text.length > 0
                    Icon {
                        anchors.centerIn: parent
                        name: "x"
                        size: 10
                        color: clearSearchMouse.containsMouse ? Theme.fg0 : Theme.fg2
                    }
                    MouseArea {
                        id: clearSearchMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            searchField.text = "";
                            searchField.forceActiveFocus();
                        }
                    }
                }
            }
        }

        // Category tabs
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            Layout.leftMargin: Theme.sp.s3
            Layout.rightMargin: Theme.sp.s3
            Layout.topMargin: Theme.sp.s1
            color: "transparent"
            visible: emojiPicker.searchQuery.length === 0

            RowLayout {
                anchors.fill: parent
                spacing: 2

                Repeater {
                    model: EmojiData.categories

                    Rectangle {
                        Layout.preferredWidth: 34
                        Layout.preferredHeight: 32
                        radius: Theme.r1
                        readonly property bool isSelected:
                            emojiPicker.currentCategory === modelData.id
                        color: isSelected
                               ? Theme.bg3
                               : categoryTabHover.containsMouse
                                 ? Theme.bg2
                                 : "transparent"
                        visible: modelData.id !== "frequent" || emojiPicker.recentEmoji.length > 0
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                        // Selected-tab accent stripe along the bottom edge —
                        // reads the selection without having to compare bg tints.
                        Rectangle {
                            anchors.bottom: parent.bottom
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: parent.isSelected ? 20 : 0
                            height: 2
                            radius: 1
                            color: Theme.accent
                            Behavior on width {
                                NumberAnimation { duration: Theme.motion.fastMs
                                                  easing.type: Easing.BezierSpline
                                                  easing.bezierCurve: Theme.motion.bezier }
                            }
                        }

                        Text {
                            anchors.centerIn: parent
                            text: modelData.icon
                            font.pixelSize: 18
                            opacity: parent.isSelected ? 1.0 : 0.65
                            Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }
                        }

                        MouseArea {
                            id: categoryTabHover
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: emojiPicker.currentCategory = modelData.id

                            ToolTip {
                                visible: categoryTabHover.containsMouse
                                text: modelData.name
                                delay: 500
                            }
                        }
                    }
                }
            }
        }

        // Separator
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            Layout.leftMargin: Theme.sp.s3
            Layout.rightMargin: Theme.sp.s3
            Layout.topMargin: Theme.sp.s1
            color: Theme.line
        }

        // Emoji grid area
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Theme.sp.s1

            GridView {
                id: emojiGrid
                anchors.fill: parent
                anchors.margins: Theme.sp.s1
                cellWidth: 40
                cellHeight: 40
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                ScrollBar.vertical: ThemedScrollBar {}

                model: {
                    if (emojiPicker.searchQuery.length > 0) {
                        return EmojiData.search(emojiPicker.searchQuery);
                    }
                    if (emojiPicker.currentCategory === "frequent") {
                        return emojiPicker.recentEmoji;
                    }
                    return EmojiData.getByCategory(emojiPicker.currentCategory);
                }

                delegate: Rectangle {
                    width: 38
                    height: 38
                    radius: Theme.r1
                    color: emojiCellHover.containsMouse ? Theme.bg3 : "transparent"

                    Text {
                        anchors.centerIn: parent
                        text: modelData.emoji
                        font.pixelSize: 24
                    }

                    MouseArea {
                        id: emojiCellHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            addToRecent(modelData);
                            emojiPicker.emojiSelected(modelData.emoji);
                        }

                        ToolTip {
                            visible: emojiCellHover.containsMouse
                            text: modelData.name
                            delay: 400
                        }
                    }
                }

                // Empty state for search
                Text {
                    anchors.centerIn: parent
                    text: "No emoji found"
                    color: Theme.fg2
                    font.pixelSize: Theme.fontSize.md
                    visible: emojiGrid.count === 0
                }
            }
        }
    }

    function addToRecent(emojiObj) {
        // Remove if already in recents
        var filtered = recentEmoji.filter(function(e) {
            return e.emoji !== emojiObj.emoji;
        });
        // Add to front
        filtered.unshift(emojiObj);
        // Trim to max
        if (filtered.length > maxRecent) {
            filtered = filtered.slice(0, maxRecent);
        }
        recentEmoji = filtered;
    }

    onOpened: {
        searchField.text = "";
        searchField.forceActiveFocus();
    }
}
