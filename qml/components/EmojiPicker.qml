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
            Accessible.ignored: true
            radius: Theme.r3 + 1
            // Was a literal #40000000, which is a black shadow at 25% — fine in
            // dark mode and a smudge in light, where the theme's shadow is both
            // a different colour and far softer.
            color: Qt.rgba(Theme.shadowColor.r, Theme.shadowColor.g,
                           Theme.shadowColor.b, Theme.shadowAlpha2)
        }
    }

    contentItem: ColumnLayout {
        spacing: 0

        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Emoji picker")

        // Search bar
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.controlHeight.lg
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

                    // Keyboard (U-3): the search field is where focus lands,
                    // so it drives the grid the way a combobox drives its
                    // list. Down moves into the grid, Enter picks the
                    // highlighted emoji (the first result until you move),
                    // Escape is the Popup's own CloseOnEscape.
                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_Down) {
                            emojiPicker._focusGrid();
                            event.accepted = true;
                        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                            emojiPicker._pickCurrent();
                            event.accepted = true;
                        }
                    }

                    Accessible.role: Accessible.EditableText
                    Accessible.name: qsTr("Search emoji")
                    Accessible.description: qsTr("Filters the emoji grid as you type. "
                                                 + "Down arrow moves into the results, "
                                                 + "Return picks the highlighted emoji.")

                    Text {
                        anchors.fill: parent
                        anchors.verticalCenter: parent.verticalCenter
                        Accessible.ignored: true
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

                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Clear emoji search")
                    Accessible.onPressAction: {
                        searchField.text = "";
                        searchField.forceActiveFocus();
                    }

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
            Layout.preferredHeight: Theme.controlHeight.md
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
                        Layout.preferredHeight: Theme.controlHeight.sm
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
                        // Tab reaches the tabs; Space/Return select, Left and
                        // Right walk along them, Down drops into the grid.
                        activeFocusOnTab: true
                        border.width: activeFocus ? 2 : 0
                        border.color: Theme.accent
                        Keys.onPressed: (event) => {
                            if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                                || event.key === Qt.Key_Enter) {
                                emojiPicker.currentCategory = modelData.id;
                                event.accepted = true;
                            } else if (event.key === Qt.Key_Right || event.key === Qt.Key_Left) {
                                var next = nextItemInFocusChain(event.key === Qt.Key_Right);
                                if (next && next.parent === parent) next.forceActiveFocus();
                                event.accepted = true;
                            } else if (event.key === Qt.Key_Down) {
                                emojiPicker.currentCategory = modelData.id;
                                emojiPicker._focusGrid();
                                event.accepted = true;
                            }
                        }

                        Accessible.role: Accessible.PageTab
                        Accessible.name: qsTr("%1 emoji").arg(modelData.name)
                        Accessible.checkable: true
                        Accessible.checked: isSelected
                        Accessible.onPressAction: emojiPicker.currentCategory = modelData.id

                        // Selected-tab accent stripe along the bottom edge —
                        // reads the selection without having to compare bg tints.
                        Rectangle {
                            anchors.bottom: parent.bottom
                            anchors.horizontalCenter: parent.horizontalCenter
                            Accessible.ignored: true
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
                            Accessible.ignored: true
                            textFormat: Text.PlainText
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
            Accessible.ignored: true
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
                activeFocusOnTab: true

                Accessible.role: Accessible.List
                Accessible.name: emojiPicker.searchQuery.length > 0
                    ? qsTr("Emoji matching %1").arg(emojiPicker.searchQuery)
                    : qsTr("Emoji")

                ScrollBar.vertical: ThemedScrollBar {}

                // Arrow keys are GridView's own key navigation (currentIndex);
                // this keeps the highlighted cell in view and says what it is,
                // because the grid, not the cell, holds focus and a screen
                // reader has nothing else to read.
                onCurrentIndexChanged: {
                    if (currentIndex < 0) return;
                    positionViewAtIndex(currentIndex, GridView.Contain);
                    if (emojiGrid.activeFocus && currentItem)
                        emojiGrid.Accessible.announce(currentItem.Accessible.name,
                                                      Accessible.AnnouncementPoliteness.Polite);
                }
                onCountChanged: currentIndex = count > 0 ? 0 : -1
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                        || event.key === Qt.Key_Space) {
                        emojiPicker._pickCurrent();
                        event.accepted = true;
                        return;
                    }
                    // Up off the top row goes back to the search field.
                    if (event.key === Qt.Key_Up && currentIndex >= 0
                        && currentIndex < Math.max(1, Math.floor(width / cellWidth))) {
                        searchField.forceActiveFocus();
                        event.accepted = true;
                        return;
                    }
                    if (event.key === Qt.Key_Backspace) {
                        searchField.text = searchField.text.slice(0, -1);
                        searchField.forceActiveFocus();
                        event.accepted = true;
                        return;
                    }
                    // Typing anywhere in the grid searches.
                    if (event.text.length > 0 && event.text.trim().length > 0
                        && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier
                                                | Qt.MetaModifier))) {
                        searchField.text += event.text;
                        searchField.forceActiveFocus();
                        event.accepted = true;
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
                    radius: Theme.r1
                    readonly property bool kbCurrent: GridView.isCurrentItem
                        && (emojiGrid.activeFocus
                            || (searchField.activeFocus && emojiPicker.searchQuery.length > 0))
                    color: emojiCellHover.containsMouse || kbCurrent ? Theme.bg3 : "transparent"
                    border.width: kbCurrent ? 2 : 0
                    border.color: Theme.accent

                    Accessible.role: Accessible.ListItem
                    Accessible.name: modelData.name
                    Accessible.selected: GridView.isCurrentItem
                    Accessible.description: qsTr("Insert %1").arg(modelData.name)
                    Accessible.onPressAction: {
                        addToRecent(modelData);
                        emojiPicker.emojiSelected(modelData.emoji);
                    }

                    Text {
                        anchors.centerIn: parent
                        Accessible.ignored: true
                        textFormat: Text.PlainText
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
                    Accessible.role: Accessible.StaticText
                    Accessible.name: qsTr("No emoji found")
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

    function _focusGrid() {
        if (emojiGrid.count === 0) return;
        if (emojiGrid.currentIndex < 0) emojiGrid.currentIndex = 0;
        emojiGrid.forceActiveFocus();
        if (emojiGrid.currentItem)
            emojiGrid.Accessible.announce(emojiGrid.currentItem.Accessible.name,
                                          Accessible.AnnouncementPoliteness.Polite);
    }
    function _pickCurrent() {
        if (emojiGrid.currentIndex < 0 || emojiGrid.currentIndex >= emojiGrid.count) return;
        var m = emojiGrid.model;
        var e = m && m[emojiGrid.currentIndex];
        if (!e || !e.emoji) return;
        addToRecent(e);
        emojiPicker.emojiSelected(e.emoji);
    }

    onOpened: {
        searchField.text = "";
        emojiGrid.currentIndex = emojiGrid.count > 0 ? 0 : -1;
        searchField.forceActiveFocus();
    }
}
