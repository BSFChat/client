import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// "Your roles" — the member-facing half of self-assignable roles.
//
// Opened from the user menu in the channel-list footer, which has no
// permission gate at all. That is the whole reason it is not a page inside
// ServerSettings.qml: the settings gear is only rendered for a member holding
// MANAGE_ROLES / MANAGE_CHANNELS / KICK / BAN, and every member this picker
// exists for holds none of them. Putting an opt-in list behind the admin
// editor would have left the surface exactly as unreachable as it was when
// the only way in was typing `!boss <name>` at a bot.
//
// It is also not a section of User Settings, though that was the near miss:
// that dialog is a fixed 380px modal ending in Log out, and this list is one
// row per published opt-in role — a boss-notification server has dozens. It
// wants its own scrolling surface.
//
// EVERY decision about what to show is made in SelfRoleModel (C++, unit
// tested in tests/test_self_roles.cpp): which roles are on offer, whether one
// is blocked by the containment rule, what the tick says while a request is
// in flight, and what the error line reads. This file renders `roles` and
// calls toggle(). It deliberately does not filter, because a filter written
// here is a filter no headless test can reach.
Popup {
    id: popup

    readonly property var model: serverManager.activeServer
        ? serverManager.activeServer.selfRoleModel : null
    readonly property var rows: model ? model.roles : []

    function openPicker() {
        // Clear a refusal left over from the last time this was open — it
        // described a click the user has since walked away from, and a stale
        // red line at the top of a freshly opened dialog reads as a new
        // problem. Same reasoning as UserProfileCard's clean-slate open.
        if (model) model.dismissError();
        open();
    }

    anchors.centerIn: Overlay.overlay
    width: Math.min(parent ? parent.width * 0.85 : 460, 460)
    height: Math.min(parent ? parent.height * 0.85 : 560, 560)
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 0

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1
    }

    contentItem: ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── header ──────────────────────────────────────────────────────
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 72
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.sp.s7
                anchors.rightMargin: Theme.sp.s5
                spacing: Theme.sp.s4

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Text {
                        text: "Your roles"
                        color: Theme.fg0
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.lg
                        font.weight: Theme.fontWeight.semibold
                        font.letterSpacing: Theme.trackTight.lg
                    }
                    Text {
                        text: "Roles this server lets you add and remove yourself."
                        color: Theme.fg3
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    Layout.alignment: Qt.AlignTop
                    Layout.topMargin: 2
                    radius: Theme.r1
                    color: closeXMouse.containsMouse ? Theme.bg3 : "transparent"
                    Icon {
                        anchors.centerIn: parent
                        name: "x"; size: 14
                        color: closeXMouse.containsMouse ? Theme.fg0 : Theme.fg2
                    }
                    MouseArea {
                        id: closeXMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: popup.close()
                    }
                }
            }
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.line
            }
        }

        // ── refusal line ────────────────────────────────────────────────
        // Inline, directly above the list, rather than a toast: the tick that
        // sprang back is three rows down and the sentence explaining it needs
        // to be in the same glance. Dismissable, and cleared by the next
        // successful toggle.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? errorRow.implicitHeight + Theme.sp.s5 * 2 : 0
            visible: popup.model && popup.model.errorText !== ""
            color: Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.12)

            RowLayout {
                id: errorRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.sp.s7
                anchors.rightMargin: Theme.sp.s5
                spacing: Theme.sp.s3

                Text {
                    Layout.fillWidth: true
                    text: popup.model ? popup.model.errorText : ""
                    color: Theme.danger
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                }
                Rectangle {
                    Layout.preferredWidth: 22
                    Layout.preferredHeight: 22
                    Layout.alignment: Qt.AlignTop
                    radius: Theme.r1
                    color: errDismiss.containsMouse ? Theme.bg3 : "transparent"
                    Icon {
                        anchors.centerIn: parent
                        name: "x"; size: 12
                        color: Theme.danger
                    }
                    MouseArea {
                        id: errDismiss
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { if (popup.model) popup.model.dismissError(); }
                    }
                }
            }
        }

        // ── the list ────────────────────────────────────────────────────
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ScrollBar.vertical: ThemedScrollBar {}
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: popup.width
                spacing: 2

                Item { Layout.preferredHeight: Theme.sp.s3 }

                // Empty state. Only ever "this server publishes none" — the
                // model reports loaded=false before a role document has been
                // seen at all, and saying nothing is better than telling a
                // member there is nothing on offer a quarter second before
                // the list fills in.
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.sp.s7
                    Layout.rightMargin: Theme.sp.s7
                    Layout.topMargin: Theme.sp.s7
                    spacing: Theme.sp.s2
                    visible: popup.rows.length === 0 && popup.model && popup.model.loaded

                    Text {
                        text: "No self-assignable roles"
                        color: Theme.fg1
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.semibold
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: "Nobody has published a role you can pick on this "
                              + "server yet. An administrator marks a role "
                              + "self-assignable in Server Settings → Roles."
                        color: Theme.fg3
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                    }
                }

                Repeater {
                    model: popup.rows
                    delegate: Rectangle {
                        id: roleRow
                        required property var modelData

                        readonly property string roleId: modelData.id
                        readonly property bool held: modelData.held === true
                        readonly property bool pending: modelData.pending === true
                        readonly property bool blocked: modelData.blocked === true

                        Layout.fillWidth: true
                        Layout.leftMargin: Theme.sp.s5
                        Layout.rightMargin: Theme.sp.s5
                        implicitHeight: rowCol.implicitHeight + Theme.sp.s3 * 2
                        radius: Theme.r1
                        color: (roleHover.containsMouse && !roleRow.blocked)
                               ? Theme.bg2 : "transparent"
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                        // A blocked row is readable, not interactive — it is
                        // there to explain a role the member is stuck with.
                        opacity: roleRow.blocked ? 0.75 : 1.0

                        ColumnLayout {
                            id: rowCol
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: Theme.sp.s3
                            anchors.rightMargin: Theme.sp.s3
                            spacing: 2

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.sp.s3

                                ThemedCheckBox {
                                    id: roleBox
                                    checked: roleRow.held
                                    enabled: !roleRow.blocked && !roleRow.pending
                                    // The model owns the tick. Without this
                                    // the control would latch its own state
                                    // on click and then disagree with the
                                    // server's answer forever — which is the
                                    // exact "silently diverge" this feature
                                    // must not do.
                                    onToggled: {
                                        checked = roleRow.held;
                                        if (popup.model) popup.model.toggle(roleRow.roleId);
                                    }
                                }
                                // Role colour dot, as the member list and the
                                // admin editor draw it.
                                Rectangle {
                                    Layout.alignment: Qt.AlignVCenter
                                    width: 12; height: 12; radius: 6
                                    color: roleRow.modelData.color || Theme.accent
                                    border.color: Theme.bg0
                                    border.width: 1
                                }
                                Text {
                                    Layout.alignment: Qt.AlignVCenter
                                    Layout.fillWidth: true
                                    text: roleRow.modelData.name || roleRow.roleId
                                    color: roleRow.modelData.color || Theme.fg0
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.fontSize.md
                                    font.weight: Theme.fontWeight.medium
                                    elide: Text.ElideRight
                                }
                                Icon {
                                    visible: roleRow.blocked
                                    name: "lock"
                                    size: 14
                                    color: Theme.fg3
                                }
                                Text {
                                    visible: roleRow.pending
                                    text: "…"
                                    color: Theme.fg3
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.fontSize.md
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                Layout.leftMargin: 18 + Theme.sp.s3
                                visible: roleRow.blocked
                                text: roleRow.modelData.blockedReason || ""
                                wrapMode: Text.WordWrap
                                color: Theme.fg3
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.xs
                            }
                        }

                        MouseArea {
                            id: roleHover
                            anchors.fill: parent
                            hoverEnabled: true
                            enabled: !roleRow.blocked && !roleRow.pending
                            cursorShape: Qt.PointingHandCursor
                            // Whole row toggles, matching RoleAssignPopup.
                            onClicked: {
                                if (popup.model) popup.model.toggle(roleRow.roleId);
                            }
                        }
                    }
                }

                Item { Layout.preferredHeight: Theme.sp.s3 }
            }
        }

        // ── footer ──────────────────────────────────────────────────────
        // No Save. Each tick is its own request and the server is idempotent,
        // so there is no batch to commit and nothing to lose by closing the
        // dialog mid-flight.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 1
                color: Theme.line
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.sp.s7
                anchors.rightMargin: Theme.sp.s7

                Text {
                    Layout.fillWidth: true
                    text: {
                        if (!popup.model) return "";
                        if (popup.model.busy) return "Saving…";
                        return "Changes save as you tick them.";
                    }
                    color: Theme.fg3
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    elide: Text.ElideRight
                }

                Button {
                    id: doneBtn
                    contentItem: Text {
                        text: "Done"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.onAccent
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: doneBtn.hovered ? Theme.accentDim : Theme.accent
                        radius: Theme.r2
                        implicitWidth: 100
                        implicitHeight: 34
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                    onClicked: popup.close()
                }
            }
        }
    }
}
