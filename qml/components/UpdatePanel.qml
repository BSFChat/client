import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat
import "../js/UpdateFormat.js" as UF

// The updater's status, rendered once and used twice:
//
//   UpdateDialog.qml           modal, for the unsolicited "an update is
//                              available" / "it's ready" interruption.
//   ClientSettings.qml         inline in the Updates pane, where the user
//     (embedded: true)         went looking on purpose.
//
// Both surfaces show the same eight states, so they share one component
// rather than two copies of a state → wording table that drift apart —
// which is what had happened: the pane said "v0.0.44-rc.3 is available."
// while the dialog said "BSFChat 0.0.44-rc.3 is available" over the top
// of it.
//
// SIZING. This panel has no fixed height and neither does the dialog that
// hosts it. Every state gets exactly the vertical space its own content
// needs: the download state is a title, a bar and a counter and it is
// about 150px tall; the update-available state grows by however much
// release-notes box it earns, up to `notesMaxHeight`. The previous
// dialog was pinned at 460px in all states, which is why the download
// view was two thirds empty and the progress bar sat a hundred pixels
// from the label describing it.
//
// LOGIC. Everything that can be decided without a scene graph lives in
// qml/js/UpdateFormat.js and is tested in tests/qml/tst_updateformat.qml.
// What is left here is layout, which those tests do not cover.
//
// RELEASE NOTES ARE UNTRUSTED. `updater.releaseNotes` is the body of a
// GitHub release — remote text that arrives before the user has done
// anything. It is reduced to plain text by UF.plainNotes() and rendered
// with `textFormat: TextEdit.PlainText`. No rich-text document is
// constructed from it, so there is no markup to inject, no <img> or <a>
// for QTextDocument to resolve, and no way for a release body to make
// the client fetch a remote resource. Do not "improve" this by switching
// to TextEdit.MarkdownText or MarkdownParser::toHtml without solving
// that first.
Rectangle {
    id: panel

    // Inline-in-settings mode: draws itself as a card, offers "Check
    // now", and drops the modal's dismiss button (there is nothing to
    // dismiss — the pane is where you went to look).
    property bool embedded: false

    // Raised by the modal's dismiss button. The settings pane ignores it.
    signal dismissRequested()

    // How much of a long release body to show before it scrolls. Roughly
    // ten lines at Theme.fontSize.sm.
    property int notesMaxHeight: 190

    // The Updater context property, absent on mobile builds and in any
    // isolated load of this file.
    readonly property var _u: (typeof updater !== "undefined") ? updater : null
    readonly property int _state: _u ? _u.state : UF.Idle
    readonly property string _notes: _u ? UF.plainNotes(_u.releaseNotes) : ""
    readonly property string _osName: Qt.platform.os

    readonly property color _tint: {
        switch (UF.kind(_state)) {
        case "ok":     return Theme.online;
        case "accent": return Theme.accent;
        case "warn":   return Theme.warn;
        case "danger": return Theme.danger;
        default:       return Theme.fg2;
        }
    }

    // The one action that moves the update forward in this state. Empty
    // string means there isn't one and the button is not rendered.
    readonly property string _primaryLabel: {
        if (_state === UF.UpdateAvailable) return "Download";
        if (_state === UF.ReadyToApply)    return UF.applyLabel(_osName);
        if (_state === UF.Failed)          return "Try again";
        return "";
    }

    // Modal only. Always present so a modal is always escapable by
    // pointer as well as by Esc.
    readonly property string _dismissLabel: {
        if (_state === UF.UpdateAvailable) return "Skip this version";
        if (_state === UF.ReadyToApply)    return "Later";
        if (UF.isBusy(_state))             return "Hide";
        return "Close";
    }

    readonly property int _pad: embedded ? Theme.sp.s7 : 0

    implicitWidth: body.implicitWidth + _pad * 2
    implicitHeight: body.implicitHeight + _pad * 2
    radius: Theme.r2
    color: embedded ? Theme.bg2 : "transparent"
    border.color: embedded ? Theme.line : "transparent"
    border.width: embedded ? 1 : 0

    // Shared button chrome. Matches the pattern already used in
    // ClientSettings / UpdateDialog rather than introducing a third one;
    // it is a `component` only so the four call sites below stop being
    // four copies of the same twenty lines.
    component ActionButton: Button {
        id: btn
        property bool primary: false
        property int minWidth: 104
        contentItem: Text {
            text: btn.text
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.sm
            font.weight: btn.primary ? Theme.fontWeight.semibold
                                     : Theme.fontWeight.medium
            color: !btn.enabled ? Theme.fg3
                 : btn.primary  ? Theme.onAccent
                                : Theme.fg1
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            // Width follows the label instead of a per-button magic
            // number, so "Open the release page" (Linux) and "Download"
            // both fit without either being padded to the other's size.
            // implicitContentWidth rather than contentItem.implicitWidth:
            // Control publishes it as 0 before the content delegate is
            // built, where the direct reference would evaluate on null.
            implicitWidth: Math.max(btn.minWidth,
                                    btn.implicitContentWidth + Theme.sp.s8 * 2)
            implicitHeight: 34
            radius: Theme.r2
            color: !btn.enabled
                     ? (btn.primary ? Theme.bg3 : Theme.bg2)
                 : btn.primary
                     ? (btn.hovered ? Theme.accentDim : Theme.accent)
                     : (btn.hovered ? Theme.bg3 : Theme.bg2)
            border.color: btn.primary ? "transparent" : Theme.line
            border.width: btn.primary ? 0 : 1
        }
    }

    ColumnLayout {
        id: body
        anchors.fill: parent
        anchors.margins: panel._pad
        spacing: Theme.sp.s5

        // ── Headline block ──────────────────────────────────────────
        //
        // Chip, title and supporting line are ONE unit with 3px between
        // the two texts. The supporting line used to be a sibling of the
        // title separated by the layout's full spacing, which is why it
        // read as an unrelated sentence floating in the panel.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.sp.s4

            Rectangle {
                Layout.alignment: Qt.AlignTop
                implicitWidth: 34
                implicitHeight: 34
                radius: Theme.r2
                color: Qt.rgba(panel._tint.r, panel._tint.g, panel._tint.b, 0.14)
                Icon {
                    anchors.centerIn: parent
                    name: UF.iconName(panel._state)
                    size: 16
                    color: panel._tint
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                spacing: 3

                Text {
                    Layout.fillWidth: true
                    text: panel._u
                        ? UF.title(panel._state, panel._u.availableVersion)
                        : "Updates"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.lg
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackTight.lg
                    color: Theme.fg0
                    wrapMode: Text.WordWrap
                }
                Text {
                    id: detailText
                    Layout.fillWidth: true
                    visible: text.length > 0
                    // No `updater` context property at all — mobile, where
                    // the OS store owns updates. Say so rather than
                    // offering a Check now button that can't do anything.
                    text: !panel._u
                        ? "The in-app updater isn't available on this build."
                        : UF.detail(panel._state, {
                            currentVersion: panel._u.currentVersion,
                            availableVersion: panel._u.availableVersion,
                            latestStableVersion: panel._u.latestStableVersion,
                            channel: panel._u.channel,
                            osName: panel._osName
                        })
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    color: Theme.fg2
                    wrapMode: Text.WordWrap
                }
            }
        }

        // ── The error, verbatim, in its own frame ───────────────────
        // Separate from detail() so a long QNetworkReply message wraps
        // inside a box instead of pushing the buttons off the panel.
        Rectangle {
            Layout.fillWidth: true
            visible: panel._state === UF.Failed && errText.text.length > 0
            color: Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.10)
            border.color: Qt.rgba(Theme.danger.r, Theme.danger.g,
                                  Theme.danger.b, 0.55)
            border.width: 1
            radius: Theme.r2
            implicitHeight: errText.implicitHeight + Theme.sp.s4 * 2
            Text {
                id: errText
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.sp.s4
                anchors.rightMargin: Theme.sp.s4
                text: panel._u ? panel._u.lastError : ""
                color: Theme.fg1
                wrapMode: Text.WordWrap
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSize.xs
            }
        }

        // ── Release notes ───────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.sp.s3
            visible: UF.showsNotes(panel._state) && panel._notes.length > 0

            Text {
                text: "WHAT'S NEW"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWidest.xs
                color: Theme.fg3
            }

            Rectangle {
                Layout.fillWidth: true
                // Grows with the text, stops at notesMaxHeight and
                // scrolls past that. A three-line release body gets a
                // three-line box, not a fixed slab with a hole in it.
                Layout.preferredHeight:
                    Math.min(notesEdit.implicitHeight + Theme.sp.s4 * 2,
                             panel.notesMaxHeight)
                color: Theme.bg0
                radius: Theme.r2
                border.color: Theme.line
                border.width: 1

                Flickable {
                    id: notesFlick
                    anchors.fill: parent
                    anchors.margins: Theme.sp.s4
                    clip: true
                    interactive: contentHeight > height
                    boundsBehavior: Flickable.StopAtBounds
                    contentWidth: width
                    contentHeight: notesEdit.implicitHeight
                    ScrollBar.vertical: ThemedScrollBar {}

                    TextEdit {
                        id: notesEdit
                        width: notesFlick.width
                        // PLAIN TEXT, deliberately — see the file header.
                        // UF.plainNotes() has already reduced the remote
                        // markdown; this must never become RichText or
                        // MarkdownText.
                        textFormat: TextEdit.PlainText
                        text: panel._notes
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                        color: Theme.fg1
                        selectionColor: Theme.accent
                        selectedTextColor: Theme.onAccent
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                    }
                }
            }
        }

        // ── Progress, as one unit ───────────────────────────────────
        // Bar and counter sit s3 apart inside their own column, so the
        // bar belongs to the number under it rather than floating alone
        // at the bottom of a half-empty dialog.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.sp.s3
            visible: UF.showsProgress(panel._state)

            Item {
                id: track
                Layout.fillWidth: true
                implicitHeight: 6

                readonly property bool known:
                    panel._state === UF.Downloading && panel._u
                    && UF.progressKnown(panel._u.totalBytes)
                readonly property real fraction:
                    known ? UF.progressFraction(panel._u.downloadedBytes,
                                                panel._u.totalBytes)
                          : 0

                Rectangle {
                    anchors.fill: parent
                    radius: height / 2
                    color: Theme.bg3
                }
                // Determinate fill.
                Rectangle {
                    visible: track.known
                    width: Math.max(height, track.width * track.fraction)
                    height: parent.height
                    radius: height / 2
                    color: Theme.accent
                    Behavior on width {
                        NumberAnimation { duration: Theme.motion.fastMs }
                    }
                }
                // Indeterminate: a pill that sweeps the track. Used while
                // the installer runs (it reports nothing back) and for a
                // download whose server sent no Content-Length.
                Rectangle {
                    id: sweeper
                    visible: !track.known
                    width: Math.max(24, track.width * 0.28)
                    height: parent.height
                    radius: height / 2
                    color: Theme.accent
                    SequentialAnimation on x {
                        // Gated on the state, not on `sweeper.visible`:
                        // an item inside an invisible ancestor still
                        // reports visible == true, so keying off that
                        // would leave this animating on a 900ms loop for
                        // the whole session in every non-download state.
                        running: !track.known
                                 && UF.showsProgress(panel._state)
                        loops: Animation.Infinite
                        NumberAnimation {
                            from: 0; to: Math.max(0, track.width - sweeper.width)
                            duration: 900
                            easing.type: Easing.InOutQuad
                        }
                        NumberAnimation {
                            from: Math.max(0, track.width - sweeper.width); to: 0
                            duration: 900
                            easing.type: Easing.InOutQuad
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp.s3
                visible: UF.showsByteCount(panel._state)

                Text {
                    text: panel._u ? UF.byteProgress(panel._u.downloadedBytes,
                                                     panel._u.totalBytes) : ""
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSize.xs
                    color: Theme.fg2
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: panel._u ? UF.percentText(panel._u.downloadedBytes,
                                                    panel._u.totalBytes) : ""
                    visible: text.length > 0
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSize.xs
                    font.weight: Theme.fontWeight.semibold
                    color: Theme.fg1
                }
            }
        }

        // ── Actions ─────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.sp.s1
            spacing: Theme.sp.s3

            // Manual check. Only inline: the modal is a response to a
            // check that already happened.
            ActionButton {
                visible: panel.embedded
                text: panel._state === UF.Checking ? "Checking…" : "Check now"
                enabled: panel._u !== null && UF.canCheck(panel._state)
                onClicked: if (panel._u) panel._u.checkNow()
            }

            // The full, formatted notes live on the release page. We
            // show a reduced plain-text copy above rather than rendering
            // remote markup, so this is the way to the real thing.
            //
            // Gated on there being notes at all: the Updater exposes no
            // property for the release URL, and both it and the body
            // come from the same release entry, so "there are notes" is
            // the closest available proxy for "there is a page".
            ActionButton {
                visible: UF.showsNotes(panel._state) && panel._notes.length > 0
                text: "Full notes on GitHub"
                onClicked: if (panel._u) panel._u.openReleasePage()
            }

            Item { Layout.fillWidth: true }

            ActionButton {
                visible: !panel.embedded
                text: panel._dismissLabel
                onClicked: panel.dismissRequested()
            }

            ActionButton {
                primary: true
                visible: panel._primaryLabel.length > 0
                text: panel._primaryLabel
                enabled: panel._u !== null
                onClicked: {
                    if (!panel._u) return;
                    if (panel._state === UF.UpdateAvailable) panel._u.downloadUpdate();
                    else if (panel._state === UF.ReadyToApply) panel._u.applyUpdate();
                    else if (panel._state === UF.Failed) panel._u.checkNow();
                }
            }
        }
    }
}
