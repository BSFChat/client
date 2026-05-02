import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// Auto-update prompt + progress UI. Driven entirely by the
// `updater` C++ context property's State machine — open the dialog
// when state hits UpdateAvailable / Downloading / ReadyToApply,
// close it on Idle / UpToDate.
//
// Three visual phases mirror the underlying state:
//   UpdateAvailable → "v0.0.X is out — Download / Later"
//   Downloading     → progress bar + bytes
//   ReadyToApply    → "Restart now / Later"
// Failed surfaces as a small inline error banner.
Popup {
    id: updateDialog
    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    width: Math.min(parent ? parent.width * 0.6 : 480, 520)
    height: Math.min(parent ? parent.height * 0.7 : 460, 460)
    modal: true
    padding: 0
    closePolicy: Popup.CloseOnEscape

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1
    }

    // Open automatically the moment the C++ side reports a usable
    // state, but only if the user hasn't already dismissed it for
    // this version (we honour `_dismissedFor` to avoid nagging).
    property string _dismissedFor: ""
    Connections {
        target: typeof updater !== "undefined" ? updater : null
        function onStateChanged() {
            if (!updater) return;
            // Updater.State enum: 0=Idle 1=Checking 2=UpToDate
            // 3=UpdateAvailable 4=Downloading 5=ReadyToApply
            // 6=Applying 7=Failed
            var s = updater.state;
            var actionable = (s === 3 || s === 4 || s === 5 || s === 7);
            var skipNag = (s === 3
                && updateDialog._dismissedFor === updater.availableVersion);
            if (actionable && !skipNag && !updateDialog.opened) {
                updateDialog.open();
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.sp.s5
        anchors.margins: Theme.sp.s7

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.sp.s3
            Icon { name: "bolt"; size: 18; color: Theme.accent }
            Text {
                Layout.fillWidth: true
                text: {
                    if (!updater) return "Updates";
                    if (updater.state === 4) return "Downloading update…";
                    if (updater.state === 5) return "Update ready to install";
                    if (updater.state === 7) return "Update failed";
                    return "BSFChat " + (updater.availableVersion || "")
                        + " is available";
                }
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.lg
                font.weight: Theme.fontWeight.semibold
                color: Theme.fg0
            }
        }

        // Subtitle: current → available, or the error message.
        Text {
            Layout.fillWidth: true
            visible: updater && updater.state !== 7
            text: updater
                ? "You're on " + updater.currentVersion
                  + " — latest is " + (updater.availableVersion
                                       || updater.currentVersion) + "."
                : ""
            color: Theme.fg2
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.sm
            wrapMode: Text.WordWrap
        }
        Rectangle {
            Layout.fillWidth: true
            visible: updater && updater.state === 7
            color: Qt.rgba(Theme.danger.r, Theme.danger.g,
                           Theme.danger.b, 0.12)
            border.color: Theme.danger
            border.width: 1
            radius: Theme.r2
            implicitHeight: errText.implicitHeight + Theme.sp.s4 * 2
            Text {
                id: errText
                anchors.fill: parent
                anchors.margins: Theme.sp.s4
                text: updater ? updater.lastError : ""
                color: Theme.fg0
                wrapMode: Text.WordWrap
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
            }
        }

        // Release notes — scrollable. Only shown in
        // UpdateAvailable / ReadyToApply; once we hit Downloading
        // the progress bar takes over.
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: updater && (updater.state === 3 || updater.state === 5)
            color: Theme.bg0
            radius: Theme.r2
            border.color: Theme.line
            border.width: 1
            ScrollView {
                anchors.fill: parent
                anchors.margins: Theme.sp.s3
                clip: true
                TextArea {
                    text: updater ? updater.releaseNotes : ""
                    readOnly: true
                    wrapMode: TextEdit.Wrap
                    background: Item {}
                    color: Theme.fg1
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                }
            }
        }

        // Progress UI — only during the actual download.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.sp.s2
            visible: updater && updater.state === 4
            ProgressBar {
                Layout.fillWidth: true
                from: 0
                to: updater && updater.totalBytes > 0
                    ? updater.totalBytes : 1
                value: updater ? updater.downloadedBytes : 0
                indeterminate: !updater || updater.totalBytes <= 0
            }
            Text {
                text: updater
                    ? Math.round(updater.downloadedBytes / 1024 / 1024 * 10) / 10
                      + " MB / "
                      + (updater.totalBytes > 0
                         ? Math.round(updater.totalBytes / 1024 / 1024 * 10) / 10
                           + " MB"
                         : "?")
                    : ""
                color: Theme.fg2
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSize.xs
            }
        }

        // Action row.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.sp.s3

            Item { Layout.fillWidth: true }

            // "Later" / dismiss — only meaningful pre-apply.
            Button {
                text: updater && updater.state === 5 ? "Later" : "Skip"
                visible: updater && (updater.state === 3 || updater.state === 5
                                     || updater.state === 7)
                onClicked: {
                    if (updater) {
                        updateDialog._dismissedFor = updater.availableVersion;
                    }
                    updateDialog.close();
                }
                contentItem: Text {
                    text: parent.text
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    color: Theme.fg1
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: parent.hovered ? Theme.bg3 : Theme.bg2
                    border.color: Theme.line
                    border.width: 1
                    radius: Theme.r2
                    implicitWidth: 96
                    implicitHeight: 36
                }
            }

            // Primary action — varies by state.
            Button {
                visible: updater && (updater.state === 3 || updater.state === 5)
                text: updater && updater.state === 5
                    ? "Restart to install" : "Download"
                onClicked: {
                    if (!updater) return;
                    if (updater.state === 3) updater.downloadUpdate();
                    else if (updater.state === 5) updater.applyUpdate();
                }
                contentItem: Text {
                    text: parent.text
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    font.weight: Theme.fontWeight.semibold
                    color: Theme.onAccent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: parent.hovered ? Theme.accentDim : Theme.accent
                    radius: Theme.r2
                    implicitWidth: 160
                    implicitHeight: 36
                }
            }

            // Failed-state retry button.
            Button {
                visible: updater && updater.state === 7
                text: "Retry"
                onClicked: if (updater) updater.checkNow()
                contentItem: Text {
                    text: parent.text
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    font.weight: Theme.fontWeight.semibold
                    color: Theme.onAccent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: parent.hovered ? Theme.accentDim : Theme.accent
                    radius: Theme.r2
                    implicitWidth: 96
                    implicitHeight: 36
                }
            }
        }
    }
}
