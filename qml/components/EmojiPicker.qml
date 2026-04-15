import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat
import "../data/EmojiData.js" as EmojiData

Popup {
    id: emojiPicker

    width: 350
    height: 400
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    signal emojiSelected(string emoji)

    // Recently used emoji storage (in-memory for this session)
    property var recentEmoji: []
    property int maxRecent: 20
    property string currentCategory: recentEmoji.length > 0 ? "frequent" : "people"
    property string searchQuery: ""

    background: Rectangle {
        color: Theme.bgDark
        radius: Theme.radiusNormal
        border.color: Theme.bgLight
        border.width: 1

        // Drop shadow effect via layered rectangle
        Rectangle {
            anchors.fill: parent
            anchors.margins: -1
            z: -1
            radius: Theme.radiusNormal + 1
            color: "#40000000"
        }
    }

    contentItem: ColumnLayout {
        spacing: 0

        // Search bar
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            Layout.margins: Theme.spacingNormal
            Layout.bottomMargin: 0
            color: Theme.bgMedium
            radius: Theme.radiusSmall

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacingNormal
                anchors.rightMargin: Theme.spacingNormal
                spacing: Theme.spacingSmall

                Text {
                    text: "\u{1F50D}"
                    font.pixelSize: 14
                    color: Theme.textMuted
                    Layout.alignment: Qt.AlignVCenter
                }

                TextInput {
                    id: searchField
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSizeNormal
                    clip: true
                    onTextChanged: emojiPicker.searchQuery = text

                    Text {
                        anchors.fill: parent
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Search emoji..."
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSizeNormal
                        visible: searchField.text.length === 0
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                // Clear search button
                Text {
                    text: "\u2715"
                    font.pixelSize: 12
                    color: Theme.textMuted
                    visible: searchField.text.length > 0
                    Layout.alignment: Qt.AlignVCenter

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -4
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
            Layout.leftMargin: Theme.spacingNormal
            Layout.rightMargin: Theme.spacingNormal
            Layout.topMargin: Theme.spacingSmall
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
                        radius: Theme.radiusSmall
                        color: emojiPicker.currentCategory === modelData.id
                               ? Theme.bgLight
                               : categoryTabHover.containsMouse
                                 ? Theme.bgMedium
                                 : "transparent"
                        visible: modelData.id !== "frequent" || emojiPicker.recentEmoji.length > 0

                        Text {
                            anchors.centerIn: parent
                            text: modelData.icon
                            font.pixelSize: 18
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
            Layout.leftMargin: Theme.spacingNormal
            Layout.rightMargin: Theme.spacingNormal
            Layout.topMargin: Theme.spacingSmall
            color: Theme.bgLight
        }

        // Emoji grid area
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Theme.spacingSmall

            GridView {
                id: emojiGrid
                anchors.fill: parent
                anchors.margins: Theme.spacingSmall
                cellWidth: 40
                cellHeight: 40
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                ScrollBar.vertical: ScrollBar {
                    active: true
                    policy: ScrollBar.AsNeeded
                    contentItem: Rectangle {
                        implicitWidth: 4
                        radius: 2
                        color: Theme.textMuted
                        opacity: 0.5
                    }
                    background: Rectangle {
                        color: "transparent"
                    }
                }

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
                    radius: Theme.radiusSmall
                    color: emojiCellHover.containsMouse ? Theme.bgLight : "transparent"

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
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeNormal
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
