import QtQuick
import QtQuick.Controls

// A ToolTip whose text is never interpreted as markup (C2-4).
//
// ToolTip has no textFormat; the style's contentItem is a plain AutoText
// Text, so a tooltip showing anything a user or server chose (an emoji name
// from a custom pack, a server's voice privacy detail) turns `<img src=…>`
// into a remote fetch. Same look as the Basic style, PlainText.
ToolTip {
    id: control
    contentItem: Text {
        text: control.text
        textFormat: Text.PlainText
        font: control.font
        wrapMode: Text.Wrap
        color: control.palette.toolTipText
    }
}
