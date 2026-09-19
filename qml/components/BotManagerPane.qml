import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BSFChat

// Bot management — the "Bots" page of Server Settings.
//
// A pane rather than a popup of its own, because Server Settings IS the modal
// this belongs in: bots are server-scope accounts governed by a server-scope
// permission, and every other thing in that category (roles, members, bans)
// is a page here. Opening a second modal on top of the first to manage them
// would put the bot list somewhere the admin cannot reach from the roles
// editor they just granted MANAGE_BOTS in.
//
// Everything with a decision in it lives in BotAdminModel (C++, unit-tested).
// This file is bindings: it renders `botAdminModel.bots`, calls four
// Q_INVOKABLEs, and owns exactly one piece of state of its own — the id
// awaiting deactivation confirmation.
//
// ─────────────────────────── the token ───────────────────────────
//
// A bot's access token exists outside the server for the length of one
// reply. It is shown in the banner below and nowhere else. Nothing in this
// file may console.log it, put it in a ToolTip that outlives the banner, or
// write it anywhere that persists — the client mirrors console output into a
// rotating log file on disk and QSettings is plain text on every platform we
// ship. The clipboard is the one export, because it is the one the operator
// explicitly asked for by clicking Copy.
Item {
    id: botPane

    readonly property var _server: serverManager.activeServer
    readonly property var _model: _server ? _server.botAdminModel : null
    // Re-evaluated on every permission tick, the same way every other
    // permission-gated surface in this popup does it: a role edit that grants
    // MANAGE_BOTS must open this page without a reconnect.
    readonly property int _gen: _server ? _server.permissionsGeneration : 0
    readonly property bool _mayManage: {
        _gen;
        return !!_server && _server.canManageBots();
    }

    // The bot the confirmation dialog is currently asking about. Empty when
    // no confirmation is up. Held here rather than on the delegate because
    // `bots` is a QVariantList snapshot that is replaced wholesale on every
    // refresh — the delegate holding the state can be destroyed out from
    // under an open dialog, which is the same trap D-M4 describes for the
    // role editor's scratch state further up this file's sibling.
    property string pendingDeactivateId: ""

    // Populate on first reveal and whenever the admin comes back to the page.
    // Not on construction: the pane is built with the rest of Server Settings
    // for every user, and a member without MANAGE_BOTS would issue a request
    // that can only ever 403.
    onVisibleChanged: {
        if (visible && _mayManage && _model) _model.refresh();
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.sp.s7 * 2
        spacing: Theme.sp.s7

        RowLayout {
            Layout.fillWidth: true
            Text {
                text: "Bots"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xxl
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackTight.xxl
                color: Theme.fg0
                Layout.fillWidth: true
            }
            // Manual refresh. The list has no push channel — bots do not
            // arrive through /sync — so a second admin creating one is
            // invisible here until something asks again.
            Button {
                id: refreshBtn
                visible: botPane._mayManage
                enabled: botPane._model && !botPane._model.busy
                implicitHeight: Theme.controlHeight.sm
                contentItem: Text {
                    text: "Refresh"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    color: refreshBtn.enabled ? Theme.fg1 : Theme.fg3
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    implicitWidth: 88
                    radius: Theme.r2
                    color: refreshBtn.hovered ? Theme.bg3 : "transparent"
                    border.color: Theme.line
                    border.width: 1
                }
                onClicked: botPane._model.refresh()
            }
        }
        Rectangle {
            Layout.fillWidth: true
            // Layout.preferredHeight, not height: a plain `height` on a
            // layout-managed item is undefined behaviour (qmllint flags it),
            // even though it happens to work for the sibling rules elsewhere
            // in Server Settings.
            Layout.preferredHeight: 1
            color: Theme.line
        }

        // ── No permission ─────────────────────────────────────────────────
        //
        // Shown rather than hiding the nav row, so a server owner who has not
        // granted themselves MANAGE_BOTS can find out that bots exist and
        // what to do about it. Naming the permission is the whole value of
        // this state — "you don't have permission" alone sends people to
        // support.
        ColumnLayout {
            Layout.fillWidth: true
            visible: !botPane._mayManage
            spacing: Theme.sp.s3
            InfoBanner {
                icon: "lock"
                text: "You need the \"Manage bots\" permission to create or manage bot accounts on this server. An administrator can grant it to one of your roles in the Roles tab."
            }
            Item { Layout.fillHeight: true }
        }

        // ── The one-time token banner ─────────────────────────────────────
        //
        // Accent-bordered and unmissable on purpose. It does not auto-dismiss,
        // does not time out, and is not cleared by the list refresh that lands
        // underneath it a moment later: the only way out is the button, which
        // is labelled with what the operator is asserting when they press it.
        Rectangle {
            Layout.fillWidth: true
            visible: botPane._mayManage && !!botPane._model
                     && botPane._model.hasIssuedToken
            Layout.preferredHeight: tokenColumn.implicitHeight + Theme.sp.s5 * 2
            radius: Theme.r2
            color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.10)
            border.color: Theme.accent
            border.width: 1

            ColumnLayout {
                id: tokenColumn
                anchors.fill: parent
                anchors.margins: Theme.sp.s5
                spacing: Theme.sp.s3

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp.s3
                    Icon { name: "lock"; size: 16; color: Theme.accent
                           Layout.alignment: Qt.AlignVCenter }
                    Text {
                        Layout.fillWidth: true
                        text: botPane._model && botPane._model.issuedTokenIsRotation
                            ? "New token for " + (botPane._model
                                  ? botPane._model.displayNameFor(botPane._model.issuedTokenUserId)
                                  : "")
                            : "Bot created — here is its token"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.lg
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.fg0
                        wrapMode: Text.WordWrap
                    }
                }

                Text {
                    Layout.fillWidth: true
                    // The two cases genuinely differ and an operator acts on
                    // the difference: after a rotation something is already
                    // broken and needs the new value, whereas after a create
                    // nothing is deployed yet.
                    text: botPane._model && botPane._model.issuedTokenIsRotation
                        ? "This is the only time this token is shown. The bot's previous token stopped working the moment this one was issued — anything running with it is signed out until you update it."
                        : "This is the only time this token is shown. The server keeps no copy you can read back. If you lose it, the only way to recover is to rotate, which invalidates whatever you have already deployed."
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    color: Theme.fg1
                    wrapMode: Text.WordWrap
                }

                // The value itself. Read-only and selectable so it can be
                // taken by keyboard as well as by the button; `TextEdit` with
                // no write path rather than a TextField, so there is no
                // editable buffer holding a credential.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(Theme.controlHeight.md,
                                                     tokenText.implicitHeight + Theme.sp.s3 * 2)
                    radius: Theme.r2
                    color: Theme.bg0
                    border.color: Theme.line
                    border.width: 1
                    TextEdit {
                        id: tokenText
                        anchors.fill: parent
                        anchors.margins: Theme.sp.s3
                        text: botPane._model ? botPane._model.issuedToken : ""
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.WrapAnywhere
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSize.sm
                        color: Theme.fg0
                        verticalAlignment: TextEdit.AlignVCenter
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp.s3

                    Button {
                        id: copyTokenBtn
                        implicitHeight: Theme.controlHeight.sm
                        // Flips to a confirmation for a moment. A copy button
                        // that does nothing visible is one an operator presses
                        // twice and then distrusts — and distrusting this one
                        // means dismissing the banner to "try again", which
                        // destroys the token.
                        property bool copied: false
                        contentItem: RowLayout {
                            spacing: Theme.sp.s2
                            Icon {
                                name: copyTokenBtn.copied ? "check" : "copy"
                                size: 14; color: Theme.onAccent
                                Layout.leftMargin: Theme.sp.s3
                                Layout.alignment: Qt.AlignVCenter
                            }
                            Text {
                                text: copyTokenBtn.copied ? "Copied" : "Copy token"
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.sm
                                font.weight: Theme.fontWeight.semibold
                                color: Theme.onAccent
                                Layout.rightMargin: Theme.sp.s3
                                Layout.alignment: Qt.AlignVCenter
                            }
                        }
                        background: Rectangle {
                            radius: Theme.r2
                            color: copyTokenBtn.hovered ? Theme.accentDim : Theme.accent
                        }
                        onClicked: {
                            // Straight from the model to the clipboard. Not
                            // via a local property: a copy held in QML is a
                            // copy that outlives dismissToken().
                            serverManager.copyToClipboard(botPane._model.issuedToken);
                            copied = true;
                            copiedResetTimer.restart();
                        }
                        Timer {
                            id: copiedResetTimer
                            interval: 1600
                            onTriggered: copyTokenBtn.copied = false
                        }
                    }

                    Button {
                        id: dismissTokenBtn
                        implicitHeight: Theme.controlHeight.sm
                        contentItem: Text {
                            text: "I've saved it — hide"
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.sm
                            color: Theme.fg1
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            implicitWidth: 170
                            radius: Theme.r2
                            color: dismissTokenBtn.hovered ? Theme.bg3 : "transparent"
                            border.color: Theme.line
                            border.width: 1
                        }
                        onClicked: {
                            copyTokenBtn.copied = false;
                            botPane._model.dismissToken();
                        }
                    }
                    Item { Layout.fillWidth: true }
                }
            }
        }

        // ── Error line ────────────────────────────────────────────────────
        InfoBanner {
            visible: botPane._mayManage && !!botPane._model
                     && botPane._model.errorText.length > 0
            icon: "x"
            tint: Theme.danger
            text: botPane._model ? botPane._model.errorText : ""
        }

        // ── Create ────────────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            visible: botPane._mayManage
            spacing: Theme.sp.s3

            Text {
                text: "ADD A BOT"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWidest.xs
                color: Theme.fg3
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp.s3

                component BotField: TextField {
                    id: botFieldRoot
                    color: Theme.fg0
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    leftPadding: Theme.sp.s4
                    rightPadding: Theme.sp.s4
                    topPadding: Theme.sp.s3
                    bottomPadding: Theme.sp.s3
                    background: Rectangle {
                        color: Theme.bg0
                        radius: Theme.r2
                        // Through the component's own id, not `parent`: the
                        // background's parent IS the control, but saying so
                        // explicitly survives anyone reparenting it.
                        border.color: botFieldRoot.activeFocus ? Theme.accent : Theme.line
                        border.width: 1
                    }
                }

                BotField {
                    id: localpartField
                    Layout.preferredWidth: 180
                    placeholderText: "username"
                    // The server owns the real rule; this is the same check
                    // BotAdminModel.localpartError applies, asked here on
                    // every keystroke so Create can be disabled rather than
                    // attempted. Asking C++ keeps one copy of the grammar.
                    readonly property string problem:
                        text.length === 0 ? ""
                        : (botPane._model ? botPane._model.validateLocalpart(text) : "")
                }
                BotField {
                    id: botNameField
                    Layout.preferredWidth: 180
                    placeholderText: "Display name"
                }
                BotField {
                    id: botDescField
                    Layout.fillWidth: true
                    placeholderText: "What is it for? (optional)"
                }

                Button {
                    id: createBotBtn
                    implicitHeight: Theme.controlHeight.md
                    enabled: !!botPane._model && !botPane._model.busy
                             && localpartField.text.length > 0
                             && localpartField.problem.length === 0
                    contentItem: RowLayout {
                        spacing: Theme.sp.s2
                        Icon {
                            name: "plus"; size: 14
                            color: createBotBtn.enabled ? Theme.onAccent : Theme.fg3
                            Layout.leftMargin: Theme.sp.s3
                            Layout.alignment: Qt.AlignVCenter
                        }
                        Text {
                            text: "Create bot"
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.md
                            font.weight: Theme.fontWeight.semibold
                            color: createBotBtn.enabled ? Theme.onAccent : Theme.fg3
                            Layout.rightMargin: Theme.sp.s3
                            Layout.alignment: Qt.AlignVCenter
                        }
                    }
                    background: Rectangle {
                        radius: Theme.r2
                        color: !createBotBtn.enabled ? Theme.bg3
                             : createBotBtn.hovered ? Theme.accentDim : Theme.accent
                    }
                    onClicked: {
                        botPane._model.createBot(localpartField.text.trim(),
                                                 botNameField.text.trim(),
                                                 botDescField.text.trim());
                        // Cleared optimistically. A failure puts the reason in
                        // the error line above, and re-typing a rejected
                        // username is cheaper than an admin wondering whether
                        // the second click created a second bot.
                        localpartField.clear();
                        botNameField.clear();
                        botDescField.clear();
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                visible: localpartField.problem.length > 0
                text: localpartField.problem
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                color: Theme.danger
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                visible: localpartField.problem.length === 0
                text: "The bot gets its own account and an access token you'll see once."
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                color: Theme.fg3
                wrapMode: Text.WordWrap
            }
        }

        // ── The list ──────────────────────────────────────────────────────
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: botPane._mayManage
            clip: true
            spacing: 4
            ScrollBar.vertical: ThemedScrollBar { id: botScrollBar }
            model: botPane._model ? botPane._model.bots : []

            // Two different empty states — "nothing here" and "not asked
            // yet" look identical from an empty list, and showing the first
            // while the request is still in flight reads as a failure.
            Text {
                anchors.centerIn: parent
                width: parent.width - Theme.sp.s7 * 2
                visible: parent.count === 0
                text: (botPane._model && botPane._model.loaded)
                    ? "No bots on this server yet."
                    : "Loading bots…"
                horizontalAlignment: Text.AlignHCenter
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.md
                color: Theme.fg3
                wrapMode: Text.WordWrap
            }

            delegate: Rectangle {
                id: botRow
                width: (ListView.view ? ListView.view.width : 400)
                       - botScrollBar.reservedWidth
                height: 56
                radius: Theme.r2
                color: botRowMouse.containsMouse ? Theme.bg3 : Theme.bg2
                border.color: Theme.line
                border.width: 1
                // A deactivated bot is dimmed rather than removed — it still
                // exists, still owns its user id, and its old messages still
                // carry the badge.
                opacity: modelData.deactivated ? 0.55 : 1.0

                MouseArea {
                    id: botRowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.sp.s5
                    anchors.rightMargin: Theme.sp.s3
                    spacing: Theme.sp.s4

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.sp.s2
                            Text {
                                text: modelData.displayName || modelData.userId
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.md
                                font.weight: Theme.fontWeight.semibold
                                color: Theme.fg0
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            BotBadge { Layout.alignment: Qt.AlignVCenter }
                            Text {
                                visible: modelData.deactivated === true
                                text: "DEACTIVATED"
                                font.family: Theme.fontSans
                                font.pixelSize: 9
                                font.weight: Theme.fontWeight.bold
                                font.letterSpacing: 0.5
                                color: Theme.danger
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: modelData.description && modelData.description.length > 0
                                ? modelData.description : modelData.userId
                            font.family: modelData.description
                                         && modelData.description.length > 0
                                         ? Theme.fontSans : Theme.fontMono
                            font.pixelSize: Theme.fontSize.xs
                            color: Theme.fg3
                            elide: Text.ElideRight
                        }
                    }

                    // Copy the bot's user id — the value an operator actually
                    // needs to configure the thing at the other end, and the
                    // one piece of a bot's identity that IS safe to hand out.
                    Button {
                        id: copyIdBtn
                        implicitHeight: Theme.controlHeight.sm
                        implicitWidth: Theme.controlHeight.sm
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: "Copy user ID"
                        contentItem: Icon {
                            name: "copy"; size: 14
                            color: copyIdBtn.hovered ? Theme.fg0 : Theme.fg2
                        }
                        background: Rectangle {
                            radius: Theme.r1
                            color: copyIdBtn.hovered ? Theme.bg1 : "transparent"
                        }
                        onClicked: serverManager.copyToClipboard(modelData.userId)
                    }

                    Button {
                        id: rotateBtn
                        implicitHeight: Theme.controlHeight.sm
                        enabled: !!botPane._model && !botPane._model.busy
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: "Issues a new token and immediately stops the old one working."
                        contentItem: Text {
                            text: "Rotate token"
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.sm
                            color: rotateBtn.enabled ? Theme.fg1 : Theme.fg3
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            implicitWidth: 112
                            radius: Theme.r2
                            color: rotateBtn.hovered ? Theme.bg1 : "transparent"
                            border.color: Theme.line
                            border.width: 1
                        }
                        onClicked: botPane._model.rotateToken(modelData.userId)
                    }

                    Button {
                        id: deactivateBtn
                        implicitHeight: Theme.controlHeight.sm
                        implicitWidth: Theme.controlHeight.sm
                        // Hidden, not disabled, once the bot is off: the
                        // server call is idempotent so a second press is
                        // harmless, but offering it says the row is still
                        // actionable when it is not.
                        visible: modelData.deactivated !== true
                        enabled: !!botPane._model && !botPane._model.busy
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: "Deactivate"
                        contentItem: Icon {
                            name: "x"; size: 14
                            color: deactivateBtn.hovered ? Theme.danger : Theme.fg2
                        }
                        background: Rectangle {
                            radius: Theme.r1
                            color: deactivateBtn.hovered
                                ? Qt.rgba(Theme.danger.r, Theme.danger.g,
                                          Theme.danger.b, 0.15)
                                : "transparent"
                        }
                        onClicked: {
                            botPane.pendingDeactivateId = modelData.userId;
                            deactivateConfirm.open();
                        }
                    }
                }
            }
        }
    }

    // ── Deactivate confirmation ───────────────────────────────────────────
    //
    // Same vocabulary as the kick/ban confirm in ServerSettings: bg1 + r3,
    // danger-tinted icon tile, ghost Cancel, filled destructive action.
    Popup {
        id: deactivateConfirm
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        width: 420
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: Theme.sp.s7
        onClosed: botPane.pendingDeactivateId = ""

        background: Rectangle {
            color: Theme.bg1
            radius: Theme.r3
            border.color: Theme.line
            border.width: 1
        }

        contentItem: ColumnLayout {
            spacing: Theme.sp.s4

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp.s3
                Rectangle {
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: Theme.controlHeight.sm
                    radius: Theme.r2
                    color: Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.15)
                    Icon { anchors.centerIn: parent; name: "x"; size: 16
                           color: Theme.danger }
                }
                Text {
                    Layout.fillWidth: true
                    text: "Deactivate "
                          + (botPane._model
                             ? botPane._model.displayNameFor(botPane.pendingDeactivateId)
                             : botPane.pendingDeactivateId)
                          + "?"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xl
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackTight.xl
                    color: Theme.fg0
                    wrapMode: Text.WordWrap
                }
            }

            Text {
                Layout.fillWidth: true
                // Says what actually happens, including the parts that do
                // NOT: an admin deciding whether to press this needs to know
                // the history stays and the id is not freed up.
                text: "Its token stops working immediately and it can no longer sign in or post. Messages it has already sent stay in their channels, and its user ID stays taken."
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.sp.s2
                spacing: Theme.sp.s3
                Item { Layout.fillWidth: true }

                Button {
                    id: cancelDeactivateBtn
                    implicitHeight: Theme.controlHeight.md
                    contentItem: Text {
                        text: "Cancel"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        color: Theme.fg1
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        implicitWidth: 92
                        radius: Theme.r2
                        color: cancelDeactivateBtn.hovered ? Theme.bg3 : "transparent"
                        border.color: Theme.line
                        border.width: 1
                    }
                    onClicked: deactivateConfirm.close()
                }

                Button {
                    id: confirmDeactivateBtn
                    implicitHeight: Theme.controlHeight.md
                    contentItem: Text {
                        text: "Deactivate"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.semibold
                        color: "#ffffff"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        implicitWidth: 120
                        radius: Theme.r2
                        color: confirmDeactivateBtn.hovered
                            ? Qt.darker(Theme.danger, 1.15) : Theme.danger
                    }
                    onClicked: {
                        // Read the id before close() clears it — onClosed
                        // fires synchronously from close().
                        var target = botPane.pendingDeactivateId;
                        deactivateConfirm.close();
                        if (botPane._model && target.length > 0)
                            botPane._model.deactivateBot(target);
                    }
                }
            }
        }
    }
}
