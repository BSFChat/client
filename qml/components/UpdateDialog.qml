import QtQuick
import QtQuick.Controls
import BSFChat
import "../js/UpdateFormat.js" as UF

// The MODAL half of the updater UI. Its whole job is the interruption:
// telling someone who was doing something else that a new build exists,
// or that the one they asked for is ready. The body is UpdatePanel.qml,
// shared with the inline panel in Client Settings → Updates.
//
// WHY A MODAL AT ALL, AND WHEN
//
// It used to open on every actionable state including Downloading, which
// meant clicking "Download" inside Client Settings → Updates threw a
// modal over the settings dialog you were already reading — covering the
// rows behind it and cutting them off mid-sentence. A modal is the right
// shape for an unsolicited interruption and the wrong shape for feedback
// on a button you just pressed three inches away.
//
// So the split is by WHO STARTED IT, not by state:
//
//   * Settings pane open  → `suppressed` is true (main.qml binds it to
//     clientSettingsGlobal.opened) and this dialog never opens itself.
//     The user is looking at the Updates pane; the inline UpdatePanel
//     there shows the same states with the same wording, in place.
//
//   * Settings pane closed → a background check that finds something,
//     finishes a download, or fails opens this dialog, because there is
//     no other surface on screen to say it on.
//
// Downloading is not in the auto-open set. It is only ever entered by
// someone pressing Download — either in this dialog (already open, it
// just changes state) or in the settings pane (suppressed). Opening for
// it could only ever produce the pop-over-settings bug.
//
// State numbers come from UF.* (qml/js/UpdateFormat.js), which mirrors
// Updater::State; the bare integers that used to be written inline here
// are gone.
Popup {
    id: updateDialog
    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay

    // While true this dialog will not open itself. Bound in main.qml to
    // "the Client Settings popup is open", i.e. "the user has a better
    // surface for this already".
    property bool suppressed: false

    // Width is capped; HEIGHT IS NOT SET. The panel sizes to whichever
    // state it is in and the popup follows it, so the download view is a
    // compact card instead of the old fixed 460px slab with the progress
    // bar stranded at the bottom.
    width: Math.min(parent ? parent.width * 0.6 : 480, 520)
    padding: Theme.sp.s8
    modal: true
    // Not CloseOnPressOutside: a stray click elsewhere should not
    // silently discard an update prompt. Esc and the dismiss button are
    // the two ways out, and both are always available.
    closePolicy: Popup.CloseOnEscape

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1
    }

    // Versions the user has explicitly skipped, so a six-hourly
    // background check doesn't re-offer the same build all day.
    property string _dismissedFor: ""

    Connections {
        target: typeof updater !== "undefined" ? updater : null
        function onStateChanged() {
            if (typeof updater === "undefined" || !updater) return;
            if (updateDialog.suppressed || updateDialog.opened) return;
            var s = updater.state;
            if (s === UF.UpdateAvailable) {
                if (updateDialog._dismissedFor === updater.availableVersion)
                    return;
                updateDialog.open();
            } else if (s === UF.ReadyToApply || s === UF.Failed) {
                updateDialog.open();
            }
        }
    }

    contentItem: UpdatePanel {
        embedded: false
        // Cap the release-notes box against the window rather than a
        // constant, so the dialog stays inside a short window.
        notesMaxHeight: Math.max(120, Math.min(220,
            (updateDialog.parent ? updateDialog.parent.height : 600) * 0.3))
        onDismissRequested: {
            // Only a dismissal of the OFFER is a skip. "Later" on a
            // finished download or "Close" on an error must not suppress
            // the next prompt for that version.
            if (typeof updater !== "undefined" && updater
                && updater.state === UF.UpdateAvailable) {
                updateDialog._dismissedFor = updater.availableVersion;
            }
            updateDialog.close();
        }
    }
}
