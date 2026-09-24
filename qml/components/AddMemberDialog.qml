import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// "Add someone to this channel" — the client half of POST /rooms/{id}/invite.
//
// Opened from two places, both channel-scoped: the + in the member-list header
// (the panel that answers "who is in here", where "and one more" belongs) and
// "Add member…" in a channel's right-click menu (where Discord puts "Invite
// People", so it is where people look). Both hand it a roomId; nothing here
// reads activeRoomId, because the context menu can be opened on a channel that
// is not the active one.
//
// Everything with a decision in it — validation, what the success line says,
// which of six identical-looking 403s just happened — is in
// ChannelInviteModel (C++, unit-tested in tests/test_channel_invite.cpp).
// This file is bindings and one piece of local state: the text in the field.
//
// ───────────────────── why it opens without the permission ─────────────────
//
// The + and the menu entry do not disappear for a member who lacks
// MANAGE_CHANNELS, and this dialog opens for them onto a locked state that
// names the permission and the account. That is the precedent
// BotManagerPane.qml set and fix/identity-permission-discoverability then
// applied to the Server Settings gear, and it is the same incident behind
// both: on 2026-09-20 a server's own owner could not find a feature because
// the entry point had silently vanished for the OIDC account he happened to
// be signed in as, while a different account of his held the admin role. A
// missing control says "this product cannot do that". It cannot say "you,
// this account, may not" — and those are the two things the person in front
// of it needs to tell apart.
Popup {
    id: addMemberDialog

    // Set by the caller before open(). `roomName` is display copy only; the
    // request carries the id.
    property string roomId: ""
    property string roomName: ""

    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    // Was a bare `460`. This dialog opens from the member list and from a
    // channel row — both of which live in a drawer on a phone — and 460 is
    // wider than every iPhone in portrait, so it hung ~35 pt off each side
    // of the screen with the title and the Cancel/Add buttons cut in half.
    // Same clamp the rest of the dialogs in this directory use; the gutter
    // is the phone chrome gutter (Theme.mobileGutter) so a dialog sits in
    // line with everything else behind it.
    width: Math.min(460, (parent ? parent.width : 460) - 2 * Theme.mobileGutter)
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: Theme.sp.s7

    readonly property var _server: serverManager.activeServer
    readonly property var _model: _server ? _server.channelInviteModel : null
    // permissionsGeneration is a real int dependency the AOT compiler will not
    // eliminate; every apply*Event handler in ServerConnection bumps it. Every
    // permission-gated surface in this client reads it for the same reason: a
    // role edit granting MANAGE_CHANNELS must unlock this without a reconnect.
    readonly property int _gen: _server ? _server.permissionsGeneration : 0

    // MANAGE_CHANNELS, in THIS room — exactly what the server checks.
    //
    // server/src/api/RoomHandler.cpp handle_invite():
    //     perms.can(*user_id, room_id, permission::kManageChannels)
    // with the comment "Inviting piggybacks on MANAGE_CHANNELS for now — we
    // don't have a separate flag." Channel scope is not incidental: a
    // per-channel override grants it in one channel and not the next, so
    // asking with the room id is the difference between a correct gate and a
    // button that 403s in half the channels it appears in.
    //
    // Read through canManageChannel(), which resolves the shared mirror in
    // util/PermissionMath.h. Not a hardcoded bit here: a flag spelled out in
    // QML is a flag no test can compare against protocol, which is exactly how
    // the ADD_REACTIONS drift got through (see
    // thePermissionMirrorHasNotDriftedFromProtocol in tests/test_bots.cpp).
    readonly property bool mayAdd: {
        _gen;
        if (!_server) return false;
        if (_server.permissionsGeneration < 0) return false;
        return _server.canManageChannel(addMemberDialog.roomId);
    }

    readonly property string _channelPhrase:
        roomName.length > 0 ? ("#" + roomName) : "this channel"

    onOpened: {
        if (_model) _model.reset();
        memberIdField.clear();
        if (mayAdd) memberIdField.forceActiveFocus();
    }
    // Cleared on the way out as well as on the way in. A notice left standing
    // would greet the next channel's dialog with the last channel's result.
    onClosed: if (_model) _model.reset()

    function submit() {
        if (!_model || !mayAdd) return;
        if (memberIdField.problem.length > 0) return;
        _model.invite(addMemberDialog.roomId, memberIdField.text.trim(),
                      addMemberDialog.roomName);
    }

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: Theme.sp.s4

        // ── Title ─────────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.sp.s3

            Rectangle {
                Layout.preferredWidth: 32
                Layout.preferredHeight: 32
                radius: Theme.r2
                color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.15)
                Icon {
                    anchors.centerIn: parent
                    name: addMemberDialog.mayAdd ? "users" : "lock"
                    size: 16
                    color: Theme.accent
                }
            }
            Text {
                Layout.fillWidth: true
                text: "Add someone to " + addMemberDialog._channelPhrase
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xl
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackTight.xl
                color: Theme.fg0
                wrapMode: Text.WordWrap
            }
        }

        // ── Locked ────────────────────────────────────────────────────────
        //
        // The account is NAMED, not just the permission. "You need Manage
        // Channels" is unanswerable when the reason you do not have it is
        // that you are signed in as a second account you had forgotten about,
        // which is the shape the 2026-09-20 incident actually took.
        ColumnLayout {
            Layout.fillWidth: true
            visible: !addMemberDialog.mayAdd
            spacing: Theme.sp.s3

            InfoBanner {
                icon: "lock"
                text: "You need the \"Manage Channels\" permission in "
                      + addMemberDialog._channelPhrase
                      + " to add members to it. An administrator can grant it "
                      + "to one of your roles under Server Settings → Roles"
                      + (addMemberDialog._server
                         ? (", or to this channel specifically in Channel "
                            + "Settings → Permissions. You are signed in as "
                            + addMemberDialog._server.userId + ".")
                         : ".")
            }
        }

        // ── The form ──────────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            visible: addMemberDialog.mayAdd
            spacing: Theme.sp.s3

            // The bot rule, said BEFORE the click rather than explained after
            // it. This is the one behaviour nobody arriving from Discord will
            // predict — there, everything is an invite somebody accepts; here,
            // a bot is in the channel the moment you press Add, with a real
            // join event everyone else sees. The sentence lives in the model
            // so a test can assert it still says so.
            Text {
                Layout.fillWidth: true
                text: addMemberDialog._model ? addMemberDialog._model.botAdvisory() : ""
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
                wrapMode: Text.WordWrap
            }

            TextField {
                id: memberIdField
                Layout.fillWidth: true
                placeholderText: "@alice:bsfchat.com"
                color: Theme.fg0
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSize.md
                leftPadding: Theme.sp.s4
                rightPadding: Theme.sp.s4
                topPadding: Theme.sp.s3
                bottomPadding: Theme.sp.s3

                // Asked of C++ on every keystroke so Add can be disabled
                // rather than attempted, and so the grammar has one home.
                readonly property string problem:
                    text.length === 0 ? ""
                    : (addMemberDialog._model
                       ? addMemberDialog._model.validateUserId(text.trim()) : "")
                // "They are not in this channel, and adding them is how they
                // get back in" — the readmit case, which is the one thing on
                // this path that reads like a failure and is not.
                readonly property string readmitNote:
                    text.length === 0 ? ""
                    : (addMemberDialog._model
                       ? addMemberDialog._model.readmitHint(addMemberDialog.roomId,
                                                            text.trim(),
                                                            addMemberDialog.roomName)
                       : "")

                background: Rectangle {
                    color: Theme.bg0
                    radius: Theme.r2
                    // Through the field's own id rather than `parent`, matching
                    // BotField in BotManagerPane.qml: the background's parent
                    // IS the control, but saying so survives a reparent.
                    border.color: memberIdField.problem.length > 0 ? Theme.danger
                                : memberIdField.activeFocus ? Theme.accent
                                : Theme.line
                    border.width: 1
                }

                // Enter submits. The field is the only input, so making the
                // keyboard path work is a two-line courtesy rather than a
                // feature — and an operator adding several bots in a row will
                // otherwise reach for the mouse on every one.
                onAccepted: addMemberDialog.submit()
                // Any edit retires the previous answer. Leaving the last
                // error under a half-retyped id is how a dialog ends up
                // asserting something about text that is no longer there.
                onTextEdited: if (addMemberDialog._model) addMemberDialog._model.reset()
            }

            // Four mutually-exclusive lines under the field, in the order a
            // reader needs them: what is wrong with what you typed, what went
            // wrong when we sent it, what adding this person will do, and
            // what happened when it worked.
            //
            // There was a FIFTH — "what probably went wrong that we cannot
            // prove", a warn-coloured caution that the typed id's homeserver
            // was not ours. It existed because the server used to accept an
            // invite for an account that did not exist and answer 200, so a
            // typo produced a success message and a member who never
            // appeared; guessing at the domain was the closest the client
            // could get to saying so. Server b8e26ac refuses that outright,
            // the refusal arrives as an ordinary 403, and it renders on the
            // error line below like every other one. Nothing here is allowed
            // to speculate about what the server would have said.
            Text {
                Layout.fillWidth: true
                visible: text.length > 0
                text: memberIdField.problem
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.danger
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                visible: text.length > 0 && memberIdField.problem.length === 0
                text: addMemberDialog._model ? addMemberDialog._model.errorText : ""
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.danger
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                visible: text.length > 0 && memberIdField.problem.length === 0
                text: memberIdField.readmitNote
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                visible: text.length > 0
                text: addMemberDialog._model ? addMemberDialog._model.noticeText : ""
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.online
                wrapMode: Text.WordWrap
            }
        }

        // ── Buttons ───────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.sp.s1
            spacing: Theme.sp.s3

            Item { Layout.fillWidth: true }

            Button {
                id: addCloseBtn
                contentItem: Text {
                    // "Done" once something has been added, because the
                    // dialog stays open on success — adding three bots to a
                    // channel is one errand, not three.
                    text: (addMemberDialog._model
                           && addMemberDialog._model.noticeText.length > 0)
                          ? "Done" : "Cancel"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    font.weight: Theme.fontWeight.medium
                    color: Theme.fg1
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: addCloseBtn.hovered ? Theme.bg3 : "transparent"
                    border.color: Theme.line
                    border.width: 1
                    radius: Theme.r2
                    implicitWidth: 100
                    implicitHeight: 36
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                }
                onClicked: addMemberDialog.close()
            }
            Button {
                id: addConfirmBtn
                visible: addMemberDialog.mayAdd
                enabled: !!addMemberDialog._model
                         && !addMemberDialog._model.busy
                         && memberIdField.text.trim().length > 0
                         && memberIdField.problem.length === 0
                contentItem: Text {
                    text: addMemberDialog._model && addMemberDialog._model.busy
                          ? "Adding…" : "Add"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    font.weight: Theme.fontWeight.semibold
                    color: addConfirmBtn.enabled ? Theme.onAccent : Theme.fg3
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: !addConfirmBtn.enabled ? Theme.bg3
                         : addConfirmBtn.hovered ? Theme.accentDim : Theme.accent
                    radius: Theme.r2
                    implicitWidth: 120
                    implicitHeight: 36
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                }
                onClicked: addMemberDialog.submit()
            }
        }
    }
}
