import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// Client-side message search. Bound to ⌘K. Queries the currently-
// loaded MessageModel (i.e. messages already in view / paginated back
// to). Server-backed indexing would be a separate track; this is the
// client-only "find what you just saw" affordance.
Popup {
    id: searchPopup
    anchors.centerIn: Overlay.overlay
    width: Math.min(parent ? parent.width * 0.7 : 640, 640)
    height: Math.min(parent ? parent.height * 0.7 : 480, 520)
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    signal resultClicked(string eventId)

    property var results: []

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1
    }

    function runSearch(text) {
        var s = serverManager.activeServer;
        if (!s || !s.messageModel) { results = []; return; }
        results = s.messageModel.searchMessages(text, 50);
    }

    onOpened: {
        searchField.text = "";
        results = [];
        searchField.forceActiveFocus();
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
                    placeholderText: "Search loaded messages…"
                    background: Item {}
                    color: Theme.fg0
                    placeholderTextColor: Theme.fg3
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.base
                    selectByMouse: true
                    onTextChanged: searchPopup.runSearch(text)
                    Keys.onReturnPressed: {
                        if (searchPopup.results.length > 0)
                            searchPopup._activate(0);
                    }
                    Keys.onDownPressed: resultsList.forceActiveFocus()
                }

                Text {
                    visible: searchPopup.results.length > 0
                    text: searchPopup.results.length + " match" +
                          (searchPopup.results.length === 1 ? "" : "es")
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
            clip: true
            spacing: 2
            model: searchPopup.results
            ScrollBar.vertical: ThemedScrollBar {}
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
                width: ListView.view.width
                height: 54
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
                        text: modelData.body
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                        color: Theme.fg1
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        maximumLineCount: 1
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

        // Empty-state message.
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: searchPopup.results.length === 0
                  && searchField.text.trim().length > 0
            Text {
                anchors.centerIn: parent
                text: "No matches in the loaded history."
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg3
            }
        }
    }

    function _activate(idx) {
        if (idx < 0 || idx >= results.length) return;
        var ev = results[idx].eventId;
        resultClicked(ev);
        close();
    }
}
