import QtQuick
import QtQuick.Layouts
import BSFChat

// Muted inline info callout for settings panes. Left-edge accent bar +
// icon + body text; useful for "setting isn't wired up yet" / "changes
// apply on next connection" style notices. Sits on bg2 so it reads as
// informational but not alarming.
Rectangle {
    id: root
    property string text: ""
    property string icon: "eye"          // any bundled SVG name
    property color tint: Theme.accent

    Layout.fillWidth: true
    Layout.preferredHeight: bannerText.implicitHeight + Theme.sp.s4 * 2
    radius: Theme.r2
    color: Theme.bg2
    border.color: Theme.line
    border.width: 1

    // Accent stripe — 2px wide, 3/4 of the height, centered.
    Rectangle {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: 2
        height: parent.height - Theme.sp.s4 * 2
        color: root.tint
        radius: 1
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.sp.s5
        anchors.rightMargin: Theme.sp.s4
        anchors.topMargin: Theme.sp.s4
        anchors.bottomMargin: Theme.sp.s4
        spacing: Theme.sp.s3

        Icon {
            name: root.icon
            size: 14
            color: root.tint
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: 2
        }
        Text {
            id: bannerText
            text: root.text
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.sm
            color: Theme.fg2
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }
}
