import QtQuick
import QtQuick.Controls
import BSFChat
import "../js/ComboBoxText.js" as ComboBoxText

// Themed dropdown. Surface matches the composer's TextField vocabulary:
// `bg0` body, `line` unfocused / `accent` focused border, `r2` radius,
// Geist font, chevron-down glyph on the right. Popup list sits on `bg1`
// with `line` border and highlights the current + hovered row with the
// familiar accent / bg3 pair.
//
// API matches Qt Controls ComboBox — drop it in anywhere a ComboBox was.
ComboBox {
    id: cb
    font.family: Theme.fontSans
    font.pixelSize: Theme.fontSize.md

    delegate: ItemDelegate {
        id: itemDelegate
        width: cb.width
        implicitHeight: 32
        required property int index
        required property var modelData
        readonly property bool selected: index === cb.currentIndex
        contentItem: Text {
            // ComboBox.textAt(index) resolves through textRole against
            // most model shapes — QVariantList-as-array,
            // QAbstractListModel with roles, plain JS array — but NOT
            // against a QJsonArray handed over from C++, where it
            // returns "" for every row while the count comes through
            // (six correctly-sized blank rows in Role overrides). The
            // even older Array.isArray + model[textRole] path had the
            // mirror-image hole on QVariantList models, which is the
            // first "black on black" dropdown; reverting to it just
            // swaps which combos are blank. ComboBoxText.js tries both
            // and carries the full story.
            text: ComboBoxText.resolve(cb.textAt(itemDelegate.index),
                                       cb.model, itemDelegate.index,
                                       cb.textRole)
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.md
            font.weight: itemDelegate.selected
                         ? Theme.fontWeight.semibold
                         : Theme.fontWeight.regular
            color: itemDelegate.selected ? Theme.accent : Theme.fg1
            verticalAlignment: Text.AlignVCenter
            leftPadding: Theme.sp.s4
        }
        background: Rectangle {
            color: itemDelegate.hovered ? Theme.bg3 : "transparent"
            Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
        }
    }

    indicator: Icon {
        anchors.right: parent.right
        anchors.rightMargin: Theme.sp.s3
        anchors.verticalCenter: parent.verticalCenter
        name: "chevron-down"
        size: 14
        color: cb.popup.visible ? Theme.accent : Theme.fg2
        rotation: cb.popup.visible ? 180 : 0
        Behavior on rotation {
            NumberAnimation { duration: Theme.motion.fastMs
                              easing.type: Easing.BezierSpline
                              easing.bezierCurve: Theme.motion.bezier }
        }
    }

    contentItem: Text {
        // Offset left to leave room for the chevron indicator.
        leftPadding: Theme.sp.s4
        rightPadding: cb.indicator.width + Theme.sp.s5
        // The closed field has the same root cause as the rows above:
        // displayText goes through the same textRole lookup, so it is
        // blank on a QJsonArray model too. Same resolver, same reasons.
        text: ComboBoxText.resolve(cb.displayText, cb.model,
                                   cb.currentIndex, cb.textRole)
        font.family: Theme.fontSans
        font.pixelSize: Theme.fontSize.md
        color: Theme.fg0
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitHeight: 36
        color: Theme.bg0
        border.color: cb.popup.visible ? Theme.accent : Theme.line
        border.width: 1
        radius: Theme.r2
        Behavior on border.color { ColorAnimation { duration: Theme.motion.fastMs } }
    }

    popup: Popup {
        y: cb.height + 4
        width: cb.width
        implicitHeight: contentItem.implicitHeight
        padding: 4

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: cb.popup.visible ? cb.delegateModel : null
            currentIndex: cb.highlightedIndex
            ScrollBar.vertical: ThemedScrollBar {}
        }

        background: Rectangle {
            color: Theme.bg1
            border.color: Theme.line
            border.width: 1
            radius: Theme.r2
        }
    }
}
