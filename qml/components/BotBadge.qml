import QtQuick
import BSFChat

// The BOT pill that sits next to a bot account's display name, in the member
// list and on message bubbles.
//
// One component rather than the same six-property Rectangle→Text pair written
// out at each site, because the two sites it is used at today are the two
// places a name is rendered, and any third one — the profile card, a voice
// tile, a mention chip — has to look identical or the badge stops reading as
// a single fact about the account and starts reading as decoration.
//
// It is a plain Rectangle with `visible` driven by the caller, not a Loader:
// it is a handful of items, it is destroyed with its delegate, and a Loader
// per message row costs more than the pill it would be avoiding.
//
// Colour: `accent`, not `warn` or `danger`. A bot is not a problem and must
// not be dressed as one — Discord's blue-ish tag is the reference. Rendering
// it on the accent surface also means it inherits the user's accent hue and
// stays legible in both themes without a second colour decision (see
// Theme.onAccent, which exists exactly for text drawn on top of the accent).
Rectangle {
    id: badge

    // Kept uppercase in the source rather than relying on font.capitalization
    // so the width calculation below and the rendered glyphs agree.
    readonly property string label: "BOT"

    implicitWidth: badgeText.implicitWidth + Theme.sp.s2 * 2
    // Deliberately short: this sits on the baseline of a name at fontSize.md
    // and a taller pill pushes the whole row's implicitHeight up, which in a
    // message list means every bot message is a different height from every
    // human one.
    implicitHeight: 15
    radius: 4
    color: Theme.accent

    // Accessible name for screen readers. Without this the pill is an
    // unlabelled rectangle and the badge — which is the entire point of the
    // row for someone who cannot see it — is invisible.
    Accessible.role: Accessible.StaticText
    Accessible.name: qsTr("Bot account")

    Text {
        id: badgeText
        anchors.centerIn: parent
        text: badge.label
        font.family: Theme.fontSans
        font.pixelSize: 9
        font.weight: Theme.fontWeight.bold
        font.letterSpacing: 0.5
        color: Theme.onAccent
    }
}
