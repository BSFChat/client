import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.platform as Platform
import BSFChat

// MessageInput is an implicitHeight-driven Rectangle so it can grow the
// vertical banner strip (when editing) without forcing the parent to
// re-layout around a fixed height.
// Composer (SPEC §3.6 bottom, 56h minimum, auto-grows to ~120h).
// r3 rounded container, bg2 surface, line border on focus.
Rectangle {
    id: inputRoot
    color: Theme.bg1
    border.color: inputArea.activeFocus ? Theme.accent : Theme.line
    border.width: 1
    radius: Theme.r3
    implicitHeight: (editingHeader.visible || replyHeader.visible)
        ? (editingHeader.visible ? editingHeader.height : replyHeader.height)
          + inputCore.implicitHeight + 8
        : inputCore.implicitHeight
    height: implicitHeight

    property string roomName: ""
    property string activeRoomId: serverManager.activeServer ? serverManager.activeServer.activeRoomId : ""
    property bool uploading: false

    // Editing state. When `editingEventId` is non-empty, pressing Enter
    // sends an edit event referencing that event_id rather than a new
    // m.room.message. Called from MessageView → MessageBubble.
    // Exposed so MessageView.focusComposer() can force focus via
    // Ctrl+L. alias avoids hardcoding the nested id path.
    property alias inputArea: inputArea

    property string editingEventId: ""
    property string editingOriginalBody: ""

    // Map of filename → progress (0..1) for currently-uploading files.
    // Kept as a dictionary so multiple simultaneous uploads each get
    // a row. A finished upload (progress == 1.0) stays in the map for
    // 400ms so the "complete" state is visible before it disappears.
    property var _uploads: ({})
    Connections {
        target: serverManager.activeServer
        ignoreUnknownSignals: true
        function onMediaUploadProgress(filename, progress) {
            var m = Object.assign({}, inputRoot._uploads);
            m[filename] = progress;
            inputRoot._uploads = m;
            if (progress >= 1.0) uploadSweepTimer.start();
        }
    }
    Timer {
        id: uploadSweepTimer
        interval: 400
        onTriggered: {
            var m = {};
            for (var k in inputRoot._uploads) {
                if (inputRoot._uploads[k] < 1.0) m[k] = inputRoot._uploads[k];
            }
            inputRoot._uploads = m;
        }
    }
    readonly property var _uploadKeys: Object.keys(inputRoot._uploads)
    function beginEditing(eventId, currentBody) {
        if (replyToEventId !== "") cancelReplying();
        editingEventId = eventId;
        editingOriginalBody = currentBody;
        inputArea.text = currentBody;
        inputArea.forceActiveFocus();
        inputArea.cursorPosition = currentBody.length;
    }
    function cancelEditing() {
        editingEventId = "";
        editingOriginalBody = "";
        inputArea.text = "";
    }

    // Reply state. When replyToEventId is non-empty, sendCurrentMessage()
    // routes through replyToMessage() instead of sendMessage(). Reply and
    // editing are mutually exclusive — beginReplying cancels any edit and
    // vice-versa.
    property string replyToEventId: ""
    property string replyToSenderName: ""
    property string replyToPreview: ""
    function beginReplying(eventId, senderName, preview) {
        if (editingEventId !== "") cancelEditing();
        replyToEventId = eventId;
        replyToSenderName = senderName;
        replyToPreview = preview;
        inputArea.forceActiveFocus();
    }
    function cancelReplying() {
        replyToEventId = "";
        replyToSenderName = "";
        replyToPreview = "";
    }

    // @mention state. Each selection from the autocomplete pushes one
    // entry; on send, we scan inputArea.text for occurrences of each
    // token to build formatted_body + m.mentions. Entries whose token was
    // manually deleted from the composer simply drop out of the scan.
    //
    // Tokens are `@<display-name>` with internal whitespace stripped so the
    // string survives copy-paste and search-and-replace. A trailing space
    // is inserted after the token at selection time so the next character
    // the user types won't re-trigger the autocomplete popup.
    property var mentionTokens: []
    // Query state for the autocomplete popup. Empty query means the popup
    // is closed; a non-empty string shows the matching-member list.
    property string mentionQuery: ""
    // Character offset in inputArea.text where the `@` of the active query
    // sits. Used to splice the chosen name back into the text.
    property int mentionAnchor: -1
    // Index of the currently-highlighted autocomplete match, bumped by
    // arrow keys and committed by Tab / Enter.
    property int mentionSelected: 0

    function _stripToToken(displayName) {
        // Mention tokens must be whitespace-free so our regex-based scan on
        // send survives round-trips through copy/paste and editing.
        return displayName.replace(/\s+/g, "");
    }

    function _mentionMembers() {
        // Filtered member list driving the popup. Returns an array of
        // { userId, displayName, tokenName } suitable for the Repeater.
        if (!serverManager.activeServer) return [];
        var mm = serverManager.activeServer.memberListModel;
        if (!mm) return [];
        var q = mentionQuery.toLowerCase();
        var out = [];
        // Rely on memberListModel.roleNames() mapping to `userId`/`displayName`.
        for (var i = 0; i < mm.rowCount(); i++) {
            var idx = mm.index(i, 0);
            var uid = mm.data(idx, Qt.UserRole + 1); // UserIdRole
            var dn = mm.data(idx, Qt.UserRole + 2);  // DisplayNameRole
            var token = _stripToToken(dn || uid);
            if (!q || token.toLowerCase().indexOf(q) >= 0
                   || (dn && dn.toLowerCase().indexOf(q) >= 0)) {
                out.push({ userId: uid, displayName: dn || uid,
                           tokenName: token });
            }
            if (out.length >= 8) break; // cap popup height
        }
        return out;
    }

    function _insertMention(member) {
        // Replace `@<query>` (from mentionAnchor..cursor) with
        // `@<tokenName> ` and record the mapping so sendCurrentMessage can
        // rebuild formatted_body + m.mentions.
        if (mentionAnchor < 0) return;
        var before = inputArea.text.substring(0, mentionAnchor);
        var after = inputArea.text.substring(inputArea.cursorPosition);
        var inserted = "@" + member.tokenName + " ";
        inputArea.text = before + inserted + after;
        inputArea.cursorPosition = (before + inserted).length;
        // De-dup on token+userId so the same mention picked twice produces
        // one entry in m.mentions.user_ids.
        var existing = mentionTokens.find(function(t) {
            return t.token === inserted.trim() && t.userId === member.userId;
        });
        if (!existing) {
            mentionTokens = mentionTokens.concat([{
                token: inserted.trim(),      // "@tokenName"
                userId: member.userId,
                displayName: member.displayName
            }]);
        }
        mentionQuery = "";
        mentionAnchor = -1;
    }

    // Refresh mentionQuery/mentionAnchor from the current cursor position.
    // Called from TextArea.onTextChanged / onCursorPositionChanged.
    function _refreshMentionQuery() {
        var text = inputArea.text;
        var cp = inputArea.cursorPosition;
        // Walk backwards from the cursor until whitespace or start; if we
        // hit an `@`, the substring between is the live query.
        var i = cp - 1;
        while (i >= 0) {
            var ch = text.charAt(i);
            if (ch === "@") {
                // `@` must be at start of line or follow whitespace to
                // count as the start of a mention (so emails don't trigger).
                if (i === 0 || /\s/.test(text.charAt(i - 1))) {
                    mentionAnchor = i;
                    mentionQuery = text.substring(i + 1, cp);
                    mentionSelected = 0;
                    return;
                }
                break;
            }
            if (/\s/.test(ch)) break;
            i--;
        }
        mentionQuery = "";
        mentionAnchor = -1;
    }

    // Permission-derived UX state. Using permissionsGeneration as a real
    // dependency (integer, read and compared) makes these bindings reactive
    // across QML's AOT-compiled path; the bare `serverRoles` touch I tried
    // earlier got dead-code-eliminated.
    property int _permGen: serverManager.activeServer ? serverManager.activeServer.permissionsGeneration : 0
    property bool canSend: {
        if (!serverManager.activeServer) return true;
        return _permGen >= 0 && serverManager.activeServer.canSend(activeRoomId);
    }
    property bool canAttach: {
        if (!serverManager.activeServer) return true;
        return _permGen >= 0 && serverManager.activeServer.canAttach(activeRoomId);
    }
    property int slowmodeSeconds: {
        if (!serverManager.activeServer) return 0;
        return _permGen >= 0 ? serverManager.activeServer.channelSlowmode(activeRoomId) : 0;
    }
    // Client-side slowmode tracker. Server is still authoritative.
    property double lastSentAt: 0
    property int _slowmodeTick: 0 // bumped by the timer to force re-eval
    readonly property int slowmodeRemaining: {
        _slowmodeTick; // dependency
        if (slowmodeSeconds <= 0 || lastSentAt === 0) return 0;
        var elapsed = (Date.now() - lastSentAt) / 1000;
        var left = slowmodeSeconds - elapsed;
        return left > 0 ? Math.ceil(left) : 0;
    }

    Timer {
        running: inputRoot.slowmodeSeconds > 0 && inputRoot.lastSentAt > 0
        interval: 500
        repeat: true
        onTriggered: inputRoot._slowmodeTick++
    }

    // Banner shown above the composer while editing an existing message.
    // 32h, quieter bg0 strip with a small SVG edit icon on the left, the
    // prompt text, and a proper X button (not a bare unicode glyph).
    Rectangle {
        id: editingHeader
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: visible ? 32 : 0
        color: Theme.bg0
        radius: Theme.r2
        visible: inputRoot.editingEventId !== ""
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.sp.s4
            anchors.rightMargin: Theme.sp.s2
            spacing: Theme.sp.s3

            Icon { name: "edit"; size: 12; color: Theme.fg2 }

            Text {
                text: "Editing message"
                color: Theme.fg1
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                font.weight: Theme.fontWeight.semibold
            }
            Text {
                text: "— press Esc to cancel"
                color: Theme.fg3
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            Rectangle {
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                radius: Theme.r1
                color: cancelMouse.containsMouse ? Theme.bg3 : "transparent"
                Icon {
                    anchors.centerIn: parent
                    name: "x"
                    size: 12
                    color: cancelMouse.containsMouse ? Theme.fg0 : Theme.fg2
                }
                MouseArea {
                    id: cancelMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: inputRoot.cancelEditing()
                }
            }
        }
    }

    // Upload progress strip — stacks rows while files are uploading.
    // Each row is a filename + a thin accent-tinted progress bar.
    Rectangle {
        id: uploadBanner
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: inputRoot._uploadKeys.length > 0
            ? 8 + inputRoot._uploadKeys.length * 22 : 0
        visible: height > 0
        color: Theme.bg0
        radius: Theme.r2
        Behavior on height { NumberAnimation { duration: Theme.motion.fastMs } }

        Column {
            anchors.fill: parent
            anchors.leftMargin: Theme.sp.s4
            anchors.rightMargin: Theme.sp.s4
            anchors.topMargin: 4
            anchors.bottomMargin: 4
            spacing: 2
            Repeater {
                model: inputRoot._uploadKeys
                delegate: Item {
                    required property string modelData
                    width: parent.width
                    height: 18
                    readonly property real p: inputRoot._uploads[modelData] || 0
                    Text {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width * 0.5
                        text: "Uploading " + modelData
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.fontSize.xs
                        color: Theme.fg2
                        elide: Text.ElideMiddle
                    }
                    Rectangle {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width * 0.45
                        height: 4
                        radius: 2
                        color: Theme.bg2
                        Rectangle {
                            width: parent.width * p
                            height: parent.height
                            radius: parent.radius
                            color: p >= 1.0 ? Theme.online : Theme.accent
                            Behavior on width { NumberAnimation { duration: 100 } }
                        }
                    }
                }
            }
        }
    }

    // Banner for replies. Same slot as editingHeader; mutually exclusive.
    // Gets a curved-arrow reply icon + accent sender name + quiet preview.
    Rectangle {
        id: replyHeader
        anchors.top: uploadBanner.visible ? uploadBanner.bottom : parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: visible ? 32 : 0
        color: Theme.bg0
        radius: Theme.r2
        visible: inputRoot.replyToEventId !== "" && !editingHeader.visible
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.sp.s4
            anchors.rightMargin: Theme.sp.s2
            spacing: Theme.sp.s3

            Icon { name: "reply"; size: 12; color: Theme.accent }

            Text {
                text: "Replying to"
                color: Theme.fg2
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
            }
            Text {
                text: inputRoot.replyToSenderName !== ""
                      ? inputRoot.replyToSenderName : "unknown"
                color: Theme.accent
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                font.weight: Theme.fontWeight.semibold
            }
            Text {
                text: inputRoot.replyToPreview
                color: Theme.fg3
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Rectangle {
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                radius: Theme.r1
                color: replyCancelMouse.containsMouse ? Theme.bg3 : "transparent"
                Icon {
                    anchors.centerIn: parent
                    name: "x"
                    size: 12
                    color: replyCancelMouse.containsMouse ? Theme.fg0 : Theme.fg2
                }
                MouseArea {
                    id: replyCancelMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: inputRoot.cancelReplying()
                }
            }
        }
    }

    RowLayout {
        id: inputCore
        anchors.top: editingHeader.visible
                     ? editingHeader.bottom
                     : (replyHeader.visible ? replyHeader.bottom : parent.top)
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.topMargin: (editingHeader.visible || replyHeader.visible) ? 4 : 0
        anchors.leftMargin: Theme.sp.s3
        anchors.rightMargin: Theme.sp.s3
        spacing: Theme.sp.s1

        // Attachment button — hidden if user lacks ATTACH_FILES.
        Rectangle {
            Layout.preferredWidth: Theme.isMobile ? 40 : 28
            Layout.preferredHeight: Theme.isMobile ? 40 : 28
            Layout.alignment: Qt.AlignVCenter
            radius: Theme.r1
            color: attachHover.containsMouse ? Theme.bg2 : "transparent"
            opacity: inputRoot.uploading ? 0.4 : 1.0
            visible: inputRoot.canAttach

            Icon {
                anchors.centerIn: parent
                name: "paperclip"
                size: Theme.isMobile ? 20 : 16
                color: attachHover.containsMouse ? Theme.fg0 : Theme.fg2
            }

            MouseArea {
                id: attachHover
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                enabled: !inputRoot.uploading
                onClicked: fileDialog.open()
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            TextArea {
                id: inputArea
                placeholderText: {
                    if (!inputRoot.canSend) return "You don't have permission to send here";
                    if (inputRoot.slowmodeRemaining > 0)
                        return "Slowmode — " + inputRoot.slowmodeRemaining + "s";
                    if (inputRoot.uploading) return "Uploading…";
                    return "Message #" + inputRoot.roomName;
                }
                placeholderTextColor: Theme.fg3
                color: Theme.fg0
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.base
                wrapMode: TextEdit.Wrap
                background: null
                selectByMouse: true
                verticalAlignment: TextEdit.AlignVCenter
                topPadding: 8
                bottomPadding: 8
                enabled: !inputRoot.uploading && inputRoot.canSend && inputRoot.slowmodeRemaining === 0

                onTextChanged: {
                    if (serverManager.activeServer && text.trim().length > 0) {
                        serverManager.activeServer.sendTypingNotification();
                    }
                    inputRoot._refreshMentionQuery();
                }
                onCursorPositionChanged: inputRoot._refreshMentionQuery()

                Keys.onPressed: (event) => {
                    // Intercept nav keys while the mention popup is open so
                    // arrow/tab/enter drive the popup instead of moving the
                    // caret or submitting the message.
                    if (inputRoot.mentionAnchor >= 0) {
                        var members = inputRoot._mentionMembers();
                        if (event.key === Qt.Key_Down) {
                            if (members.length > 0) {
                                inputRoot.mentionSelected =
                                    (inputRoot.mentionSelected + 1) % members.length;
                            }
                            event.accepted = true;
                            return;
                        }
                        if (event.key === Qt.Key_Up) {
                            if (members.length > 0) {
                                inputRoot.mentionSelected =
                                    (inputRoot.mentionSelected - 1 + members.length) % members.length;
                            }
                            event.accepted = true;
                            return;
                        }
                        if (event.key === Qt.Key_Tab
                            || (event.key === Qt.Key_Return
                                && !(event.modifiers & Qt.ShiftModifier))) {
                            if (members.length > 0) {
                                inputRoot._insertMention(
                                    members[inputRoot.mentionSelected]);
                            } else {
                                // No match — close the popup so Enter can
                                // submit on the next keypress.
                                inputRoot.mentionQuery = "";
                                inputRoot.mentionAnchor = -1;
                            }
                            event.accepted = true;
                            return;
                        }
                        if (event.key === Qt.Key_Escape) {
                            inputRoot.mentionQuery = "";
                            inputRoot.mentionAnchor = -1;
                            event.accepted = true;
                            return;
                        }
                    }
                }
                Keys.onReturnPressed: (event) => {
                    // Touch keyboards don't distinguish Shift+Enter
                    // reliably and users expect Enter to insert a
                    // newline the way SMS / WhatsApp do. On mobile,
                    // always allow newline — send via the button.
                    if (Theme.isMobile || (event.modifiers & Qt.ShiftModifier)) {
                        event.accepted = false;
                    } else {
                        sendCurrentMessage();
                        event.accepted = true;
                    }
                }
                Keys.onEscapePressed: (event) => {
                    if (inputRoot.editingEventId !== "") {
                        inputRoot.cancelEditing();
                        event.accepted = true;
                    } else if (inputRoot.replyToEventId !== "") {
                        inputRoot.cancelReplying();
                        event.accepted = true;
                    }
                }
            }
        }

        // Emoji button
        Rectangle {
            Layout.preferredWidth: Theme.isMobile ? 40 : 28
            Layout.preferredHeight: Theme.isMobile ? 40 : 28
            Layout.alignment: Qt.AlignVCenter
            radius: Theme.r1
            color: emojiHover.containsMouse || emojiPopup.visible ? Theme.bg2 : "transparent"

            Icon {
                anchors.centerIn: parent
                name: "smile"
                size: Theme.isMobile ? 20 : 16
                color: emojiHover.containsMouse || emojiPopup.visible
                       ? Theme.fg0 : Theme.fg2
            }

            MouseArea {
                id: emojiHover
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (emojiPopup.visible) {
                        emojiPopup.close();
                    } else {
                        emojiPopup.open();
                    }
                }
            }

            EmojiPicker {
                id: emojiPopup
                y: -height - Theme.sp.s1
                x: -width + 28

                onEmojiSelected: function(emoji) {
                    inputArea.insert(inputArea.cursorPosition, emoji);
                    emojiPopup.close();
                    inputArea.forceActiveFocus();
                }
            }
        }

        // Upload progress — three pulsing accent dots (matches the
        // typing indicator's vocabulary so uploading "looks like" the
        // app's other async-work affordances).
        Row {
            Layout.alignment: Qt.AlignVCenter
            spacing: 3
            visible: inputRoot.uploading
            Repeater {
                model: 3
                delegate: Rectangle {
                    required property int index
                    width: 5; height: 5; radius: 2.5
                    color: Theme.accent
                    opacity: 0.35
                    SequentialAnimation on opacity {
                        loops: Animation.Infinite
                        running: inputRoot.uploading
                        PauseAnimation { duration: index * 140 }
                        NumberAnimation { to: 1.0; duration: 280 }
                        NumberAnimation { to: 0.35; duration: 280 }
                        PauseAnimation { duration: (2 - index) * 140 }
                    }
                }
            }
        }

        // Send button — accent-filled once the composer has something to
        // send. SPEC §3.6 calls for it to show only when input is
        // non-empty. We fade+scale the button in instead of toggling a
        // visibility flag so the right side of the composer doesn't
        // pop-reflow on every key press.
        Rectangle {
            id: sendBtn
            // 44 px on mobile to meet Apple / Material touch-target
            // guidelines. Desktop stays compact since it's driven by
            // Enter in most cases anyway.
            Layout.preferredWidth: Theme.isMobile ? 44 : 28
            Layout.preferredHeight: Theme.isMobile ? 44 : 28
            Layout.alignment: Qt.AlignVCenter
            radius: Theme.r1
            // Screen readers: a "Send" button at all times — the
            // visual disabled state already covers empty composers.
            Accessible.role: Accessible.Button
            Accessible.name: "Send message"
            Accessible.description: sendBtn.armed
                ? "Send the typed message"
                : "Nothing to send yet"
            Accessible.onPressAction: if (sendBtn.armed) sendCurrentMessage()
            // `armed` must include preeditText so Android IMEs (which
            // keep keystrokes in preedit until a commit char like space
            // or newline) arm the send button on the first character,
            // not just after the user types a newline.
            readonly property bool armed:
                (inputArea.text.trim().length > 0
                 || inputArea.preeditText.length > 0)
                && !inputRoot.uploading
            color: sendMouse.containsMouse && armed
                ? Theme.accentDim : Theme.accent
            Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }
            opacity: armed ? 1.0 : 0.0
            scale:   armed ? 1.0 : 0.8
            Behavior on opacity { NumberAnimation { duration: Theme.motion.fastMs } }
            Behavior on scale {
                NumberAnimation { duration: Theme.motion.fastMs
                                  easing.type: Easing.BezierSpline
                                  easing.bezierCurve: Theme.motion.bezier }
            }

            Icon {
                anchors.centerIn: parent
                name: "send"
                size: 14
                color: Theme.onAccent
            }

            MouseArea {
                id: sendMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: sendBtn.armed ? Qt.PointingHandCursor : Qt.ArrowCursor
                enabled: sendBtn.armed
                onClicked: sendCurrentMessage()
            }
        }
    }

    // @mention autocomplete popup. Shown whenever mentionAnchor >= 0,
    // positioned just above the composer. The list is rebuilt each time
    // `mentionQuery` changes (cheap: bounded to 8 entries by _mentionMembers).
    Popup {
        id: mentionPopup
        parent: inputRoot
        y: -height - 4
        x: Theme.sp.s3
        width: 280
        padding: 4
        modal: false
        focus: false
        closePolicy: Popup.NoAutoClose
        visible: inputRoot.mentionAnchor >= 0 && mentionListModel.length > 0

        // Re-read on every change — cheap, bounded, and avoids stale state
        // after the user deletes a character and re-triggers the popup.
        property var mentionListModel: inputRoot.mentionAnchor >= 0
                                        ? inputRoot._mentionMembers() : []

        background: Rectangle {
            color: Theme.bg1
            border.color: Theme.line
            border.width: 1
            radius: Theme.r2
        }

        contentItem: Column {
            spacing: 0

            // Popup header label — gives context so the list doesn't feel
            // like a stray menu.
            Text {
                leftPadding: Theme.sp.s3
                topPadding: Theme.sp.s2
                bottomPadding: Theme.sp.s2
                text: "MATCHING MEMBERS"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWidest.xs
                color: Theme.fg3
            }

            Repeater {
                model: mentionPopup.mentionListModel
                delegate: Rectangle {
                    required property int index
                    required property var modelData
                    width: mentionPopup.width - 8
                    height: 32
                    radius: Theme.r1
                    color: (index === inputRoot.mentionSelected)
                           ? Theme.accent
                           : (memberHover.containsMouse ? Theme.bg3 : "transparent")
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.sp.s3
                        anchors.rightMargin: Theme.sp.s3
                        spacing: Theme.sp.s3

                        // Tiny avatar initial so the row reads faster.
                        Rectangle {
                            Layout.preferredWidth: 20
                            Layout.preferredHeight: 20
                            radius: Theme.r1
                            color: index === inputRoot.mentionSelected
                                   ? Qt.rgba(0, 0, 0, 0.15)
                                   : Theme.senderColor(modelData.userId || modelData.tokenName)
                            Text {
                                anchors.centerIn: parent
                                text: {
                                    var n = modelData.displayName || modelData.tokenName;
                                    var s = n.replace(/^[^a-zA-Z0-9]+/, "");
                                    return (s.length > 0 ? s.charAt(0) : "?").toUpperCase();
                                }
                                font.family: Theme.fontSans
                                font.pixelSize: 11
                                font.weight: Theme.fontWeight.semibold
                                color: Theme.onAccent
                            }
                        }

                        Text {
                            text: modelData.displayName
                            color: index === inputRoot.mentionSelected
                                   ? Theme.onAccent : Theme.fg0
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.md
                            font.weight: Theme.fontWeight.medium
                            elide: Text.ElideRight
                        }
                        Text {
                            text: "@" + modelData.tokenName
                            color: index === inputRoot.mentionSelected
                                   ? Qt.rgba(0, 0, 0, 0.5) : Theme.fg3
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fontSize.xs
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignRight
                            elide: Text.ElideRight
                        }
                    }
                    MouseArea {
                        id: memberHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            inputRoot._insertMention(modelData);
                            inputArea.forceActiveFocus();
                        }
                    }
                }
            }
        }
    }

    Platform.FileDialog {
        id: fileDialog
        title: "Select a file to upload"
        nameFilters: ["All files (*)"]
        onAccepted: {
            if (serverManager.activeServer) {
                serverManager.activeServer.sendMediaMessage(fileDialog.file.toString());
                inputRoot.uploading = true;
            }
        }
    }

    // Listen for upload completion to reset uploading state
    Connections {
        target: serverManager.activeServer ? serverManager.activeServer : null

        function onMediaSendCompleted() {
            inputRoot.uploading = false;
        }

        function onMediaSendFailed(error) {
            inputRoot.uploading = false;
            console.warn("Media upload failed:", error);
        }
    }

    // Transform /me, /shrug, /tableflip, /unflip into the right message
    // shape or body. /me sends an m.emote; the rest are just text macros.
    // Returns { body, msgtype } or null if not a slash-command.
    function _runSlashCommand(raw) {
        if (!raw.startsWith("/")) return null;
        var space = raw.indexOf(" ");
        var cmd = (space < 0 ? raw : raw.substring(0, space)).toLowerCase();
        var rest = space < 0 ? "" : raw.substring(space + 1);
        switch (cmd) {
        case "/me":
            return { body: rest, msgtype: "m.emote" };
        case "/shrug":
            return { body: rest + (rest ? " " : "") + "¯\\_(ツ)_/¯",
                     msgtype: "m.text" };
        case "/tableflip":
            return { body: rest + (rest ? " " : "") + "(╯°□°)╯︵ ┻━┻",
                     msgtype: "m.text" };
        case "/unflip":
            return { body: rest + (rest ? " " : "") + "┬─┬ノ( º _ ºノ)",
                     msgtype: "m.text" };
        case "/lenny":
            return { body: rest + (rest ? " " : "") + "( ͡° ͜ʖ ͡°)",
                     msgtype: "m.text" };
        }
        return null;
    }

    // Expand markdown-lite formatting into HTML for the formatted_body
    // field. Intentionally minimal — no full CommonMark — just the
    // four things chat users actually want: **bold**, *italic*, `code`,
    // and ```code blocks```. Input is already HTML-escaped before we
    // run this, so we swap markers rather than inject tags.
    function _markdownToHtml(escaped) {
        var out = escaped;
        // Fenced code blocks first so their contents don't get
        // re-interpreted as inline markdown.
        out = out.replace(/```([\s\S]*?)```/g, function(_, code) {
            return "<pre><code>" + code + "</code></pre>";
        });
        out = out.replace(/`([^`\n]+)`/g, "<code>$1</code>");
        out = out.replace(/\*\*([^*\n]+)\*\*/g, "<b>$1</b>");
        // Single-asterisk italics — avoid matching leftovers from the
        // **bold** pass by requiring a non-asterisk neighbour.
        out = out.replace(/(^|[^*])\*([^*\n]+)\*(?!\*)/g, "$1<i>$2</i>");
        // __underline__ (rare but convenient)
        out = out.replace(/__([^_\n]+)__/g, "<u>$1</u>");
        return out;
    }
    // Detect whether the body carries any of the markdown markers so
    // we only pay the HTML formatted_body cost when useful.
    function _hasMarkdown(text) {
        return /`|\*\*|(?:^|[^*])\*[^*\n]/.test(text) || text.indexOf("__") >= 0;
    }

    function sendCurrentMessage() {
        // Commit any in-flight IME composition first so what the user
        // sees (preedit text) becomes actual text before we read it.
        // Without this, Android sending via button tap while mid-
        // composition would drop the typed-but-uncommitted chars.
        if (inputArea.preeditText && inputArea.preeditText.length > 0)
            Qt.inputMethod.commit();
        var text = inputArea.text.trim();
        if (text.length === 0) return;
        if (!serverManager.activeServer) return;

        // Slash-command intercept — takes priority over replies/edits
        // since "/me fixed typo" inside a reply context would be a
        // weird thing to commit to.
        if (inputRoot.editingEventId === "" && inputRoot.replyToEventId === "") {
            var cmd = _runSlashCommand(text);
            if (cmd) {
                if (cmd.msgtype === "m.emote") {
                    serverManager.activeServer.sendEmote(cmd.body);
                } else {
                    serverManager.activeServer.sendMessage(cmd.body);
                }
                inputArea.text = "";
                inputRoot.lastSentAt = Date.now();
                return;
            }
        }

        if (inputRoot.editingEventId !== "") {
            // Edit path — server still gates on sender match. Don't apply
            // slowmode / canSend checks to edits; they're intentionally
            // allowed during cooldown (you're refining, not flooding).
            if (text === inputRoot.editingOriginalBody) {
                inputRoot.cancelEditing();
                return;
            }
            serverManager.activeServer.editMessage(inputRoot.editingEventId, text);
            inputRoot.cancelEditing();
            return;
        }

        if (!inputRoot.canSend || inputRoot.slowmodeRemaining > 0) return;

        if (inputRoot.replyToEventId !== "") {
            // Reply path — send as a m.in_reply_to-bearing message, then
            // clear both the composer and the reply banner.
            serverManager.activeServer.replyToMessage(inputRoot.replyToEventId, text);
            inputArea.text = "";
            inputRoot.cancelReplying();
            inputRoot.lastSentAt = Date.now();
            return;
        }

        // Scan the tracked mention tokens against the composer text; the
        // ones still present (user didn't delete them mid-typing) get
        // rewritten into HTML anchors + added to m.mentions.user_ids.
        var activeMentions = [];
        for (var i = 0; i < mentionTokens.length; i++) {
            var t = mentionTokens[i];
            if (text.indexOf(t.token) >= 0) activeMentions.push(t);
        }

        var hasMarkdown = _hasMarkdown(text);
        if (activeMentions.length > 0 || hasMarkdown) {
            // Escape HTML first so no user-typed `<` creates a spurious tag,
            // then swap each tracked token for a proper anchor + expand
            // markdown. Order matters: mentions replace EXACT tokens, which
            // are plain text with no markdown characters, so markdown
            // expansion after mention-swap is safe.
            var html = text.replace(/&/g, "&amp;")
                           .replace(/</g, "&lt;")
                           .replace(/>/g, "&gt;");
            if (hasMarkdown) html = _markdownToHtml(html);
            html = html.replace(/\n/g, "<br>");
            var uids = [];
            for (var j = 0; j < activeMentions.length; j++) {
                var m = activeMentions[j];
                var encId = encodeURIComponent(m.userId);
                var anchor = '<a href="bsfchat://user/' + encId
                             + '" style="color:' + Theme.accent
                             + '; text-decoration:none; font-weight:bold;">'
                             + m.token + '</a>';
                html = html.split(m.token).join(anchor);
                if (uids.indexOf(m.userId) < 0) uids.push(m.userId);
            }
            serverManager.activeServer.sendRichMessage(text, html, uids);
        } else {
            serverManager.activeServer.sendMessage(text);
        }
        inputArea.text = "";
        inputRoot.mentionTokens = [];
        inputRoot.mentionQuery = "";
        inputRoot.mentionAnchor = -1;
        inputRoot.lastSentAt = Date.now();
    }
}
