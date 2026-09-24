import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import BSFChat

Popup {
    id: serverSettingsPopup
    anchors.centerIn: Overlay.overlay
    // Sized to what is actually in it. The locked panel is one banner and an
    // account id; rendering it inside an 85%-of-the-window modal would read as
    // a settings dialog that failed to load rather than as an answer.
    //
    // On a phone "85% of the window" is still a phone, and what it has to
    // hold is a 200 pt nav rail plus a page of rows built for a desktop
    // dialog. The unlocked pane takes the whole viewport there; the locked
    // one — a banner and an account id — keeps its small size, because
    // blowing THAT up to full screen is the "settings failed to load" read
    // this sizing exists to avoid.
    width: {
        var full = Theme.isMobile
            ? (parent ? parent.width - 2 * Theme.mobileGutter : 360)
            : (parent ? parent.width * 0.85 : 800);
        return serverSettingsPopup._mayOpen ? full : Math.min(full, 520);
    }
    height: {
        var full = Theme.isMobile
            ? (parent ? parent.height - 2 * Theme.mobileGutter : 600)
            : (parent ? parent.height * 0.85 : 600);
        return serverSettingsPopup._mayOpen ? full : Math.min(full, 420);
    }
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    property int selectedSection: 0
    // One list, read by the desktop nav rail and by the phone's
    // section dropdown. Two copies is how a section gets added to
    // one and not the other.
    //
    // Bots sits after Members because it is a roster of accounts, not
    // a server-shape setting, and before Channels for the same reason.
    // The section is present for everyone but shows a "you don't have
    // permission" state without MANAGE_BOTS — hiding the row entirely
    // makes the feature undiscoverable to the owner who has not yet
    // granted themselves the flag.
    readonly property var sections: ["Overview", "Roles", "Members",
                                     "Bots", "Channels", "Bans"]

    // Whether this account may use any page in here.
    //
    // False is not an error state and not a bug — it is the ordinary
    // condition of every member of every server, and before 2026-09-20 it was
    // expressed by the header gear simply not existing. That made "BSFChat has
    // no server settings" and "THIS account may not use them" the same picture,
    // which cost a support call and a database inspection over SSH: the owner
    // was signed in as an OIDC account holding only @everyone while another
    // account of his held the admin role, and nothing on screen could have told
    // him so. The locked panel below is the sentence that was missing.
    //
    // Asked through ServerConnection so the flag set lives in one place
    // (permmath::kServerSettingsPages) rather than being spelled out again in
    // QML, where it had already drifted from the nav list below.
    readonly property int _permGen:
        serverManager.activeServer ? serverManager.activeServer.permissionsGeneration : 0
    readonly property bool _mayOpen: {
        _permGen;
        var s = serverManager.activeServer;
        return !!s && s.canOpenServerSettings();
    }

    // Emitted when the user clicks a channel row in the Channels tab. The
    // host component is expected to open its ChannelSettings instance for
    // the given room (we don't have access to it from here — it's owned by
    // ChannelList.qml alongside this popup).
    signal channelSettingsRequested(string roomId, string roomName)

    // Emitted when the user clicks the trash icon on a channel row. Host
    // opens its delete-confirmation popup; we don't close the settings
    // popup because the user is mid-task and may delete several in a row.
    signal channelDeleteRequested(string roomId, string roomName)

    // Emitted by the "+ Add channel" / "+ Add category" affordances. Host
    // opens its create-prompt popup seeded with the right kind/parent.
    //   kind: "category" | "text" | "voice"
    //   categoryId: "" for top-level create or for kind=="category"
    signal createChannelRequested(string kind, string categoryId)

    // Per-tab header (24px title + divider rule). SPEC §3.10 section-header.
    // Factored here so each tab body doesn't redeclare the five-property
    // ColumnLayout → Text → Rectangle pattern.
    component TabHeader: ColumnLayout {
        property string title: ""
        Layout.fillWidth: true
        spacing: Theme.sp.s3
        Text {
            text: parent.title
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.xxl
            font.weight: Theme.fontWeight.semibold
            font.letterSpacing: Theme.trackTight.xxl
            color: Theme.fg0
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.line }
    }

    // Permission bit definitions mirrored from protocol/include/bsfchat/Permissions.h.
    // Each entry has {key, label, flag, hint} where flag is the bit value and
    // hint is the hover explanation.
    //
    // Every flag the SERVER actually enforces must appear here. A permission the
    // server checks but this list omits is invisible to an admin: they can see
    // people being refused and have no control that grants them access. The
    // converse is equally a bug — a switch for something nothing enforces is a
    // control that silently does nothing — so an entry is added here only once
    // there is a server-side check behind it.
    //
    // CHANGE_NICKNAME (0x0800) and MANAGE_NICKNAMES (0x1000) were absent for
    // exactly that reason until per-server nicknames existed. Both are now
    // enforced by ProfileHandler's nickname endpoint (self-rename and renaming
    // others respectively, evaluated at server scope), so both are listed.
    readonly property var permissionFlags: [
        {key: "view",     label: "View channels",      flag: 0x0001,
         hint: "See channels and read their history."},
        {key: "send",     label: "Send messages",      flag: 0x0002,
         hint: ""},
        {key: "attach",   label: "Attach files",       flag: 0x0004,
         hint: ""},
        {key: "embed",    label: "Embed links",        flag: 0x0008,
         hint: ""},
        // ADD_REACTIONS (0x4000). Out of numeric order on purpose: it belongs
        // beside "Send messages" because that is the pair a moderator reasons
        // about. Denying both is a mute; denying only send leaves a
        // read-mostly channel people can still react in. Before this flag the
        // server checked nothing on m.reaction, so denying send did not stop
        // anyone reacting.
        {key: "react",    label: "Add reactions",      flag: 0x4000,
         hint: "React to messages with emoji. Separate from sending, so a muted member can be stopped from reacting too."},
        {key: "manmsg",   label: "Manage messages",    flag: 0x0010,
         hint: "Delete anyone's message. Also bypasses slowmode."},
        // MANAGE_CHANNELS is the flag that lets a non-admin CREATE channels and
        // categories, which is not something "Manage channels" says out loud —
        // an owner looking for a "create channels" switch could not find one.
        {key: "manchan",  label: "Create & manage channels", flag: 0x0020,
         hint: "Create, rename, move, and delete channels and categories, and invite people to them."},
        {key: "manrole",  label: "Manage roles",       flag: 0x0040,
         hint: "Create and edit roles, assign them to members, and set per-channel overrides. Server-wide."},
        {key: "kick",     label: "Kick members",       flag: 0x0080,
         hint: ""},
        {key: "ban",      label: "Ban members",        flag: 0x0100,
         hint: ""},
        {key: "mentall",  label: "Mention @everyone",  flag: 0x0200,
         hint: "Use @everyone and @here to notify the whole channel."},
        {key: "manserv",  label: "Manage server",      flag: 0x0400,
         hint: "Change the server name and icon, and read the audit log."},
        // The two nickname flags are independent, not a hierarchy — the same as
        // Discord and as the server's checks. "Manage nicknames" lets you rename
        // other people and does NOT let you rename yourself, so a moderator role
        // that should be able to do both needs both switches on.
        {key: "changenick", label: "Change nickname",  flag: 0x0800,
         hint: "Set your own nickname on this server, without changing your account's name elsewhere."},
        {key: "mannick",  label: "Manage nicknames",   flag: 0x1000,
         hint: "Set and clear other members' nicknames. Cannot rename anyone whose role is equal or higher."},
        // MANAGE_BOTS (0x2000). Listed from the day it exists, because the
        // paragraph above is the whole reason: the bot endpoints are gated on
        // this flag server-side, and without a switch here the only account
        // that could ever create a bot would be an Administrator — a role
        // most servers hand out to nobody. That is not "a permission with no
        // UI", it is a feature with no way in.
        {key: "manbots",  label: "Manage bots",        flag: 0x2000,
         hint: "Create bot accounts, rotate their tokens, and deactivate them. Server-wide."},
        {key: "admin",    label: "Administrator",      flag: 0x8000,
         hint: "Grants every permission and bypasses all channel overrides. Give sparingly."}
    ]

    property string editingRoleId: ""
    property string editingMemberId: ""

    // ---- In-progress admin edit state (D-M4) -------------------------------
    //
    // These used to live on the Roles / Members ListView delegates
    // (roleEditCard.scratchColor et al., memberRow.assignedSet). `serverRoles`
    // and `serverMembers` are snapshot QVariantLists rebuilt on every sync, so
    // every sync pass replaced the model, destroyed the delegates and threw
    // away whatever the admin was halfway through typing — a permission grid
    // they had spent a minute on could vanish between two keystrokes. Holding
    // the state on the popup, keyed by the id being edited, makes it survive
    // any number of model refreshes; U-H2/U-H8 make those refreshes rarer but
    // cannot make them impossible, because a real change must still land.
    //
    // The id fields are what make this safe: if the row being edited is gone
    // (role deleted elsewhere, member left), the scratch simply no longer
    // matches anything and is seeded afresh on the next edit.
    // Role colours are SERVER data: they are written to the server and rendered
    // by every other member, whose theme and accent may be nothing like this
    // one. So these two are deliberately NOT theme tokens — binding them to
    // Theme.accent would persist one user's local accent as a shared value.
    // They are named here only so the hex stops being a magic number repeated
    // at the two places a role's colour can come into being.
    readonly property color defaultRoleColor: "#36d6c7"   // Designer cyan
    // Swatches offered in the role editor: the four Designer accents first,
    // then a wider gamut of classic chat role colours.
    readonly property var roleColorSwatches: [
        "#36d6c7", "#a28bff", "#ec6dd6", "#ffa34a",
        "#57f287", "#fee75c", "#ed4245", "#f47067",
        "#39c5cf", "#dcbdfb", "#f69d50", "#768390"
    ]

    property string roleScratchId: ""
    property string roleScratchName: ""
    property string roleScratchColor: ""
    property int roleScratchPos: 0
    property double roleScratchPerms: 0
    // `mentionable` is not a permission bit — it is a property of the ROLE, not
    // of who holds it — so it gets its own scratch value rather than a seat in
    // roleScratchPerms.
    property bool roleScratchMentionable: false
    // Nor is `self_assignable` a permission bit — it says who may PUT this
    // role on themselves, not what holding it lets them do — so it gets its
    // own scratch value too. It had no control at all until now: the save
    // below carried the flag through untouched (which is why the bot's roles
    // survived an admin renaming one) but nothing in the client could set it,
    // so the only way to publish an opt-in role was PATCH by hand.
    property bool roleScratchSelfAssignable: false

    property string memberScratchId: ""
    // Set of assigned role ids: { roleId: true }. Written by replacement
    // rather than mutation so QML notices the change.
    property var memberScratchRoles: ({})

    // A role's permission bitfield arrives as a hex string ("0x1f") from the
    // server and as a number from our own optimistic writes. Normalise.
    function _permsToNumber(p) {
        if (typeof p === "string") {
            var str = p;
            if (str.indexOf("0x") === 0 || str.indexOf("0X") === 0) str = str.substr(2);
            return parseInt(str, 16) || 0;
        }
        return p || 0;
    }

    // Open the inline editor for `role` and seed the scratch from it. Seeding
    // happens ONCE, here, rather than through a binding on the model row —
    // a binding would re-seed (and so discard the edit) on every sync.
    function beginRoleEdit(role) {
        var rid = (role && (role.id || role.name)) || "";
        if (!rid) return;
        editingRoleId = rid;
        roleScratchId = rid;
        roleScratchName = (role && role.name) || "";
        roleScratchColor = (role && role.color) || defaultRoleColor;
        roleScratchPos = (role && role.position !== undefined) ? role.position : 0;
        roleScratchPerms = _permsToNumber(role && role.permissions);
        roleScratchMentionable = !!(role && role.mentionable);
        roleScratchSelfAssignable = !!(role && role.self_assignable);
    }

    function endRoleEdit() {
        editingRoleId = "";
        roleScratchId = "";
        // Cleared with the rest of the scratch state, so opening a second role
        // does not inherit the first one's checkbox before beginRoleEdit runs.
        roleScratchMentionable = false;
        roleScratchSelfAssignable = false;
    }

    function toggleRoleScratchPerm(flag) {
        roleScratchPerms = (Number(roleScratchPerms) ^ flag);
    }

    function roleScratchHasPerm(flag) {
        return (Number(roleScratchPerms) & flag) !== 0;
    }

    // Same contract for the member role-assignment card.
    function beginMemberEdit(userId) {
        if (!userId) return;
        editingMemberId = userId;
        memberScratchId = userId;
        var m = {};
        if (serverManager.activeServer) {
            var list = serverManager.activeServer.memberRoles(userId);
            for (var i = 0; i < list.length; i++) m[list[i]] = true;
        }
        memberScratchRoles = m;
    }

    function endMemberEdit() {
        editingMemberId = "";
        memberScratchId = "";
        memberScratchRoles = ({});
    }

    function toggleMemberScratchRole(roleId) {
        if (!roleId) return;
        var m = {};
        for (var k in memberScratchRoles) m[k] = memberScratchRoles[k];
        if (m[roleId]) delete m[roleId];
        else m[roleId] = true;
        memberScratchRoles = m;
    }

    function memberScratchHasRole(roleId) {
        return memberScratchRoles[roleId] === true;
    }

    // Swap the role at `index` with its neighbour in the given
    // direction (-1 = up, +1 = down). We swap BOTH array order and
    // position fields: the UI renders in array order (the ListView
    // iterates as the server serialises), and the server's permission
    // engine sorts by position — so if we only swapped positions the
    // UI would look unchanged until the next reload. Keeping both in
    // sync means the rebuilt view matches what we just asked for.
    function moveRole(index, direction) {
        if (!serverManager.activeServer) return;
        var roles = serverManager.activeServer.serverRoles;
        if (!roles || roles.length === 0) return;
        var other = index + direction;
        if (other < 0 || other >= roles.length) return;

        // Deep copy so we don't mutate the live Q_PROPERTY payload.
        var out = [];
        for (var i = 0; i < roles.length; i++) {
            var r = {};
            for (var k in roles[i]) r[k] = roles[i][k];
            out.push(r);
        }
        // Swap positions first so that sorting by position downstream
        // still lines up with our array order.
        var p1 = out[index].position;
        var p2 = out[other].position;
        if (p1 === p2) p2 = p1 + direction;
        out[index].position = p2;
        out[other].position = p1;
        // Then swap the array slots so the ListView reflects the
        // change immediately (sync echo will confirm the same shape).
        var tmp = out[index];
        out[index] = out[other];
        out[other] = tmp;
        serverManager.activeServer.updateServerRoles(out);
    }

    // Transient state for the kick/ban/unban confirm dialog. Populated by
    // the row that initiated the action; consumed on confirm.
    //   { kind: "kick" | "ban" | "unban", userId, displayName }
    property var confirmMod: ({ kind: "", userId: "", displayName: "" })
    property alias confirmModDialog: _confirmModDialog

    // Inline error toast state. Populated by the stateWriteFailed handler
    // on the active server; auto-clears after 5 seconds.
    property string _toastMessage: ""
    Timer {
        id: _toastTimer
        interval: 5000
        onTriggered: serverSettingsPopup._toastMessage = ""
    }
    Connections {
        target: serverManager.activeServer
        function onStateWriteFailed(kind, status, error) {
            var prefix;
            switch (kind) {
                case "role-assign":
                    prefix = "Couldn't save role assignments";
                    break;
                case "server-name":
                    prefix = "Couldn't update server name";
                    break;
                case "channel-override":
                    prefix = "Couldn't save channel permissions";
                    break;
                case "channel-settings":
                    prefix = "Couldn't update channel settings";
                    break;
                case "server-roles":
                    prefix = "Couldn't save server roles";
                    break;
                default:
                    prefix = "Save failed";
            }
            var suffix = status === 403
                ? "you don't have permission."
                : (error || ("HTTP " + status));
            serverSettingsPopup._toastMessage = prefix + " — " + suffix;
            _toastTimer.restart();
        }
    }

    background: Rectangle {
        color: Theme.bg1
        radius: Theme.r3
        border.color: Theme.line
        border.width: 1

        // Inline error toast — slides in from the top when the active
        // server's stateWriteFailed signal fires, auto-dismisses after 5s.
        // Anchored inside the popup's background so it floats above content
        // at any tab.
        Rectangle {
            id: _errorToast
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.topMargin: serverSettingsPopup._toastMessage !== ""
                               ? Theme.sp.s5 : -_errorToast.height - Theme.sp.s5
            width: Math.min(parent.width - Theme.sp.s7 * 2, 520)
            height: _errorToastCol.implicitHeight + Theme.sp.s4 * 2
            radius: Theme.r2
            color: Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.14)
            border.color: Theme.danger
            border.width: 1
            z: 20
            visible: anchors.topMargin > -_errorToast.height
            Behavior on anchors.topMargin {
                NumberAnimation { duration: 220; easing.type: Easing.OutCubic }
            }

            RowLayout {
                id: _errorToastCol
                anchors.fill: parent
                anchors.leftMargin: Theme.sp.s5
                anchors.rightMargin: Theme.sp.s4
                anchors.topMargin: Theme.sp.s4
                anchors.bottomMargin: Theme.sp.s4
                spacing: Theme.sp.s3
                Icon {
                    name: "x"
                    size: 14
                    color: Theme.danger
                    Layout.alignment: Qt.AlignTop
                    Layout.topMargin: 2
                }
                Text {
                    Layout.fillWidth: true
                    text: serverSettingsPopup._toastMessage
                    color: Theme.fg0
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.sm
                    wrapMode: Text.WordWrap
                }
                Rectangle {
                    Layout.preferredWidth: 20
                    Layout.preferredHeight: 20
                    Layout.alignment: Qt.AlignTop
                    radius: Theme.r1
                    color: _toastDismissMouse.containsMouse
                           ? Qt.rgba(0, 0, 0, 0.15) : "transparent"
                    Text {
                        anchors.centerIn: parent
                        text: "×"
                        color: Theme.fg1
                        font.pixelSize: 14
                    }
                    MouseArea {
                        id: _toastDismissMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: serverSettingsPopup._toastMessage = ""
                    }
                }
            }
        }

        // Top-right close X — floats on top of whatever the content is,
        // matches the ChannelSettings pattern. Esc / click-outside still
        // work the same way.
        Rectangle {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: Theme.sp.s5
            anchors.rightMargin: Theme.sp.s5
            // 44 pt on a phone. This pane takes the whole screen there,
            // Esc does not exist and click-outside has nowhere to land,
            // so this X is the ONLY way back out — at 28 it was well
            // under the touch minimum.
            width: Theme.isMobile ? 44 : 28
            height: Theme.isMobile ? 44 : 28
            radius: Theme.r1
            color: closeXMouse.containsMouse ? Theme.bg3 : "transparent"
            z: 10
            Icon {
                anchors.centerIn: parent
                name: "x"
                size: 14
                color: closeXMouse.containsMouse ? Theme.fg0 : Theme.fg2
            }
            MouseArea {
                id: closeXMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: serverSettingsPopup.close()
            }
        }
    }

    // GridLayout, not RowLayout, so this is a row on a desktop and a stack
    // on a phone without the tree being written twice. Exactly two of the
    // children below are ever visible at once — the locked panel replaces
    // the nav AND the pages — and a layout skips invisible children, so
    // `columns: 2` still pairs them the way it always did.
    contentItem: GridLayout {
        columns: Theme.isMobile ? 1 : 2
        rowSpacing: 0
        columnSpacing: 0

        // ── Locked ──────────────────────────────────────────────────
        //
        // Replaces the nav AND the pages rather than sitting on top of them:
        // an admin surface a member cannot use is clutter at best, and at worst
        // it is a roster of roles and bans rendered for somebody the server
        // would refuse. One sentence, the permissions by name, and — the part
        // the incident turned on — which account is asking.
        //
        // Same shape as BotManagerPane's "You need the \"Manage bots\"
        // permission…" state, which is the in-repo precedent for this: name
        // the permission, say who can grant it, and do not pretend the feature
        // is absent.
        ColumnLayout {
            visible: !serverSettingsPopup._mayOpen
            Layout.fillWidth: true
            Layout.fillHeight: true
            // 32 pt of gutter each side is a desktop dialog margin; on a
            // phone it is a sixth of the screen. See Theme.mobileGutter.
            Layout.margins: Theme.isMobile ? Theme.mobileGutter : Theme.sp.s7 * 2
            spacing: Theme.sp.s5

            TabHeader { title: "Server Settings"; Layout.fillWidth: true }

            InfoBanner {
                icon: "lock"
                tint: Theme.warn
                // Names the permissions rather than saying "you don't have
                // permission", for the reason BotManagerPane's banner gives:
                // the name is the whole value — it is what the person asks an
                // administrator for. No "see the Roles tab" here, though;
                // whoever is reading this cannot open it.
                text: "None of the pages in here are available to this account. "
                    + "They need one of: Manage server, Manage roles, "
                    + "Create & manage channels, Kick members, Ban members, or "
                    + "Manage bots. Someone who already has Manage roles can put "
                    + "one of them on a role this account holds."
            }

            // The identity block. Deliberately NOT just the display name: the
            // 2026-09-20 incident was two accounts of the same person on the
            // same homeserver, and display names are exactly the thing that
            // cannot tell those apart. The mxid is the answer, so the mxid is
            // what is shown, in full, selectable, and next to the sentence
            // explaining why it matters.
            Text {
                text: "SIGNED IN AS"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWidest.xs
                color: Theme.fg3
            }
            // The name, and never a second copy of the id underneath: an
            // account with no display name at all renders its localpart here
            // (ServerConnection::displayName), which is also what the member
            // list prints for it. Two identical lines say nothing twice.
            Text {
                Layout.fillWidth: true
                text: serverManager.activeServer
                      ? serverManager.activeServer.displayName : ""
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.lg
                font.weight: Theme.fontWeight.semibold
                color: Theme.fg0
                elide: Text.ElideRight
            }
            // Read-only TextEdit rather than Text so the id can be selected and
            // copied into a support message — which is what the person who hit
            // this will be doing next. Same read-only-TextEdit idiom the bot
            // token banner uses, and for the same reason: no editable buffer,
            // but a value a human can get out of the window.
            TextEdit {
                Layout.fillWidth: true
                text: serverManager.activeServer
                      ? serverManager.activeServer.userId : ""
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSize.md
                color: Theme.fg1
            }
            Text {
                Layout.fillWidth: true
                text: "If you have more than one account on this server, the "
                    + "permissions belong to the account above — not to you as "
                    + "a person. Signing in as the other one is often the whole "
                    + "answer."
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
                wrapMode: Text.WordWrap
            }

            Item { Layout.fillHeight: true }
        }

        // Left nav sidebar.
        //
        // Gone on a phone, where 200 pt of fixed rail out of a ~360 pt
        // viewport is more than half the screen spent on navigation. The
        // phone gets the dropdown below instead.
        Rectangle {
            visible: serverSettingsPopup._mayOpen && !Theme.isMobile
            Layout.fillHeight: true
            // Unconditional — see the identical note in ClientSettings.qml.
            // A layout never reads an invisible child's size hints, so the
            // mobile branch this used to carry did nothing except look like
            // a 0 pt touch target to the hygiene scan.
            Layout.preferredWidth: 200
            color: Theme.bg0
            radius: Theme.r2

            // Clip right radius
            Rectangle {
                anchors.right: parent.right
                width: Theme.r2
                height: parent.height
                color: Theme.bg0
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.sp.s3
                spacing: 2

                Text {
                    text: "SERVER SETTINGS"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xs
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackWidest.xs
                    color: Theme.fg3
                    Layout.leftMargin: Theme.sp.s3
                    Layout.topMargin: Theme.sp.s3
                    Layout.bottomMargin: Theme.sp.s3
                }

                Repeater {
                    // Order and rationale live on the `sections` property.
                    model: serverSettingsPopup.sections
                    delegate: Rectangle {
                        Layout.fillWidth: true
                        height: 36
                        radius: Theme.r1
                        readonly property bool isActive: selectedSection === index
                        color: isActive ? Theme.bg3
                             : navItemMouse.containsMouse ? Theme.bg2
                             : "transparent"
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: Theme.sp.s3
                            text: modelData
                            color: parent.isActive ? Theme.fg0 : Theme.fg1
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.md
                            font.weight: parent.isActive
                                         ? Theme.fontWeight.semibold
                                         : Theme.fontWeight.medium
                        }

                        MouseArea {
                            id: navItemMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: selectedSection = index
                        }
                    }
                }

                // Close-nav row removed; the dialog is dismissed via the
                // top-right X (see below) or Esc / click-outside.
                Item { Layout.fillHeight: true }
            }
        }

        // Phone section picker — the replacement for the nav rail, and the
        // reason the container above is a GridLayout. On a phone the rail is
        // `visible: false`, a layout skips invisible children, and
        // `columns: 1` stacks what is left: picker on top, pages below. On a
        // desktop this one is the invisible child instead.
        ThemedComboBox {
            id: mobileSectionPicker
            visible: Theme.isMobile && serverSettingsPopup._mayOpen
            Layout.fillWidth: true
            Layout.margins: Theme.mobileGutter
            Layout.bottomMargin: 0
            // The pane's close X floats over the top-right corner of the
            // background at z:10 and does not participate in this layout, so
            // the picker has to step around it by hand: 12 (its margin) + 44
            // (its size) + a gap.
            Layout.rightMargin: Theme.sp.s5 + 44 + Theme.sp.s3
            // 44 pt: Apple HIG / Material touch minimum. Every other control
            // in this pane is reached through this one.
            implicitHeight: 44
            model: serverSettingsPopup.sections
            currentIndex: serverSettingsPopup.selectedSection
            // Re-established with Qt.binding, not left as the plain value
            // ComboBox just wrote (D-C1 — the rule this file states for
            // every control that is both bound and written imperatively).
            //
            // Selecting a row makes ComboBox assign `currentIndex` itself,
            // which DESTROYS the binding above. `onAboutToShow` sets
            // selectedSection back to 0 on every open, so the very next time
            // the dialog is opened the pages go to Overview and this field
            // does not.
            // From then on this field is a one-shot value: it shows whatever
            // was last picked while the pages behind it show something else,
            // and the only way back is to pick a different section and
            // return. Read off `currentIndex` rather than the injected
            // signal argument, matching the other combos in the tree.
            onActivated: {
                serverSettingsPopup.selectedSection = currentIndex;
                currentIndex = Qt.binding(function() {
                    return serverSettingsPopup.selectedSection;
                });
            }
        }

        // Content area
        StackLayout {
            visible: serverSettingsPopup._mayOpen
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: selectedSection

            // ---- Overview (index 0) ----
            Item {
                // D-H4. This page is taller than the popup at the default
                // window size — server icon, name field, the five screen-share
                // cap rows and the banner — so with a bare ColumnLayout the
                // bottom of it simply painted past the popup's edge with no way
                // to reach it. Same Flickable wrapper as the Screen Share page.
                Flickable {
                    id: overviewFlick
                    anchors.fill: parent
                    // 32 pt of gutter each side is a desktop dialog margin; on a
                    // phone it is a sixth of the screen. See Theme.mobileGutter.
                    anchors.margins: Theme.isMobile ? Theme.mobileGutter : Theme.sp.s7 * 2
                    contentHeight: overviewPane.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ThemedScrollBar { id: overviewScrollBar }

                ColumnLayout {
                    // id needed so the PolicyRow inline component can be
                    // handed policyEditorEnabled — inline components
                    // don't capture the enclosing item's scope.
                    id: overviewPane
                    // Bind to the Flickable itself, NOT parent (the
                    // contentItem): with contentWidth unset the contentItem's
                    // width follows its children, so parent.width is circular
                    // and the column blows out past the right edge.
                    width: overviewFlick.width - overviewScrollBar.reservedWidth
                    spacing: Theme.sp.s7

                    TabHeader { title: "Server Overview" }

                    // Server icon — 80×80 preview on the left, Upload /
                    // Remove actions on the right. Icon is a resolved
                    // http URL (mxc → media endpoint). Fallback is the
                    // server's initial letter in an accent tile.
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.maximumWidth: 420
                        spacing: Theme.sp.s5

                        Rectangle {
                            Layout.preferredWidth: 80
                            Layout.preferredHeight: 80
                            radius: Theme.r3
                            color: Theme.bg2
                            border.color: Theme.line
                            border.width: 1
                            clip: true

                            Text {
                                anchors.centerIn: parent
                                visible: serverIconPreview.status !== Image.Ready
                                text: {
                                    var n = serverManager.activeServer
                                        ? serverManager.activeServer.serverName : "?";
                                    var stripped = (n || "?").replace(/^[^a-zA-Z0-9]+/, "");
                                    return (stripped.charAt(0) || "?").toUpperCase();
                                }
                                font.family: Theme.fontSans
                                font.pixelSize: 32
                                font.weight: Theme.fontWeight.semibold
                                color: Theme.fg1
                            }

                            Image {
                                id: serverIconPreview
                                anchors.fill: parent
                                anchors.margins: 1
                                source: serverManager.activeServer
                                    ? serverManager.activeServer.serverAvatarUrl : ""
                                visible: status === Image.Ready
                                fillMode: Image.PreserveAspectCrop
                                smooth: true
                                asynchronous: true
                                cache: true
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.sp.s2

                            Text {
                                text: "SERVER ICON"
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.xs
                                font.weight: Theme.fontWeight.semibold
                                font.letterSpacing: Theme.trackWidest.xs
                                color: Theme.fg3
                            }

                            Text {
                                text: "Square image, at least 128×128. PNG, JPEG, GIF or WebP."
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.sm
                                color: Theme.fg2
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            RowLayout {
                                spacing: Theme.sp.s3
                                Layout.topMargin: Theme.sp.s2

                                Button {
                                    id: uploadIconBtn
                                    text: "Upload icon…"
                                    contentItem: Text {
                                        text: uploadIconBtn.text
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.sm
                                        font.weight: Theme.fontWeight.medium
                                        color: Theme.fg0
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    background: Rectangle {
                                        color: uploadIconBtn.hovered ? Theme.bg3 : Theme.bg2
                                        border.color: Theme.line
                                        border.width: 1
                                        radius: Theme.r2
                                        implicitWidth: 120
                                        implicitHeight: Theme.controlHeight.sm
                                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                    }
                                    onClicked: serverIconFileDialog.open()
                                }
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.maximumWidth: 420
                        Layout.preferredHeight: 1
                        color: Theme.lineSoft
                    }

                    Text {
                        text: "SERVER NAME"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                        font.weight: Theme.fontWeight.semibold
                        font.letterSpacing: Theme.trackWidest.xs
                        color: Theme.fg3
                    }

                    TextField {
                        id: serverNameField
                        Layout.fillWidth: true
                        Layout.maximumWidth: 420
                        // Shown to all users across the server (channel-list
                        // header, tooltips, etc.). Only editable by users
                        // with MANAGE_SERVER; server enforces regardless.
                        text: serverManager.activeServer ? serverManager.activeServer.serverName : ""
                        color: Theme.fg0
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        background: Rectangle {
                            color: Theme.bg0
                            radius: Theme.r2
                            border.color: serverNameField.activeFocus ? Theme.accent : Theme.line
                            border.width: 1
                        }
                        leftPadding: Theme.sp.s4
                        rightPadding: Theme.sp.s4
                        topPadding: Theme.sp.s3
                        bottomPadding: Theme.sp.s3
                    }

                    // Primary action row — "Save" as a proper primary
                    // button, sized so it commands attention without
                    // dominating. Disabled until the field value differs
                    // from the currently persisted server name so the
                    // button reads the current state at a glance.
                    RowLayout {
                        Layout.topMargin: Theme.sp.s3
                        Layout.maximumWidth: 420

                        Button {
                            id: saveServerNameBtn
                            enabled: serverManager.activeServer
                                     && serverNameField.text.trim().length > 0
                                     && serverNameField.text.trim() !== serverManager.activeServer.serverName
                            contentItem: Text {
                                text: "Save changes"
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.md
                                font.weight: Theme.fontWeight.semibold
                                color: saveServerNameBtn.enabled ? Theme.onAccent : Theme.fg3
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                color: !saveServerNameBtn.enabled
                                       ? Theme.bg2
                                       : (saveServerNameBtn.hovered ? Theme.accentDim : Theme.accent)
                                radius: Theme.r2
                                implicitWidth: 140
                                implicitHeight: Theme.controlHeight.md
                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                            }
                            onClicked: {
                                if (serverManager.activeServer) {
                                    serverManager.activeServer.updateServerName(serverNameField.text.trim());
                                }
                            }
                        }
                    }

                    InfoBanner {
                        Layout.maximumWidth: 420
                        icon: "shield"
                        text: "Only members with the Manage Server permission can change the server name. Everyone else sees the field read-only."
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.maximumWidth: 420
                        Layout.topMargin: Theme.sp.s5
                        Layout.preferredHeight: 1
                        color: Theme.lineSoft
                    }

                    Text {
                        text: "SCREEN-SHARE MAXIMUM QUALITY"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                        font.weight: Theme.fontWeight.semibold
                        font.letterSpacing: Theme.trackWidest.xs
                        color: Theme.fg3
                    }

                    Text {
                        Layout.maximumWidth: 420
                        text: "Cap the highest quality preset users may pick when "
                            + "sharing their screen in voice channels. Lower caps "
                            + "protect bandwidth on busy servers; Ultra is uncapped."
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                        color: Theme.fg2
                        wrapMode: Text.WordWrap
                    }

                    // Per-axis caps. Each row is a slider + a numeric
                    // input + a "no cap" toggle (-1 sentinel) that
                    // tells clients to honour their user prefs
                    // verbatim. Edits are batched into a single
                    // setScreenSharePolicy() call on commit so we
                    // emit one room-state event per change.
                    component PolicyRow: RowLayout {
                        id: pr
                        property string label: ""
                        property int minVal: 0
                        property int maxVal: 60
                        property int stepVal: 1
                        property string suffix: ""
                        property int value: -1
                        property bool editable: false
                        signal commit(int v)

                        spacing: Theme.sp.s3
                        Text {
                            Layout.preferredWidth: 130
                            text: pr.label
                            color: Theme.fg1
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.sm
                        }
                        ThemedSlider {
                            implicitWidth: 220
                            from: pr.minVal; to: pr.maxVal; stepSize: pr.stepVal
                            enabled: pr.value >= 0 && pr.editable
                            value: pr.value < 0 ? pr.minVal : pr.value
                            onMoved: pr.commit(Math.round(value))
                        }
                        Text {
                            Layout.preferredWidth: 80
                            text: pr.value < 0 ? "no cap" : pr.value + pr.suffix
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fontSize.sm
                            color: pr.value < 0 ? Theme.fg3 : Theme.fg0
                            horizontalAlignment: Text.AlignRight
                        }
                        ThemedSwitch {
                            text: "Cap"
                            enabled: pr.editable
                            checked: pr.value >= 0
                            onToggled: {
                                if (checked) pr.commit(pr.maxVal);
                                else         pr.commit(-1);
                            }
                        }
                    }

                    // D-M7. These caps are SERVER-wide, so with no channel
                    // selected the question has to be asked at server scope
                    // rather than answered "no": an admin who opens Settings
                    // straight from the server rail, before clicking into any
                    // channel, otherwise finds every control here greyed out
                    // with nothing on screen explaining why.
                    // ServerConnection::myPermissions treats an empty room id
                    // as exactly that question — evaluated without channel
                    // overrides, matching what the server does.
                    readonly property bool policyEditorEnabled: {
                        var s = serverManager.activeServer;
                        if (!s) return false;
                        if (s.permissionsGeneration < 0) return false;
                        if (!s.activeRoomId) return s.canManageChannel("");
                        return s.canManageChannel(s.activeRoomId);
                    }

                    // D-M2. The rows used to write `value` imperatively in
                    // onCommit, and again from a remote-change handler, which
                    // destroyed the binding on the server property each time.
                    // After that a row showed whatever was last written to it,
                    // so switching servers left the PREVIOUS server's caps on
                    // screen — and a commit then sent them.
                    //
                    // Nothing is written imperatively now. setScreenSharePolicy
                    // updates the cached policy optimistically and emits
                    // maxScreenSharePolicyChanged, which is the NOTIFY signal
                    // for all five properties, so the rows repaint from their
                    // own bindings: immediately on commit, and again when the
                    // sync echo confirms or reverts it. `axis`/`v` carry the
                    // value being changed because the row's own property has
                    // deliberately not been written at commit time.
                    function _commitPolicy(axis, v) {
                        var s = serverManager.activeServer;
                        if (!s) return;
                        s.setScreenSharePolicy(
                            axis === "fps"      ? v : s.maxScreenShareFps,
                            axis === "width"    ? v : s.maxScreenShareWidth,
                            axis === "jpeg"     ? v : s.maxScreenShareJpeg,
                            axis === "bitrate"  ? v : s.maxScreenShareBitrate,
                            axis === "lossless" ? v : s.allowLossless);
                    }

                    PolicyRow {
                        id: fpsPolicyRow
                        editable: overviewPane.policyEditorEnabled
                        label: "Max frame rate"
                        minVal: 1; maxVal: 60; stepVal: 1; suffix: " fps"
                        value: serverManager.activeServer
                            ? serverManager.activeServer.maxScreenShareFps : -1
                        onCommit: (v) => overviewPane._commitPolicy("fps", v)
                    }
                    PolicyRow {
                        id: widthPolicyRow
                        editable: overviewPane.policyEditorEnabled
                        label: "Max long edge"
                        minVal: 480; maxVal: 3840; stepVal: 80; suffix: " px"
                        value: serverManager.activeServer
                            ? serverManager.activeServer.maxScreenShareWidth : -1
                        onCommit: (v) => overviewPane._commitPolicy("width", v)
                    }
                    PolicyRow {
                        id: jpegPolicyRow
                        editable: overviewPane.policyEditorEnabled
                        label: "Max JPEG quality"
                        minVal: 1; maxVal: 100; stepVal: 1; suffix: ""
                        value: serverManager.activeServer
                            ? serverManager.activeServer.maxScreenShareJpeg : -1
                        onCommit: (v) => overviewPane._commitPolicy("jpeg", v)
                    }
                    PolicyRow {
                        id: bitratePolicyRow
                        editable: overviewPane.policyEditorEnabled
                        label: "Max video bitrate"
                        minVal: 250; maxVal: 50000; stepVal: 250; suffix: " kbps"
                        value: serverManager.activeServer
                            ? serverManager.activeServer.maxScreenShareBitrate : -1
                        onCommit: (v) => overviewPane._commitPolicy("bitrate", v)
                    }
                    RowLayout {
                        spacing: Theme.sp.s3
                        Text {
                            Layout.preferredWidth: 130
                            text: "Allow lossless"
                            color: Theme.fg1
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.sm
                        }
                        ThemedSwitch {
                            id: losslessSwitch
                            enabled: overviewPane.policyEditorEnabled
                            checked: serverManager.activeServer
                                ? serverManager.activeServer.allowLossless : true
                            // A user toggle writes `checked` itself, replacing
                            // the binding above — so it has to be put back, or
                            // this switch stops tracking the server exactly the
                            // way the numeric rows used to.
                            onToggled: {
                                overviewPane._commitPolicy("lossless", checked);
                                checked = Qt.binding(function() {
                                    return serverManager.activeServer
                                        ? serverManager.activeServer.allowLossless : true;
                                });
                            }
                        }
                        Text {
                            text: "AV1 mathematically-lossless mode (very high "
                                + "bandwidth — LAN-class links)"
                            color: Theme.fg3
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.xs
                        }
                    }
                    // No Connections handler here any more: an admin on
                    // another device changing the policy emits
                    // maxScreenSharePolicyChanged, which is the NOTIFY signal
                    // the five bindings above already depend on. The handler
                    // that used to copy the values across by hand was the
                    // thing breaking them.

                    InfoBanner {
                        Layout.maximumWidth: 540
                        icon: "shield"
                        text: "Only Manage-Server members can change these caps. "
                            + "Each axis caps independently — leave any of them on "
                            + "\"no cap\" to honour the user's setting on that axis. "
                            + "Effective stream params are min(user, cap)."
                    }
                    // No trailing fillHeight spacer: inside a Flickable the
                    // column is sized by its content, and a greedy spacer would
                    // either do nothing or fight contentHeight.
                }
                }
            }

            // ---- Roles (index 1) ----
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    // 32 pt of gutter each side is a desktop dialog margin; on a
                    // phone it is a sixth of the screen. See Theme.mobileGutter.
                    anchors.margins: Theme.isMobile ? Theme.mobileGutter : Theme.sp.s7 * 2
                    spacing: Theme.sp.s7

                    TabHeader { title: "Roles" }

                    Text {
                        Layout.fillWidth: true
                        text: "Roles grant permissions server-wide. Assign them to members in the Members tab; override per-channel in each channel's settings."
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                        color: Theme.fg2
                        wrapMode: Text.WordWrap
                    }

                    // Roles list
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true

                        ScrollBar.vertical: ThemedScrollBar { id: rolesScrollBar }
                        model: serverManager.activeServer ? serverManager.activeServer.serverRoles : []
                        spacing: 4

                        delegate: Column {
                            id: roleDelegate
                            width: (ListView.view ? ListView.view.width : 400)
                                   - rolesScrollBar.reservedWidth
                            spacing: 2

                            property var role: modelData
                            property int roleIndex: index
                            property bool isEditing: serverSettingsPopup.editingRoleId === (role.id || role.name)
                            readonly property bool canMoveUp:
                                roleIndex > 0
                            readonly property bool canMoveDown: {
                                if (!serverManager.activeServer) return false;
                                return roleIndex < serverManager.activeServer.serverRoles.length - 1;
                            }

                            Rectangle {
                                width: parent.width
                                height: 48
                                radius: Theme.r2
                                color: roleRowMouse.containsMouse ? Theme.bg3 : Theme.bg2
                                border.color: roleDelegate.isEditing ? Theme.accent : Theme.line
                                border.width: 1
                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                Behavior on border.color { ColorAnimation { duration: Theme.motion.fastMs } }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: Theme.sp.s5
                                    anchors.rightMargin: Theme.sp.s3
                                    spacing: Theme.sp.s4

                                    // Role color chip — slightly larger so
                                    // it reads as a real identifier.
                                    Rectangle {
                                        width: 18; height: 18; radius: 9
                                        color: roleDelegate.role.color || Theme.accent
                                        border.color: Theme.bg0
                                        border.width: 1
                                    }
                                    Text {
                                        text: roleDelegate.role.name || ""
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.md
                                        font.weight: Theme.fontWeight.semibold
                                        color: Theme.fg0
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                    }

                                    // Reorder chevrons — reveal-on-hover so
                                    // the resting list stays quiet. Click
                                    // swaps the role with its neighbour's
                                    // position and writes the new ordering
                                    // back. Disabled at the ends of the
                                    // list so `position`s stay unique.
                                    component ReorderBtn: Rectangle {
                                        id: rbtn
                                        property string iconName: ""
                                        property bool disabled: false
                                        signal clicked()
                                        Layout.preferredWidth: 22
                                        Layout.preferredHeight: 22
                                        radius: Theme.r1
                                        color: rbtnMouse.containsMouse && !disabled
                                            ? Theme.bg2 : "transparent"
                                        opacity: disabled
                                            ? 0.25
                                            : (roleRowMouse.containsMouse
                                               || rbtnMouse.containsMouse ? 1.0 : 0.0)
                                        Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }
                                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                        Icon {
                                            anchors.centerIn: parent
                                            name: rbtn.iconName
                                            size: 12
                                            color: rbtnMouse.containsMouse && !rbtn.disabled
                                                ? Theme.fg0 : Theme.fg2
                                        }
                                        MouseArea {
                                            id: rbtnMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            enabled: !rbtn.disabled
                                            cursorShape: rbtn.disabled
                                                ? Qt.ArrowCursor : Qt.PointingHandCursor
                                            onClicked: rbtn.clicked()
                                        }
                                    }

                                    ReorderBtn {
                                        iconName: "chevron-right"
                                        rotation: -90          // point up
                                        disabled: !roleDelegate.canMoveUp
                                        onClicked: serverSettingsPopup.moveRole(
                                            roleDelegate.roleIndex, -1)
                                    }
                                    ReorderBtn {
                                        iconName: "chevron-right"
                                        rotation: 90           // point down
                                        disabled: !roleDelegate.canMoveDown
                                        onClicked: serverSettingsPopup.moveRole(
                                            roleDelegate.roleIndex, 1)
                                    }

                                    // Expand / collapse chevron — rotates
                                    // 90° rather than swapping glyphs.
                                    Icon {
                                        name: "chevron-right"
                                        size: 14
                                        color: Theme.fg2
                                        rotation: roleDelegate.isEditing ? 90 : 0
                                        Behavior on rotation {
                                            NumberAnimation { duration: Theme.motion.fastMs
                                                              easing.type: Easing.BezierSpline
                                                              easing.bezierCurve: Theme.motion.bezier }
                                        }
                                    }
                                }

                                MouseArea {
                                    id: roleRowMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    // Sit BEHIND the chevron buttons in
                                    // z-order so their MouseAreas get
                                    // clicks first and don't propagate
                                    // back to the row's expand/collapse
                                    // handler. In QML later-declared
                                    // items stack on top; we declared
                                    // the RowLayout first, so without
                                    // a negative z this MouseArea
                                    // overlays the chevrons and steals
                                    // their clicks.
                                    z: -1
                                    onClicked: {
                                        if (roleDelegate.isEditing) {
                                            serverSettingsPopup.endRoleEdit();
                                        } else {
                                            // Seeds the scratch from this role
                                            // exactly once — see beginRoleEdit.
                                            serverSettingsPopup.beginRoleEdit(roleDelegate.role);
                                        }
                                    }
                                }
                            }

                            // Inline editor
                            Rectangle {
                                id: roleEditCard
                                width: parent.width
                                visible: parent.isEditing
                                radius: Theme.r2
                                color: Theme.bg0
                                border.color: Theme.line
                                border.width: 1
                                height: visible ? roleEditCol.implicitHeight + Theme.sp.s7 * 2 : 0

                                // Single source of truth for the form.
                                // editRole binds to the delegate's role, so
                                // scratch values reset whenever the user opens
                                // a different role. Falls back to an empty
                                // object so the descendant bindings don't
                                // explode into "Cannot read property X of
                                // undefined" during the brief window between
                                // the surrounding Column being recycled and
                                // the new role reference landing (e.g. after
                                // a role is deleted).
                                // Directly reference the enclosing delegate's
                                // role. The old `parent.parent.role` walked
                                // up past the Column into the ListView's
                                // contentItem (which has no `role` property)
                                // so editRole was always `({})` — harmless-
                                // looking because the descendant bindings
                                // all short-circuit to defaults, but it
                                // meant the Save button's `editRoleName`
                                // (which depends on `editRole.name`) had
                                // nothing to fall back to and saves looked
                                // like they silently cleared the name.
                                readonly property var editRole: roleDelegate.role || ({})
                                // The edit-in-progress values live on the popup
                                // (serverSettingsPopup.roleScratch*), not here:
                                // this card is a ListView delegate and is
                                // destroyed whenever a sync republishes
                                // serverRoles. See D-M4 at the top of the file.

                                ColumnLayout {
                                    id: roleEditCol
                                    anchors.fill: parent
                                    anchors.margins: Theme.sp.s7
                                    spacing: Theme.sp.s3

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.sp.s3

                                        TextField {
                                            id: editRoleName
                                            Layout.fillWidth: true
                                            // Bound to the popup-level scratch, and
                                            // written back on edit rather than left
                                            // to accumulate in the field: a TextField
                                            // that owns its own text loses it with
                                            // the delegate.
                                            text: serverSettingsPopup.roleScratchName
                                            onTextEdited: serverSettingsPopup.roleScratchName = text
                                            color: Theme.fg0
                                            font.family: Theme.fontSans
                                            font.pixelSize: Theme.fontSize.md
                                            background: Rectangle {
                                                color: Theme.bg1
                                                radius: Theme.r2
                                                border.color: editRoleName.activeFocus ? Theme.accent : Theme.line
                                                border.width: 1
                                            }
                                            leftPadding: Theme.sp.s4
                                            rightPadding: Theme.sp.s4
                                            topPadding: Theme.sp.s3
                                            bottomPadding: Theme.sp.s3
                                        }

                                        // SpinBox keeps Qt Controls chrome for its
                                        // up/down buttons but we swap in a themed
                                        // background and centre the value.
                                        SpinBox {
                                            id: editRolePosition
                                            from: 0; to: 1000
                                            value: serverSettingsPopup.roleScratchPos
                                            onValueModified: serverSettingsPopup.roleScratchPos = value
                                            font.family: Theme.fontMono
                                            font.pixelSize: Theme.fontSize.md
                                            background: Rectangle {
                                                color: Theme.bg1
                                                radius: Theme.r2
                                                border.color: Theme.line
                                                border.width: 1
                                                implicitWidth: 100
                                                implicitHeight: Theme.controlHeight.md
                                            }
                                        }
                                    }

                                    // Role color palette — the four Designer accents up
                                    // front, then a wider gamut of classic chat role
                                    // colors. Selected swatch gets an fg0 ring.
                                    Row {
                                        spacing: Theme.sp.s2
                                        Repeater {
                                            model: serverSettingsPopup.roleColorSwatches
                                            delegate: Rectangle {
                                                width: 22; height: 22; radius: 11
                                                color: modelData
                                                readonly property bool selected:
                                                    serverSettingsPopup.roleScratchColor === modelData
                                                border.color: selected ? Theme.fg0 : Theme.line
                                                border.width: selected ? 3 : 1
                                                Behavior on border.width {
                                                    NumberAnimation { duration: Theme.motion.fastMs }
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: serverSettingsPopup.roleScratchColor = modelData
                                                }
                                            }
                                        }
                                    }

                                    // `mentionable` sits ABOVE the permissions grid and
                                    // outside it on purpose. It is not a permission: a
                                    // permission says what the HOLDER of this role may do,
                                    // and this says what everybody else may do TO the role.
                                    // Putting it in the grid would file it under "things
                                    // this role can do", which is how it gets read as
                                    // harmless and left off checks like same_role().
                                    Row {
                                        Layout.topMargin: Theme.sp.s3
                                        spacing: 6
                                        ThemedCheckBox {
                                            id: mentionableBox
                                            checked: serverSettingsPopup.roleScratchMentionable
                                            onToggled: serverSettingsPopup.roleScratchMentionable
                                                = !serverSettingsPopup.roleScratchMentionable
                                        }
                                        Text {
                                            text: "Allow anyone to @mention this role"
                                            color: Theme.fg0
                                            font.family: Theme.fontSans
                                            font.pixelSize: Theme.fontSize.sm
                                            anchors.verticalCenter: mentionableBox.verticalCenter
                                        }
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        text: "Members with \u201cMention Everyone\u201d can "
                                              + "ping this role either way."
                                        color: Theme.fg3
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.xs
                                    }

                                    // Sits beside `mentionable` for the same
                                    // reason: a property of the role, not of
                                    // its holder. The pairing is not
                                    // decorative — a boss-notification role is
                                    // both, and the two together are the whole
                                    // mechanism: members opt in by taking the
                                    // role, the bot notifies by mentioning it,
                                    // and there is no subscriber table
                                    // anywhere.
                                    Row {
                                        Layout.topMargin: Theme.sp.s3
                                        spacing: 6
                                        ThemedCheckBox {
                                            id: selfAssignableBox
                                            checked: serverSettingsPopup.roleScratchSelfAssignable
                                            onToggled: serverSettingsPopup.roleScratchSelfAssignable
                                                = !serverSettingsPopup.roleScratchSelfAssignable
                                        }
                                        Text {
                                            text: "Let members add and remove this role themselves"
                                            color: Theme.fg0
                                            font.family: Theme.fontSans
                                            font.pixelSize: Theme.fontSize.sm
                                            anchors.verticalCenter: selfAssignableBox.verticalCenter
                                        }
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        text: "It appears in every member\u2019s Your Roles "
                                              + "picker. The server refuses this unless the "
                                              + "role\u2019s permissions are already covered by "
                                              + "@everyone \u2014 and refuses to hand it out "
                                              + "later if that stops being true."
                                        color: Theme.fg3
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.xs
                                    }

                                    Text {
                                        Layout.topMargin: Theme.sp.s3
                                        text: "PERMISSIONS"
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.xs
                                        font.weight: Theme.fontWeight.semibold
                                        font.letterSpacing: Theme.trackWidest.xs
                                        color: Theme.fg3
                                    }

                                    Grid {
                                        Layout.fillWidth: true
                                        columns: 2
                                        columnSpacing: Theme.sp.s7
                                        rowSpacing: Theme.sp.s1

                                        Repeater {
                                            model: serverSettingsPopup.permissionFlags
                                            delegate: Row {
                                                spacing: 6
                                                ThemedCheckBox {
                                                    id: cb
                                                    // The bare read of roleScratchPerms is
                                                    // the dependency, and is deliberate: a
                                                    // property only read INSIDE the helper can
                                                    // be dead-code-eliminated on QML's
                                                    // AOT-compiled path, after which the box
                                                    // would never re-evaluate on a toggle or on
                                                    // switching to another role. Same defence,
                                                    // and same reason, as MessageInput's.
                                                    checked: serverSettingsPopup.roleScratchPerms >= 0
                                                        && serverSettingsPopup.roleScratchHasPerm(modelData.flag)
                                                    onToggled: serverSettingsPopup.toggleRoleScratchPerm(modelData.flag)
                                                }
                                                Text {
                                                    id: permLabel
                                                    text: modelData.label
                                                    color: Theme.fg0
                                                    font.family: Theme.fontSans
                                                    font.pixelSize: Theme.fontSize.sm
                                                    anchors.verticalCenter: cb.verticalCenter

                                                    // Hover explanation. The grid is two narrow
                                                    // columns, so a second line of body text per row
                                                    // would double its height; a tooltip keeps the
                                                    // list scannable while still saying what the
                                                    // permission actually does.
                                                    HoverHandler { id: permHover }
                                                    ToolTip.visible: permHover.hovered
                                                                  && modelData.hint.length > 0
                                                    ToolTip.text: modelData.hint
                                                    ToolTip.delay: 400
                                                }
                                            }
                                        }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.topMargin: Theme.sp.s3
                                        spacing: Theme.sp.s3

                                        // Primary Save — accent pill.
                                        Button {
                                            id: roleSaveBtn
                                            contentItem: Text {
                                                text: "Save role"
                                                font.family: Theme.fontSans
                                                font.pixelSize: Theme.fontSize.md
                                                font.weight: Theme.fontWeight.semibold
                                                color: Theme.onAccent
                                                horizontalAlignment: Text.AlignHCenter
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                            background: Rectangle {
                                                color: roleSaveBtn.hovered ? Theme.accentDim : Theme.accent
                                                radius: Theme.r2
                                                implicitHeight: Theme.controlHeight.md
                                                implicitWidth: 120
                                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                            }
                                            onClicked: {
                                                if (!serverManager.activeServer) return;
                                                var permsVal = Number(serverSettingsPopup.roleScratchPerms) | 0;
                                                var existing = serverManager.activeServer.serverRoles;
                                                var out = [];
                                                var myId = roleEditCard.editRole.id || roleEditCard.editRole.name;
                                                for (var j = 0; j < existing.length; j++) {
                                                    var r = existing[j];
                                                    var rid = r.id || r.name;
                                                    if (rid === myId) {
                                                        out.push({
                                                            id: myId,
                                                            name: serverSettingsPopup.roleScratchName.trim() || r.name,
                                                            color: serverSettingsPopup.roleScratchColor || r.color,
                                                            position: serverSettingsPopup.roleScratchPos,
                                                            permissions: "0x" + permsVal.toString(16),
                                                            mentionable: serverSettingsPopup.roleScratchMentionable,
                                                            hoist: r.hoist || false,
                                                            // Edited here now, and still written
                                                            // unconditionally: this save rebuilds
                                                            // the role object field by field and
                                                            // PUTs the WHOLE role list, so any
                                                            // field the literal forgets is silently
                                                            // cleared on every role edit. The
                                                            // scratch is seeded from the role on
                                                            // open, so an admin who never touches
                                                            // the checkbox still writes back what
                                                            // was there.
                                                            self_assignable: serverSettingsPopup.roleScratchSelfAssignable
                                                        });
                                                    } else {
                                                        out.push(r);
                                                    }
                                                }
                                                serverManager.activeServer.updateServerRoles(out);
                                                serverSettingsPopup.endRoleEdit();
                                            }
                                        }

                                        // Ghost danger Delete — hollow with danger
                                        // border, fills on hover.
                                        Button {
                                            id: roleDeleteBtn
                                            visible: (roleEditCard.editRole.id || roleEditCard.editRole.name) !== "everyone"
                                                  && (roleEditCard.editRole.id || roleEditCard.editRole.name) !== "admin"
                                            contentItem: Text {
                                                text: "Delete role"
                                                font.family: Theme.fontSans
                                                font.pixelSize: Theme.fontSize.md
                                                font.weight: Theme.fontWeight.semibold
                                                color: roleDeleteBtn.hovered ? Theme.onAccent : Theme.danger
                                                horizontalAlignment: Text.AlignHCenter
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                            background: Rectangle {
                                                color: roleDeleteBtn.hovered ? Theme.danger : "transparent"
                                                radius: Theme.r2
                                                border.color: Theme.danger
                                                border.width: 1
                                                implicitHeight: Theme.controlHeight.md
                                                implicitWidth: 120
                                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                            }
                                            onClicked: {
                                                if (!serverManager.activeServer) return;
                                                var existing = serverManager.activeServer.serverRoles;
                                                var out = [];
                                                var myId = roleEditCard.editRole.id || roleEditCard.editRole.name;
                                                for (var j = 0; j < existing.length; j++) {
                                                    var r = existing[j];
                                                    var rid = r.id || r.name;
                                                    if (rid !== myId) out.push(r);
                                                }
                                                serverManager.activeServer.updateServerRoles(out);
                                                serverSettingsPopup.endRoleEdit();
                                            }
                                        }

                                        Item { Layout.fillWidth: true }
                                    }
                                }
                            }
                        }

                        Text {
                            anchors.centerIn: parent
                            visible: parent.count === 0
                            text: "No roles configured — defaults will seed on first boot."
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.md
                            color: Theme.fg2
                        }
                    }

                    // Add-role footer — accent pill, r2/36h matching the
                    // rest of the settings button vocabulary. Inline icon +
                    // label instead of a unicode plus glyph.
                    Button {
                        id: addRoleBtn
                        contentItem: RowLayout {
                            spacing: Theme.sp.s2
                            Icon { name: "plus"; size: 14; color: Theme.onAccent; Layout.alignment: Qt.AlignVCenter }
                            Text {
                                text: "Add role"
                                color: Theme.onAccent
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.md
                                font.weight: Theme.fontWeight.semibold
                                Layout.alignment: Qt.AlignVCenter
                            }
                        }
                        background: Rectangle {
                            color: addRoleBtn.hovered ? Theme.accentDim : Theme.accent
                            radius: Theme.r2
                            implicitHeight: Theme.controlHeight.md
                            implicitWidth: 140
                            Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                        }
                        onClicked: {
                            if (!serverManager.activeServer) return;
                            var existing = serverManager.activeServer.serverRoles || [];
                            var out = [];
                            var maxPos = 0;
                            for (var i = 0; i < existing.length; i++) {
                                out.push(existing[i]);
                                if (existing[i].position > maxPos) maxPos = existing[i].position;
                            }
                            var newId = "role-" + Date.now();
                            out.push({
                                id: newId,
                                name: "New Role",
                                // Default new-role swatch — Designer cyan.
                                // (Was Discord blurple; swapped for
                                // visual continuity with our accent.)
                                color: serverSettingsPopup.defaultRoleColor,
                                position: maxPos + 1,
                                // Same as kEveryoneDefault in Permissions.h:
                                // view + send + attach + embed (0x000f) plus
                                // CHANGE_NICKNAME (0x0800) and ADD_REACTIONS
                                // (0x4000). Reactions are in the default
                                // because the flag exists to make denying them
                                // possible, not to make reacting a privilege —
                                // a new role without it could post but not
                                // react, which nobody creating a role means.
                                permissions: "0x480f",
                                mentionable: false,
                                // A new role is never opt-in. Making it so is
                                // a deliberate act with a containment rule
                                // attached (the server refuses a
                                // self-assignable role that grants more than
                                // @everyone), and defaulting it on would mean
                                // every role an admin created was one every
                                // member could silently take.
                                self_assignable: false,
                                hoist: false
                            });
                            serverManager.activeServer.updateServerRoles(out);
                        }
                    }
                }
            }

            // ---- Members (index 2) ----
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    // 32 pt of gutter each side is a desktop dialog margin; on a
                    // phone it is a sixth of the screen. See Theme.mobileGutter.
                    anchors.margins: Theme.isMobile ? Theme.mobileGutter : Theme.sp.s7 * 2
                    spacing: Theme.sp.s7

                    TabHeader { title: "Members" }

                    // Search field
                    TextField {
                        id: memberSearchField
                        Layout.fillWidth: true
                        Layout.maximumWidth: 400
                        placeholderText: "Search members..."
                        placeholderTextColor: Theme.fg2
                        color: Theme.fg0
                        font.pixelSize: Theme.fontSize.md
                        background: Rectangle {
                            color: Theme.bg0
                            radius: Theme.r2
                            border.color: memberSearchField.activeFocus ? Theme.accent : Theme.line
                            border.width: 1
                        }
                        padding: Theme.sp.s3
                    }

                    // Empty-state card when no members have synced yet.
                    // Mirrors the Bans-tab placeholder so the two tabs read
                    // as siblings.
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: {
                            if (!serverManager.activeServer) return true;
                            return serverManager.activeServer.serverMembers.length === 0;
                        }
                        ColumnLayout {
                            anchors.centerIn: parent
                            width: 360
                            spacing: Theme.sp.s4
                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 56
                                Layout.preferredHeight: 56
                                radius: Theme.r3
                                color: Theme.bg2
                                border.color: Theme.line
                                border.width: 1
                                Icon {
                                    anchors.centerIn: parent
                                    name: "users"
                                    size: 24
                                    color: Theme.fg3
                                }
                            }
                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: "No members synced yet"
                                color: Theme.fg0
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.lg
                                font.weight: Theme.fontWeight.semibold
                            }
                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                text: "Give it a moment — the member list populates from sync as each channel's state arrives."
                                color: Theme.fg2
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.sm
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        visible: serverManager.activeServer
                                 && serverManager.activeServer.serverMembers.length > 0

                        ScrollBar.vertical: ThemedScrollBar { id: membersScrollBar }
                        // Server-wide member union (not the active-room model
                        // which only knows about the currently-selected
                        // channel). See ServerConnection::serverMembers.
                        model: serverManager.activeServer
                               ? serverManager.activeServer.serverMembers : []
                        spacing: 2

                        delegate: Column {
                            id: memberRow
                            width: (ListView.view ? ListView.view.width : 400)
                                   - membersScrollBar.reservedWidth
                            // D-M3: a Column delegate keeps its implicit height
                            // when hidden, so filtering the list left a run of
                            // blank gaps where the non-matching rows used to be.
                            height: visible ? implicitHeight : 0
                            visible: {
                                var search = memberSearchField.text.toLowerCase();
                                if (search.length === 0) return true;
                                var dn = modelData.displayName ? modelData.displayName.toLowerCase() : "";
                                var uid = modelData.userId ? modelData.userId.toLowerCase() : "";
                                return dn.indexOf(search) >= 0 || uid.indexOf(search) >= 0;
                            }
                            readonly property string memberUserId: modelData.userId || ""
                            readonly property bool expanded: serverSettingsPopup.editingMemberId === memberUserId
                            // The set of assigned role ids lives on the popup
                            // (serverSettingsPopup.memberScratchRoles), seeded
                            // once by beginMemberEdit. It used to be a property
                            // on this delegate, which a sync-driven refresh of
                            // serverMembers destroyed mid-edit. See D-M4.

                            Rectangle {
                                width: parent.width
                                height: 52
                                radius: Theme.r2
                                color: memberItemMouse.containsMouse ? Theme.bg3 : Theme.bg2
                                border.color: parent.expanded ? Theme.accent : Theme.line
                                border.width: 1
                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                Behavior on border.color { ColorAnimation { duration: Theme.motion.fastMs } }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: Theme.sp.s5
                                    anchors.rightMargin: Theme.sp.s4
                                    spacing: Theme.sp.s4

                                    // Rounded-square avatar to match the
                                    // ServerRail + MemberList treatment
                                    // instead of the old circle.
                                    Rectangle {
                                        width: Theme.avatar.md
                                        height: Theme.avatar.md
                                        radius: Theme.r2
                                        color: Theme.senderColor(modelData.userId || "")
                                        Text {
                                            anchors.centerIn: parent
                                            text: {
                                                var n = modelData.displayName || modelData.userId || "?";
                                                var s = n.replace(/^[^a-zA-Z0-9]+/, "");
                                                return (s.length > 0 ? s.charAt(0) : "?").toUpperCase();
                                            }
                                            font.family: Theme.fontSans
                                            font.pixelSize: 13
                                            font.weight: Theme.fontWeight.semibold
                                            color: Theme.onAccent
                                        }
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        // mxid pinned above name+chips so the
                                        // three-line stack reads: display
                                        // name → id → assigned roles.
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: Theme.sp.s2
                                            Text {
                                                text: modelData.displayName || ""
                                                font.family: Theme.fontSans
                                                font.pixelSize: Theme.fontSize.md
                                                font.weight: Theme.fontWeight.semibold
                                                color: Theme.fg0
                                                elide: Text.ElideRight
                                            }
                                            Text {
                                                text: modelData.userId || ""
                                                font.family: Theme.fontMono
                                                font.pixelSize: Theme.fontSize.xs
                                                color: Theme.fg3
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }
                                        }

                                        // Assigned-role chips. Binds to
                                        // permissionsGeneration so the row
                                        // updates immediately after save —
                                        // no expand/collapse required.
                                        Flow {
                                            Layout.fillWidth: true
                                            spacing: 4
                                            readonly property int _gen:
                                                serverManager.activeServer
                                                    ? serverManager.activeServer.permissionsGeneration : 0
                                            readonly property var assigned: {
                                                _gen;
                                                if (!serverManager.activeServer) return [];
                                                return serverManager.activeServer.memberRoles(memberRow.memberUserId);
                                            }
                                            visible: assigned.length > 0
                                            Repeater {
                                                model: parent.assigned
                                                delegate: Rectangle {
                                                    // Resolve the role's
                                                    // colour + display name
                                                    // from serverRoles; fall
                                                    // back to the raw id.
                                                    readonly property var roleInfo: {
                                                        if (!serverManager.activeServer) return null;
                                                        var list = serverManager.activeServer.serverRoles;
                                                        for (var i = 0; i < list.length; i++) {
                                                            var rid = list[i].id || list[i].name;
                                                            if (rid === modelData) return list[i];
                                                        }
                                                        return null;
                                                    }
                                                    readonly property color rcolor:
                                                        roleInfo && roleInfo.color ? roleInfo.color : Theme.fg3
                                                    readonly property string rname:
                                                        roleInfo && roleInfo.name ? roleInfo.name : modelData
                                                    visible: modelData !== "everyone"
                                                    implicitWidth: chipRow.implicitWidth + 10
                                                    implicitHeight: 18
                                                    radius: 9
                                                    color: Qt.rgba(rcolor.r, rcolor.g, rcolor.b, 0.14)
                                                    border.color: Qt.rgba(rcolor.r, rcolor.g, rcolor.b, 0.38)
                                                    border.width: 1
                                                    RowLayout {
                                                        id: chipRow
                                                        anchors.centerIn: parent
                                                        spacing: 4
                                                        Rectangle {
                                                            width: 6; height: 6; radius: 3
                                                            color: parent.parent.rcolor
                                                            Layout.alignment: Qt.AlignVCenter
                                                        }
                                                        Text {
                                                            text: parent.parent.rname
                                                            color: parent.parent.rcolor
                                                            font.family: Theme.fontSans
                                                            font.pixelSize: 10
                                                            font.weight: Theme.fontWeight.semibold
                                                            Layout.alignment: Qt.AlignVCenter
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    // Chevron to mirror the role row.
                                    Icon {
                                        name: "chevron-right"
                                        size: 14
                                        color: Theme.fg2
                                        rotation: parent.parent.expanded ? 90 : 0
                                        Behavior on rotation {
                                            NumberAnimation { duration: Theme.motion.fastMs
                                                              easing.type: Easing.BezierSpline
                                                              easing.bezierCurve: Theme.motion.bezier }
                                        }
                                    }

                                    // Chevron alone carries the expand/collapse
                                    // affordance; no more duplicate "Roles ▸" /
                                    // "Close ▾" text — it read as two different
                                    // buttons instead of one gesture.
                                }

                                MouseArea {
                                    id: memberItemMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (memberRow.expanded) {
                                            serverSettingsPopup.endMemberEdit();
                                        } else {
                                            serverSettingsPopup.beginMemberEdit(memberRow.memberUserId);
                                        }
                                    }
                                }
                            }

                            // Role assignment card — mirrors the role-edit
                            // card's treatment (r2 bg0 with line border) so the
                            // two expansion patterns read consistently.
                            Rectangle {
                                id: roleAssignCard
                                width: parent.width
                                visible: memberRow.expanded
                                radius: Theme.r2
                                color: Theme.bg0
                                border.color: Theme.line
                                border.width: 1
                                height: visible ? roleAssignCol.implicitHeight + Theme.sp.s7 * 2 : 0

                                ColumnLayout {
                                    id: roleAssignCol
                                    anchors.fill: parent
                                    anchors.margins: Theme.sp.s7
                                    spacing: Theme.sp.s3

                                    Text {
                                        text: "ASSIGNED ROLES"
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.xs
                                        font.weight: Theme.fontWeight.semibold
                                        font.letterSpacing: Theme.trackWidest.xs
                                        color: Theme.fg3
                                    }

                                    Repeater {
                                        model: serverManager.activeServer ? serverManager.activeServer.serverRoles : []
                                        delegate: Rectangle {
                                            // Each role is a bg1-tinted row with
                                            // checkbox + color dot + name. Hover
                                            // flips the row bg so the click
                                            // affordance reads even when the
                                            // checkbox is a small target.
                                            readonly property string roleId: modelData.id || modelData.name
                                            visible: roleId !== "everyone"
                                            Layout.fillWidth: true
                                            implicitHeight: Theme.controlHeight.sm
                                            radius: Theme.r1
                                            color: assignHover.containsMouse ? Theme.bg2 : "transparent"
                                            Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.leftMargin: Theme.sp.s3
                                                anchors.rightMargin: Theme.sp.s3
                                                spacing: Theme.sp.s3

                                                ThemedCheckBox {
                                                    id: rolecb
                                                    checked: serverSettingsPopup.memberScratchHasRole(
                                                        parent.parent.roleId)
                                                    onToggled: serverSettingsPopup.toggleMemberScratchRole(
                                                        parent.parent.roleId)
                                                }
                                                Rectangle {
                                                    Layout.alignment: Qt.AlignVCenter
                                                    width: 12; height: 12; radius: 6
                                                    color: modelData.color || Theme.accent
                                                    border.color: Theme.bg0
                                                    border.width: 1
                                                }
                                                Text {
                                                    Layout.alignment: Qt.AlignVCenter
                                                    Layout.fillWidth: true
                                                    text: modelData.name || ""
                                                    color: Theme.fg0
                                                    font.family: Theme.fontSans
                                                    font.pixelSize: Theme.fontSize.md
                                                    font.weight: Theme.fontWeight.medium
                                                    elide: Text.ElideRight
                                                }
                                            }

                                            MouseArea {
                                                id: assignHover
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                // Click anywhere on the row
                                                // toggles the role in/out of the
                                                // popup-level scratch set; the
                                                // checkbox's `checked` binding
                                                // then flips visually.
                                                //
                                                // We used to call rolecb.toggle()
                                                // here, but Qt Controls only
                                                // fires the CheckBox's
                                                // `toggled` signal for real
                                                // user clicks on the indicator
                                                // — a programmatic toggle()
                                                // flips state silently with no
                                                // `toggled` emission. So
                                                // clicks that landed on the
                                                // row (not the 18px box)
                                                // visually flipped but never
                                                // reached onToggled, and the
                                                // save path sent the unchanged
                                                // role list.
                                                onClicked: serverSettingsPopup.toggleMemberScratchRole(
                                                    parent.roleId)
                                            }
                                        }
                                    }

                                    // Action row — primary Save (accent pill)
                                    // on the left; ghost-danger Kick + filled
                                    // danger Ban pushed to the right so the
                                    // destructive actions don't compete with
                                    // the role-assignment save. Self is
                                    // protected: the buttons disappear when
                                    // editing your own account.
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.topMargin: Theme.sp.s3
                                        spacing: Theme.sp.s3

                                        readonly property bool isSelf:
                                            serverManager.activeServer
                                            && memberRow.memberUserId
                                               === serverManager.activeServer.userId

                                        Button {
                                            id: roleAssignSaveBtn
                                            contentItem: Text {
                                                text: "Save assignments"
                                                font.family: Theme.fontSans
                                                font.pixelSize: Theme.fontSize.md
                                                font.weight: Theme.fontWeight.semibold
                                                color: Theme.onAccent
                                                horizontalAlignment: Text.AlignHCenter
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                            background: Rectangle {
                                                color: roleAssignSaveBtn.hovered ? Theme.accentDim : Theme.accent
                                                radius: Theme.r2
                                                implicitHeight: Theme.controlHeight.md
                                                implicitWidth: 160
                                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                            }
                                            onClicked: {
                                                if (!serverManager.activeServer) return;
                                                var ids = [];
                                                var scratch = serverSettingsPopup.memberScratchRoles;
                                                for (var k in scratch) {
                                                    if (scratch[k]) ids.push(k);
                                                }
                                                serverManager.activeServer.setMemberRoles(
                                                    memberRow.memberUserId, ids);
                                                serverSettingsPopup.endMemberEdit();
                                            }
                                        }

                                        Item { Layout.fillWidth: true }

                                        // Ghost-danger Kick — hollow with
                                        // danger border, fills on hover.
                                        // Hidden for the current user.
                                        Button {
                                            id: kickBtn
                                            visible: !parent.isSelf
                                            contentItem: Text {
                                                text: "Kick"
                                                font.family: Theme.fontSans
                                                font.pixelSize: Theme.fontSize.md
                                                font.weight: Theme.fontWeight.semibold
                                                color: kickBtn.hovered ? Theme.onAccent : Theme.danger
                                                horizontalAlignment: Text.AlignHCenter
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                            background: Rectangle {
                                                color: kickBtn.hovered ? Theme.danger : "transparent"
                                                radius: Theme.r2
                                                border.color: Theme.danger
                                                border.width: 1
                                                implicitHeight: Theme.controlHeight.md
                                                implicitWidth: 80
                                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                            }
                                            onClicked: {
                                                serverSettingsPopup.confirmMod = {
                                                    kind: "kick",
                                                    userId: memberRow.memberUserId,
                                                    displayName: modelData.displayName || memberRow.memberUserId
                                                };
                                                serverSettingsPopup.confirmModDialog.open();
                                            }
                                        }

                                        // Filled danger Ban — primary
                                        // destructive action, strongest read.
                                        Button {
                                            id: banBtn
                                            visible: !parent.isSelf
                                            contentItem: Text {
                                                text: "Ban"
                                                font.family: Theme.fontSans
                                                font.pixelSize: Theme.fontSize.md
                                                font.weight: Theme.fontWeight.semibold
                                                color: Theme.onAccent
                                                horizontalAlignment: Text.AlignHCenter
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                            background: Rectangle {
                                                color: banBtn.hovered
                                                       ? Qt.lighter(Theme.danger, 1.1)
                                                       : Theme.danger
                                                radius: Theme.r2
                                                implicitHeight: Theme.controlHeight.md
                                                implicitWidth: 80
                                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                            }
                                            onClicked: {
                                                serverSettingsPopup.confirmMod = {
                                                    kind: "ban",
                                                    userId: memberRow.memberUserId,
                                                    displayName: modelData.displayName || memberRow.memberUserId
                                                };
                                                serverSettingsPopup.confirmModDialog.open();
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ---- Bots (index 3) ----
            //
            // The whole surface is BotManagerPane; this wrapper exists only
            // so the StackLayout child order keeps matching the nav model
            // above. Inserting here rather than appending is deliberate: the
            // nav Repeater's `index` IS the StackLayout index, so the two
            // lists are one ordering expressed twice and a page appended at
            // the end while its nav row sat in the middle would silently
            // show the wrong tab for everything after it.
            Item {
                BotManagerPane {
                    anchors.fill: parent
                    // A StackLayout gives every child the full size but only
                    // makes the current one visible, which is exactly the
                    // signal BotManagerPane's onVisibleChanged refresh wants.
                }
            }

            // ---- Channels (index 4) ----
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    // 32 pt of gutter each side is a desktop dialog margin; on a
                    // phone it is a sixth of the screen. See Theme.mobileGutter.
                    anchors.margins: Theme.isMobile ? Theme.mobileGutter : Theme.sp.s7 * 2
                    spacing: Theme.sp.s7

                    RowLayout {
                        Layout.fillWidth: true
                        TabHeader { title: "Channels"; Layout.fillWidth: true }

                        // Top-right "+ Add category" — matches the accent-pill
                        // vocabulary used on "Add role" in the Roles tab. The
                        // create-prompt popup is owned by ChannelList, so we
                        // emit a signal instead of opening it directly.
                        Button {
                            id: addCategoryBtn
                            contentItem: RowLayout {
                                spacing: Theme.sp.s2
                                Icon { name: "plus"; size: 14; color: Theme.onAccent; Layout.alignment: Qt.AlignVCenter }
                                Text {
                                    text: "Add category"
                                    color: Theme.onAccent
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.fontSize.md
                                    font.weight: Theme.fontWeight.semibold
                                    Layout.alignment: Qt.AlignVCenter
                                }
                            }
                            background: Rectangle {
                                color: addCategoryBtn.hovered ? Theme.accentDim : Theme.accent
                                radius: Theme.r2
                                implicitHeight: Theme.controlHeight.md
                                implicitWidth: 150
                                Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                            }
                            onClicked: serverSettingsPopup.createChannelRequested("category", "")
                        }
                    }

                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true

                        ScrollBar.vertical: ThemedScrollBar { id: channelsScrollBar }
                        model: serverManager.activeServer ? serverManager.activeServer.categorizedRooms : []
                        spacing: Theme.sp.s3

                        delegate: Column {
                            width: (ListView.view ? ListView.view.width : 400)
                                   - channelsScrollBar.reservedWidth
                            readonly property string catId: modelData.categoryId || ""

                            // Category header — widest-tracked small caps
                            // matching the channel-list + settings labels.
                            // Hover reveals "+ Add text / + Add voice"
                            // affordances pushed to the right.
                            Rectangle {
                                id: catHeader
                                width: parent.width
                                height: 32
                                color: "transparent"
                                visible: parent.catId !== ""

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: Theme.sp.s1
                                    anchors.rightMargin: Theme.sp.s1
                                    spacing: Theme.sp.s2

                                    Text {
                                        text: (modelData.categoryName || "").toUpperCase()
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.xs
                                        font.weight: Theme.fontWeight.semibold
                                        font.letterSpacing: Theme.trackWidest.xs
                                        color: Theme.fg3
                                        Layout.fillWidth: true
                                    }

                                    Text {
                                        text: modelData.channels ? modelData.channels.length + " channels" : "0 channels"
                                        font.family: Theme.fontMono
                                        font.pixelSize: Theme.fontSize.xs
                                        color: Theme.fg3
                                        // On mobile the add buttons are always
                                        // out (no hover to reveal them), so this
                                        // always yields the space to them.
                                        visible: !Theme.isMobile && !catHeaderHover.containsMouse
                                    }

                                    // Inline create buttons — only show on
                                    // category hover so the list stays quiet
                                    // at rest. Each is an icon-only 24×24
                                    // ghost button with an accent-on-hover tint.
                                    component CatAddBtn: Rectangle {
                                        id: _cab
                                        property string iconName: ""
                                        property string tooltipText: ""
                                        property string createKind: ""
                                        Layout.preferredWidth: Theme.isMobile ? Theme.touchTarget : 24
                                        Layout.preferredHeight: Theme.isMobile ? Theme.touchTarget : 24
                                        radius: Theme.r1
                                        color: _cabMouse.containsMouse ? Theme.bg3 : "transparent"
                                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                        Icon {
                                            anchors.centerIn: parent
                                            name: _cab.iconName
                                            size: 12
                                            color: _cabMouse.containsMouse ? Theme.accent : Theme.fg2
                                        }
                                        MouseArea {
                                            id: _cabMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: serverSettingsPopup.createChannelRequested(
                                                           _cab.createKind, catHeader.parent.catId)
                                        }
                                        ToolTip.visible: _cabMouse.containsMouse && tooltipText.length > 0
                                        ToolTip.text: tooltipText
                                        ToolTip.delay: 500
                                    }

                                    CatAddBtn {
                                        iconName: "hash"
                                        tooltipText: "Add text channel"
                                        createKind: "text"
                                        visible: Theme.isMobile || catHeaderHover.containsMouse
                                    }
                                    CatAddBtn {
                                        iconName: "volume"
                                        tooltipText: "Add voice channel"
                                        createKind: "voice"
                                        visible: Theme.isMobile || catHeaderHover.containsMouse
                                    }
                                }

                                MouseArea {
                                    id: catHeaderHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    acceptedButtons: Qt.NoButton  // pass clicks through to the CatAddBtn children
                                }
                            }

                            // Channels in category.
                            Repeater {
                                model: modelData.channels

                                delegate: Rectangle {
                                    width: parent.width
                                    height: 36
                                    radius: Theme.r1
                                    color: chSettingsMouse.containsMouse ? Theme.bg3 : "transparent"
                                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                                    readonly property string rowRoomId: modelData.roomId || ""
                                    readonly property string rowRoomName: modelData.displayName || ""
                                    readonly property bool rowIsVoice: modelData.isVoice === true

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: Theme.sp.s4
                                        anchors.rightMargin: Theme.sp.s2
                                        spacing: Theme.sp.s3

                                        Icon {
                                            name: parent.parent.rowIsVoice ? "volume" : "hash"
                                            size: 14
                                            color: chSettingsMouse.containsMouse ? Theme.accent : Theme.fg2
                                        }

                                        Text {
                                            text: parent.parent.rowRoomName
                                            font.family: Theme.fontSans
                                            font.pixelSize: Theme.fontSize.md
                                            color: Theme.fg1
                                            Layout.fillWidth: true
                                            elide: Text.ElideRight
                                        }

                                        Text {
                                            text: (modelData.roomType || (parent.parent.rowIsVoice ? "voice" : "text")).toUpperCase()
                                            font.family: Theme.fontMono
                                            font.pixelSize: Theme.fontSize.xs
                                            font.letterSpacing: Theme.trackWide.xs
                                            color: Theme.fg3
                                        }

                                        // Settings (text channels only) and
                                        // Delete (all channels) on hover. Each
                                        // has its own MouseArea so the chrome
                                        // reliably routes clicks — bare Icon
                                        // inside a parent MouseArea would
                                        // eat everything.
                                        Rectangle {
                                            Layout.preferredWidth: Theme.isMobile ? Theme.touchTarget : 24
                                            Layout.preferredHeight: Theme.isMobile ? Theme.touchTarget : 24
                                            radius: Theme.r1
                                            color: _settingsMouse.containsMouse ? Theme.bg2 : "transparent"
                                            // Reveal-on-hover has no touch
                                            // equivalent: on a phone these two
                                            // were the whole of "edit or delete a
                                            // channel", invisible and unreachable.
                                            visible: (Theme.isMobile || chSettingsMouse.containsMouse)
                                                   && !parent.parent.rowIsVoice
                                            Icon {
                                                anchors.centerIn: parent
                                                name: "settings"
                                                size: 12
                                                color: _settingsMouse.containsMouse ? Theme.fg0 : Theme.fg2
                                            }
                                            MouseArea {
                                                id: _settingsMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    var rid = parent.parent.parent.rowRoomId;
                                                    var rname = parent.parent.parent.rowRoomName;
                                                    serverSettingsPopup.close();
                                                    serverSettingsPopup.channelSettingsRequested(rid, rname);
                                                }
                                            }
                                            ToolTip.visible: _settingsMouse.containsMouse
                                            ToolTip.text: "Channel settings"
                                            ToolTip.delay: 500
                                        }
                                        Rectangle {
                                            Layout.preferredWidth: Theme.isMobile ? Theme.touchTarget : 24
                                            Layout.preferredHeight: Theme.isMobile ? Theme.touchTarget : 24
                                            radius: Theme.r1
                                            color: _deleteMouse.containsMouse
                                                   ? Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.16)
                                                   : "transparent"
                                            visible: Theme.isMobile || chSettingsMouse.containsMouse
                                            Icon {
                                                anchors.centerIn: parent
                                                name: "x"
                                                size: 12
                                                color: _deleteMouse.containsMouse ? Theme.danger : Theme.fg2
                                            }
                                            MouseArea {
                                                id: _deleteMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    var rid = parent.parent.parent.rowRoomId;
                                                    var rname = parent.parent.parent.rowRoomName;
                                                    serverSettingsPopup.channelDeleteRequested(rid, rname);
                                                }
                                            }
                                            ToolTip.visible: _deleteMouse.containsMouse
                                            ToolTip.text: "Delete channel"
                                            ToolTip.delay: 500
                                        }
                                    }

                                    MouseArea {
                                        id: chSettingsMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        // Hover-only — clicks on the settings/
                                        // delete buttons are handled by their
                                        // own MouseAreas stacked above.
                                        acceptedButtons: Qt.NoButton
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ---- Bans (index 5) ----
            // Aggregates banned users across every room we've synced. The
            // list is a snapshot — drop to empty-state when there's nothing
            // to show rather than rendering a blank ListView.
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    // 32 pt of gutter each side is a desktop dialog margin; on a
                    // phone it is a sixth of the screen. See Theme.mobileGutter.
                    anchors.margins: Theme.isMobile ? Theme.mobileGutter : Theme.sp.s7 * 2
                    spacing: Theme.sp.s7

                    TabHeader { title: "Bans" }

                    readonly property var banList:
                        serverManager.activeServer
                            ? serverManager.activeServer.bannedMembers : []

                    Text {
                        Layout.fillWidth: true
                        text: "Banned users can't see or join any channel on this server until they're unbanned."
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.sm
                        color: Theme.fg2
                        wrapMode: Text.WordWrap
                    }

                    // Empty-state card, shown when there are no bans.
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: parent.banList.length === 0

                        ColumnLayout {
                            anchors.centerIn: parent
                            width: 360
                            spacing: Theme.sp.s4

                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 56
                                Layout.preferredHeight: 56
                                radius: Theme.r3
                                color: Theme.bg2
                                border.color: Theme.line
                                border.width: 1
                                Icon {
                                    anchors.centerIn: parent
                                    name: "shield"
                                    size: 24
                                    color: Theme.fg3
                                }
                            }
                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: "No one is banned"
                                color: Theme.fg0
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.lg
                                font.weight: Theme.fontWeight.semibold
                            }
                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                text: "Ban a member from the Members tab and they'll show up here."
                                color: Theme.fg2
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.fontSize.sm
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                    // Populated list — each row shows avatar + name + mxid +
                    // room count + reason, with a ghost Unban button on the
                    // right. Same visual vocabulary as the Members rows so
                    // the two tabs read as siblings.
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        visible: parent.banList.length > 0
                        ScrollBar.vertical: ThemedScrollBar { id: bansScrollBar }
                        model: parent.banList
                        spacing: 4

                        delegate: Rectangle {
                            width: (ListView.view ? ListView.view.width : 400)
                                   - bansScrollBar.reservedWidth
                            height: 60
                            radius: Theme.r2
                            color: Theme.bg2
                            border.color: Theme.line
                            border.width: 1

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.sp.s5
                                anchors.rightMargin: Theme.sp.s4
                                spacing: Theme.sp.s4

                                Rectangle {
                                    width: Theme.avatar.md
                                    height: Theme.avatar.md
                                    radius: Theme.r2
                                    color: Theme.senderColor(modelData.userId || "")
                                    Text {
                                        anchors.centerIn: parent
                                        text: {
                                            var n = modelData.displayName || modelData.userId || "?";
                                            var s = n.replace(/^[^a-zA-Z0-9]+/, "");
                                            return (s.length > 0 ? s.charAt(0) : "?").toUpperCase();
                                        }
                                        font.family: Theme.fontSans
                                        font.pixelSize: 13
                                        font.weight: Theme.fontWeight.semibold
                                        color: Theme.onAccent
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0

                                    Text {
                                        text: modelData.displayName || modelData.userId || ""
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.md
                                        font.weight: Theme.fontWeight.semibold
                                        color: Theme.fg0
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.sp.s2

                                        Text {
                                            text: modelData.userId || ""
                                            font.family: Theme.fontMono
                                            font.pixelSize: Theme.fontSize.xs
                                            color: Theme.fg3
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                        // Reason, if any — quieter, italic.
                                        Text {
                                            visible: (modelData.reason || "") !== ""
                                            text: "· " + (modelData.reason || "")
                                            font.family: Theme.fontSans
                                            font.pixelSize: Theme.fontSize.xs
                                            font.italic: true
                                            color: Theme.fg3
                                            elide: Text.ElideRight
                                        }
                                    }
                                }

                                // Ghost Unban.
                                Button {
                                    id: unbanBtn
                                    contentItem: Text {
                                        text: "Unban"
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.fontSize.md
                                        font.weight: Theme.fontWeight.semibold
                                        color: unbanBtn.hovered ? Theme.fg0 : Theme.fg1
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    background: Rectangle {
                                        color: unbanBtn.hovered ? Theme.bg3 : "transparent"
                                        radius: Theme.r2
                                        border.color: Theme.line
                                        border.width: 1
                                        implicitHeight: Theme.controlHeight.sm
                                        implicitWidth: 88
                                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                                    }
                                    onClicked: {
                                        serverSettingsPopup.confirmMod = {
                                            kind: "unban",
                                            userId: modelData.userId,
                                            displayName: modelData.displayName || modelData.userId
                                        };
                                        serverSettingsPopup.confirmModDialog.open();
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // (Legacy top-right close-X removed; the new one lives in `background`
    // so it floats above content regardless of the current pane.)

    // D-C1. The bug this fixes renamed the WRONG SERVER: `serverNameField.text`
    // is declared as a binding on activeServer.serverName, but typing into a
    // TextField REPLACES that binding with the typed string — permanently, and
    // silently. Type a new name on server A, press Esc (which closes without
    // saving), switch to server B, reopen this dialog and press Save: the field
    // still holds A's half-typed name, the binding that would have refreshed it
    // is gone, and B gets renamed to it.
    //
    // So every field in this dialog whose value comes from a property and can
    // be written imperatively has to have its binding RE-ESTABLISHED when the
    // dialog is shown, with Qt.binding — reassigning the plain value would
    // simply install another one-shot string. `onAboutToShow` runs before the
    // popup is visible, so the field never paints the stale value.
    //
    // This is the rule for ClientSettings and ChannelSettings too, and it is
    // the shared cause behind D-M2 and D-M5.
    onAboutToShow: {
        selectedSection = 0;
        serverNameField.text = Qt.binding(function() {
            return serverManager.activeServer ? serverManager.activeServer.serverName : "";
        });
        // An expanded role or member editor from a previous visit would
        // otherwise still be open, pointing at whatever id was last touched —
        // possibly on a different server entirely.
        endRoleEdit();
        endMemberEdit();
        memberSearchField.text = "";
    }

    onOpened: {
        // D-L: the dialog opened with nothing focused, so the first keypress
        // went nowhere — including Esc, which is the documented way out.
        // `focus: true` (above) hands the popup focus on open; this makes the
        // content item the actual focus scope so Tab starts inside the dialog.
        contentItem.forceActiveFocus();
    }

    // Shared confirm dialog for kick / ban / unban. Reuses the delete-channel
    // popup vocabulary — bg1 + r3 + line, danger-tinted icon tile for
    // destructive actions, ghost Cancel + filled primary action.
    Popup {
        id: _confirmModDialog
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        width: 420
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: Theme.sp.s7

        readonly property bool isDestructive:
            serverSettingsPopup.confirmMod.kind === "kick"
            || serverSettingsPopup.confirmMod.kind === "ban"
        readonly property string titleText: {
            var who = serverSettingsPopup.confirmMod.displayName || "this member";
            switch (serverSettingsPopup.confirmMod.kind) {
                case "kick":  return "Kick " + who + "?";
                case "ban":   return "Ban " + who + "?";
                case "unban": return "Unban " + who + "?";
                default:      return "";
            }
        }
        readonly property string descText: {
            switch (serverSettingsPopup.confirmMod.kind) {
                case "kick":
                    return "They're removed from every channel on this server but can be invited back or rejoin if the server allows.";
                case "ban":
                    return "They're removed from every channel and prevented from rejoining until you unban them. Their messages stay — you can delete those separately.";
                case "unban":
                    return "They can be re-invited or rejoin this server (subject to channel permissions) once the ban is lifted.";
                default:
                    return "";
            }
        }

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
                    color: _confirmModDialog.isDestructive
                        ? Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.15)
                        : Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.15)
                    Icon {
                        anchors.centerIn: parent
                        name: _confirmModDialog.isDestructive ? "x" : "check"
                        size: 16
                        color: _confirmModDialog.isDestructive ? Theme.danger : Theme.accent
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: _confirmModDialog.titleText
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
                text: _confirmModDialog.descText
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                color: Theme.fg2
                wrapMode: Text.WordWrap
            }

            // Optional reason input — only for kick/ban, since unban doesn't
            // carry a reason in our model. Matrix attaches the reason to the
            // ban event so it shows up in the banned-members reason column.
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp.s1
                visible: _confirmModDialog.isDestructive
                Text {
                    text: "REASON (OPTIONAL)"
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.xs
                    font.weight: Theme.fontWeight.semibold
                    font.letterSpacing: Theme.trackWidest.xs
                    color: Theme.fg3
                }
                TextField {
                    id: reasonField
                    Layout.fillWidth: true
                    placeholderText: "Spam, harassment, off-topic, …"
                    placeholderTextColor: Theme.fg3
                    color: Theme.fg0
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.fontSize.md
                    background: Rectangle {
                        color: Theme.bg0
                        radius: Theme.r2
                        border.color: reasonField.activeFocus ? Theme.accent : Theme.line
                        border.width: 1
                    }
                    leftPadding: Theme.sp.s4
                    rightPadding: Theme.sp.s4
                    topPadding: Theme.sp.s3
                    bottomPadding: Theme.sp.s3
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.sp.s3
                spacing: Theme.sp.s3
                Item { Layout.fillWidth: true }

                Button {
                    id: cancelModBtn
                    contentItem: Text {
                        text: "Cancel"
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.medium
                        color: Theme.fg1
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: cancelModBtn.hovered ? Theme.bg3 : "transparent"
                        border.color: Theme.line
                        border.width: 1
                        radius: Theme.r2
                        implicitWidth: 100
                        implicitHeight: Theme.controlHeight.md
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                    onClicked: _confirmModDialog.close()
                }

                Button {
                    id: confirmModBtn
                    contentItem: Text {
                        text: {
                            switch (serverSettingsPopup.confirmMod.kind) {
                                case "kick":  return "Kick member";
                                case "ban":   return "Ban member";
                                case "unban": return "Unban member";
                                default:      return "Confirm";
                            }
                        }
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.md
                        font.weight: Theme.fontWeight.semibold
                        color: Theme.onAccent
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        readonly property color base: _confirmModDialog.isDestructive
                            ? Theme.danger : Theme.accent
                        color: confirmModBtn.hovered
                               ? (_confirmModDialog.isDestructive
                                   ? Qt.lighter(Theme.danger, 1.1)
                                   : Theme.accentDim)
                               : base
                        radius: Theme.r2
                        implicitWidth: 160
                        implicitHeight: Theme.controlHeight.md
                        Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
                    }
                    onClicked: {
                        if (!serverManager.activeServer) {
                            _confirmModDialog.close();
                            return;
                        }
                        var m = serverSettingsPopup.confirmMod;
                        var reason = reasonField.text.trim();
                        switch (m.kind) {
                            case "kick":
                                serverManager.activeServer.kickFromServer(m.userId, reason);
                                break;
                            case "ban":
                                serverManager.activeServer.banFromServer(m.userId, reason);
                                break;
                            case "unban":
                                serverManager.activeServer.unbanFromServer(m.userId);
                                break;
                        }
                        serverSettingsPopup.endMemberEdit();
                        _confirmModDialog.close();
                    }
                }
            }
        }

        onClosed: reasonField.text = ""
    }

    FileDialog {
        id: serverIconFileDialog
        title: "Choose Server Icon"
        nameFilters: ["Image files (*.png *.jpg *.jpeg *.gif *.webp)"]
        onAccepted: {
            if (!serverManager.activeServer) return;
            serverManager.activeServer.uploadServerAvatar(
                selectedFile.toString());
        }
    }
}
