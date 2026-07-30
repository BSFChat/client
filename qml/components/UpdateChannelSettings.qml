import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// Beta-channel opt-in for the Updates pane, kept in its own file so it
// can be dropped into ClientSettings.qml with a single line:
//
//     UpdateChannelSettings { Layout.fillWidth: true }
//
// Self-contained on purpose — it builds its own row instead of using
// ClientSettings' inline SettingRow/SectionHeader components, which are
// scoped to that file and not visible from here.
//
// Backed by:
//   appSettings.updateChannel  "stable" | "beta"  (persisted)
//   updater.state              Updater::State; 8 == AheadOfChannel
//   updater.prereleaseBuild    is the RUNNING build a prerelease?
//   updater.buildLabel         "RC" / "BETA" / "DEV" / "PRE" / ""
//   updater.latestStableVersion newest non-prerelease tag last seen
//
// Wording rule: this component talks about release channels only. It
// makes no claim about how any build behaves beyond what the channel
// mechanically does, because there is nothing behind such a claim.
ColumnLayout {
    id: root
    Layout.fillWidth: true
    spacing: Theme.sp.s5

    readonly property bool _hasUpdater: typeof updater !== "undefined"
    readonly property bool _onBeta:
        typeof appSettings !== "undefined"
        && appSettings.updateChannel === "beta"
    // Updater::State::AheadOfChannel — running a build newer than
    // anything the selected channel publishes.
    readonly property bool _aheadOfChannel:
        root._hasUpdater && updater.state === 8

    // ---- Channel toggle -------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.sp.s7

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp.s3

                Text {
                    text: "Beta channel"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    font.weight: Theme.fontWeight.semibold
                    color: Theme.fg0
                }

                // Badge for the build you are actually running. It sits
                // next to the channel switch rather than replacing the
                // version readout above, so "which build am I on" stays
                // answerable at a glance even after opting out.
                Rectangle {
                    visible: root._hasUpdater && updater.prereleaseBuild
                    radius: Theme.r1
                    color: Qt.rgba(Theme.warn.r, Theme.warn.g, Theme.warn.b, 0.16)
                    border.color: Theme.warn
                    border.width: 1
                    implicitWidth: buildBadge.implicitWidth + Theme.sp.s3 * 2
                    implicitHeight: buildBadge.implicitHeight + Theme.sp.s1 * 2
                    Text {
                        id: buildBadge
                        anchors.centerIn: parent
                        text: root._hasUpdater
                            ? updater.buildLabel + " " + updater.currentVersion
                            : ""
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSize.xs
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.fg0
                    }
                }

                Item { Layout.fillWidth: true }
            }

            Text {
                Layout.fillWidth: true
                text: "Also offer release candidates — builds published "
                    + "ahead of a final release for testing. They arrive "
                    + "through the same update prompt as ordinary "
                    + "releases. Turning this off leaves your current "
                    + "build in place; nothing is rolled back."
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
                wrapMode: Text.WordWrap
            }
        }

        ThemedSwitch {
            Layout.alignment: Qt.AlignVCenter
            checked: root._onBeta
            onToggled: {
                if (typeof appSettings === "undefined") return;
                appSettings.updateChannel = checked ? "beta" : "stable";
                // Re-check immediately: the answer to "is there an
                // update" changes with the channel, and leaving the
                // previous channel's verdict on screen for up to six
                // hours is how a stale "up to date" outlives its truth.
                if (root._hasUpdater) updater.checkNow();
            }
        }
    }

    // ---- Honest state for a build ahead of its channel -------------
    //
    // The opt-out case the product decision creates: you were on an RC,
    // you turned beta off, and the newest stable release is older than
    // what you are running. Nothing is offered, but "You're up to date"
    // would be false, so the Updater reports a distinct state and this
    // banner explains it rather than leaving the pane silent.
    InfoBanner {
        visible: root._aheadOfChannel
        icon: "bolt"
        tint: Theme.warn
        text: {
            if (!root._hasUpdater) return "";
            var stable = updater.latestStableVersion;
            var head = "This build (" + updater.currentVersion
                     + ") is newer than the latest "
                     + (root._onBeta ? "release on the beta channel"
                                     : "stable release")
                     + (stable ? " (" + stable + ")" : "") + ".";
            return head + " No update is being offered. You'll be "
                 + "prompted again once a "
                 + (root._onBeta ? "newer build" : "stable release")
                 + " goes past this one — or turn the beta channel "
                 + (root._onBeta ? "off" : "back on")
                 + " to follow release candidates again.";
        }
    }
}
