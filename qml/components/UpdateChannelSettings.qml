import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat
import "../js/UpdateFormat.js" as UF

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
                        // Version spelling goes through UF.vtag like every
                        // other version in the updater UI; this badge used
                        // to print a bare "0.0.44-rc.2" a few rows under a
                        // panel that said "v0.0.44-rc.2".
                        text: root._hasUpdater
                            ? updater.buildLabel + " "
                              + UF.vtag(updater.currentVersion)
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

    // ---- What the toggle means for a build ahead of its channel ----
    //
    // The opt-out case the product decision creates: you were on an RC,
    // you turned beta off, and the newest stable release is older than
    // what you are running.
    //
    // The Updater reports that as its own state (AheadOfChannel) and the
    // UpdatePanel above already states the fact — which build, which
    // channel, which stable release, nothing to install. This banner
    // therefore covers only the part that belongs to the switch it sits
    // under: what happens next and what the switch would change. It used
    // to restate the versions too, which put the same sentence on screen
    // twice once the pane grew a status panel.
    InfoBanner {
        visible: root._aheadOfChannel
        icon: "bolt"
        tint: Theme.warn
        text: root._onBeta
            ? "You'll be prompted again once a newer build is published "
              + "on the beta channel. Turning the switch off would leave "
              + "this build in place and wait for stable to catch up."
            : "You'll be prompted again once a stable release goes past "
              + "this build. Turn the beta channel back on to follow "
              + "release candidates again."
    }
}
