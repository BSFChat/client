import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.platform as Platform
import BSFChat
import "../js/UploadTally.js" as UploadTally

// MessageInput is an implicitHeight-driven Rectangle so it can grow the
// vertical banner strip (when editing) without forcing the parent to
// re-layout around a fixed height.
// Composer (SPEC §3.6 bottom, 56h minimum, auto-grows to ~120h).
// r3 rounded container, bg2 surface, line border on focus.
Rectangle {
    id: inputRoot
    color: Theme.bg1
    border.color: inputRoot.overLimit
        ? Theme.danger
        : (inputArea.activeFocus ? Theme.accent : Theme.line)
    border.width: 1
    radius: Theme.r3
    // Every banner currently taking a slice off the top of the composer.
    // Editing and replying are mutually exclusive; the size warning stacks
    // under whichever of them is up, because you can be editing a message
    // into being too long.
    readonly property real _bannerHeight:
        (editingHeader.visible ? editingHeader.height
                               : (replyHeader.visible ? replyHeader.height : 0))
        + sizeHeader.height
    implicitHeight: inputRoot._bannerHeight > 0
        ? inputRoot._bannerHeight + inputCore.implicitHeight + 8
        : inputCore.implicitHeight
    height: implicitHeight

    property string roomName: ""
    property string activeRoomId: serverManager.activeServer ? serverManager.activeServer.activeRoomId : ""

    // ── How long a message may be ────────────────────────────────────
    //
    // There was no cap here at all, and none on the server either, so the
    // composer would cheerfully accept a pasted logfile, send it, and have it
    // land in everyone's timeline. The server now refuses an oversize body
    // with 413 M_TOO_LARGE — this is the half that tells the user BEFORE they
    // press Enter, which is the only point at which the information is any use
    // to them.
    //
    // Nothing is ever truncated. A composer that silently drops the tail of a
    // paste is worse than one that refuses it: the user sends what looks like
    // a complete message and only finds out later, if ever. The send is
    // blocked, the border goes red, and the banner says by how much.
    //
    // BYTES, not `text.length`. QML's length is UTF-16 code units, which is
    // neither what the user sees nor what the server counts; see
    // ServerConnection::messageByteLength. preeditText is included because an
    // IME's uncommitted composition is about to become text — the same reason
    // the send button's `armed` reads it.
    readonly property int maxBodyBytes: serverManager.activeServer
        ? serverManager.activeServer.maxMessageBytes : 0
    readonly property int bodyBytes: {
        var s = serverManager.activeServer;
        if (!s) return 0;
        return s.messageByteLength(
            (inputArea.text + inputArea.preeditText).trim());
    }
    readonly property bool overLimit:
        inputRoot.maxBodyBytes > 0 && inputRoot.bodyBytes > inputRoot.maxBodyBytes
    // The counter appears only in the last tenth of the budget. Shown always,
    // it is noise on every message anyone actually sends; shown only once the
    // limit is already breached, the user has no warning that they are getting
    // close to it mid-paste.
    readonly property bool nearLimit:
        inputRoot.maxBodyBytes > 0
        && inputRoot.bodyBytes > inputRoot.maxBodyBytes * 0.9

    // U-H6. `uploading` used to be a plain bool: set true when an upload
    // started, set false by the FIRST mediaSendCompleted. With N files in
    // flight the composer unlocked after one of them finished, and a
    // server switch mid-upload re-targeted the Connections below so the
    // completion never arrived at all and the composer stayed locked for
    // the rest of the session. It is now derived from the count of
    // uploads this composer has actually started and not yet seen finish,
    // and that count is reset outright on a room / server change.
    //
    // The arithmetic itself lives in js/UploadTally.js so it can be tested
    // without instantiating this component (which imports BSFChat and so
    // cannot be loaded from a test binary) — read that file's header for the
    // two invariants a bare-signal tally depends on and the three times they
    // have been broken. Everything below is the wiring.
    property var _tally: UploadTally.empty()
    readonly property bool uploading: inputRoot._tally.inFlight > 0
    // Diagnostic, not UI: how many terminal signals arrived that this composer
    // could account for neither as its own upload nor as an orphan it had
    // already written off. Above zero means somebody's wiring is wrong.
    readonly property int unmatchedUploadReports: inputRoot._tally.unmatched

    // Called by every site that kicks off an upload — the attach button,
    // paste, MessageView's drop area, and the mobile shell's Android
    // share-intent handler. All of them call it AFTER sendMediaMessage, which
    // is only safe because that function never reports a failure before it
    // returns; the invariant is stated and held in
    // ServerConnection::emitPreflightMediaFailure. A pre-flight failure that
    // arrived synchronously used to decrement this count before it had been
    // incremented, and the clamp then made the loss silent: the composer sat
    // disabled reading "Uploading…" with nothing in flight, recoverable only
    // by leaving the channel and coming back.
    //
    // tests/test_qml_hygiene.cpp pins the pairing: a QML file that calls
    // sendMediaMessage and does not call this is the shape of the fourth bug
    // on this counter, and it is a source scan rather than a convention
    // because the first three were all "somebody did not know the rule".
    function noteUploadStarted() {
        inputRoot._tally = UploadTally.started(inputRoot._tally);
    }
    function _noteUploadFinished() {
        var next = UploadTally.finished(inputRoot._tally);
        if (UploadTally.wasUnmatched(inputRoot._tally, next)) {
            // Still clamped — a negative count would disable the composer by
            // arithmetic later, which is a worse failure than the one being
            // reported. But say so. A clamp that absorbs an unmatched
            // decrement without a word is why the pre-flight ordering bug
            // presented as a stuck message box rather than as something
            // anybody could see going wrong.
            console.warn("MessageInput: upload finished with none in flight —"
                         + " an upload was counted late, or a signal arrived"
                         + " for an upload this composer never started"
                         + " (unmatched so far: " + next.unmatched + ")");
        }
        inputRoot._tally = next;
    }
    // `connectionChanged` decides whether the uploads being written off here
    // can still be expected to report back. See UploadTally.contextChanged:
    // on a room switch they can and are credited, on a server switch their
    // signals go to a connection this composer is no longer listening to and
    // the credit would only serve to silence a later, real bug.
    function _resetUploads(connectionChanged) {
        inputRoot._tally = UploadTally.contextChanged(inputRoot._tally,
                                                      connectionChanged);
        inputRoot._uploads = ({});
        uploadSweepTimer.stop();
    }

    // ── Per-room composer state (U-H3) ───────────────────────────────
    //
    // The edit target, the reply target and the mention tokens all name
    // something in ONE room, and all three used to survive a channel
    // switch: "edit a message in #general, click #random, press Enter"
    // sent an m.replace for a #general event into #random, and the server
    // accepted it. The composer text survived too, with nowhere to put a
    // per-channel draft, so a half-typed message silently followed the
    // user into whatever channel they looked at next.
    //
    // Keyed on (server, room) rather than the room id alone: a server
    // switch does not change any connection's activeRoomId (see the note
    // on roomContextKey in MessageView.qml), so keying on the room id
    // would miss it — and two servers can host the same room id.
    readonly property string roomKey: {
        var s = serverManager.activeServer;
        if (!s) return "";
        return s.serverUrl + "\u001f" + s.activeRoomId;   // U+001F: in neither half
    }
    property var _drafts: ({})
    property string _lastRoomKey: ""

    // The connection itself, not its URL. roomKey embeds the URL and would
    // catch a server switch in every case anyone can currently produce, but
    // what the upload tally actually needs to know is whether the OBJECT the
    // Connections blocks below are attached to has been swapped — that is what
    // decides whether an in-flight upload can still report back. Two live
    // connections to the same URL in the same room would give the same
    // roomKey, and relying on that not happening is the kind of implicit
    // coupling this counter has already been bitten by twice.
    readonly property var uploadConnection: serverManager.activeServer
    property var _lastConnection: null

    onRoomKeyChanged: inputRoot._swapRoomState()
    onUploadConnectionChanged: inputRoot._swapRoomState()
    Component.onCompleted: {
        inputRoot._lastRoomKey = inputRoot.roomKey;
        inputRoot._lastConnection = inputRoot.uploadConnection;
    }

    function _swapRoomState() {
        // Both handlers above fire on a server switch, in an order QML does
        // not define. Whichever runs first does the swap; the second finds
        // nothing left to do and returns.
        var connChanged = (inputRoot.uploadConnection !== inputRoot._lastConnection);
        inputRoot._lastConnection = inputRoot.uploadConnection;

        var prev = inputRoot._lastRoomKey;
        if (prev === inputRoot.roomKey && !connChanged) return;

        // Stash the outgoing room's draft. An in-progress EDIT is not a
        // draft — its text belongs to a message that already exists —
        // so it is dropped rather than saved under the room key.
        if (prev !== "") {
            var d = {};
            for (var k in inputRoot._drafts) d[k] = inputRoot._drafts[k];
            if (inputRoot.editingEventId === "" && inputArea.text.length > 0) {
                d[prev] = { text: inputArea.text, tokens: inputRoot.mentionTokens };
            } else {
                delete d[prev];
            }
            inputRoot._drafts = d;
        }
        inputRoot._lastRoomKey = inputRoot.roomKey;

        // Nothing that named something in the old room may survive.
        inputRoot.editingEventId = "";
        inputRoot.editingOriginalBody = "";
        inputRoot.replyToEventId = "";
        inputRoot.replyToSenderName = "";
        inputRoot.replyToPreview = "";
        inputRoot._clearMentionState();
        inputRoot.slashQuery = "";
        inputRoot.slashAnchor = -1;
        // Slowmode is per-channel; carrying the send timestamp over locks
        // the composer in a channel the user has not posted in.
        inputRoot.lastSentAt = 0;
        // In-flight uploads were started against the room we just left, so
        // they must not keep the composer locked here. Whether their terminal
        // signal can still reach this composer depends on whether the
        // connection changed too — see _resetUploads.
        inputRoot._resetUploads(connChanged);

        var next = inputRoot._drafts[inputRoot.roomKey];
        inputArea.text = (next && next.text) ? next.text : "";
        if (next && next.tokens) inputRoot.mentionTokens = next.tokens;
        inputArea.cursorPosition = inputArea.text.length;
    }

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
        // Drop tokens tracked against the draft we just overwrote — they name
        // nothing in the text now in the composer, and a stale "@room" whose
        // word-boundary test happened to match the edited body would attach a
        // room-wide ping (which the server gates on MENTION_EVERYONE, so it
        // would fail the edit outright). The original message's own mentions are
        // preserved on the C++ side, not re-derived here: see
        // ServerConnection::editMessage.
        inputRoot._clearMentionState();
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
        // @room broadcasts to everyone in the channel. Offered first so it is
        // reachable in one keystroke, and marked isRoom so the send path emits
        // m.mentions.room instead of putting "@room" in user_ids.
        if ("room".indexOf(q) === 0) {
            out.push({ userId: "@room", displayName: "@room",
                       tokenName: "room", isRoom: true,
                       subtitle: "Notify everyone in this channel" });
        }
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
                displayName: member.displayName,
                isRoom: member.isRoom === true
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

    // Over-length warning. Stacks under the editing / reply banner rather than
    // replacing it: "you are editing a message and it is now too long" is a
    // real state and the user needs both halves of it.
    //
    // A banner and not a toast, because the condition persists — it is true
    // for as long as the text is, and it goes away by itself when the user
    // deletes enough. A toast would fire once, be missed, and leave a composer
    // that refuses to send for no visible reason.
    Rectangle {
        id: sizeHeader
        anchors.top: editingHeader.visible
                     ? editingHeader.bottom
                     : (replyHeader.visible
                        ? replyHeader.bottom
                        : (uploadBanner.visible ? uploadBanner.bottom : parent.top))
        anchors.left: parent.left
        anchors.right: parent.right
        height: visible ? 32 : 0
        color: Theme.bg0
        radius: Theme.r2
        visible: inputRoot.overLimit

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.sp.s4
            anchors.rightMargin: Theme.sp.s4
            spacing: Theme.sp.s3

            // No icon: qml/icons has no warning glyph, and the two banners
            // above use theirs to say WHICH mode the composer is in, not to
            // raise an alarm. Colour and wording carry this one.
            Text {
                text: "Message is too long"
                color: Theme.danger
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                font.weight: Theme.fontWeight.semibold
            }
            Text {
                // The numbers, because "too long" on its own gives the user no
                // idea whether to delete a word or a page. Bytes rather than
                // characters is what the server counts, and saying so is more
                // honest than quoting a character figure that is wrong for
                // anything outside ASCII.
                text: "— " + (inputRoot.bodyBytes - inputRoot.maxBodyBytes)
                      + " bytes over the " + inputRoot.maxBodyBytes
                      + "-byte limit. Shorten it, or attach it as a file."
                color: Theme.fg2
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.sm
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
        }
    }

    RowLayout {
        id: inputCore
        anchors.top: sizeHeader.visible
                     ? sizeHeader.bottom
                     : (editingHeader.visible
                        ? editingHeader.bottom
                        : (replyHeader.visible ? replyHeader.bottom : parent.top))
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.topMargin: inputRoot._bannerHeight > 0 ? 4 : 0
        anchors.leftMargin: Theme.sp.s3
        anchors.rightMargin: Theme.sp.s3
        spacing: Theme.sp.s1

        // Attachment button — hidden if user lacks ATTACH_FILES.
        Rectangle {
            Layout.preferredWidth: Theme.isMobile ? Theme.touchTarget : 28
            Layout.preferredHeight: Theme.isMobile ? Theme.touchTarget : 28
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
                    inputRoot._refreshSlashQuery();
                }
                onCursorPositionChanged: {
                    inputRoot._refreshMentionQuery();
                    inputRoot._refreshSlashQuery();
                }

                Keys.onPressed: (event) => {
                    // Paste (U-M14). Only claims the event when the
                    // clipboard actually holds an image or local files;
                    // a text paste falls through to TextArea untouched.
                    // Both modifiers are tested because Qt maps the mac
                    // Command key onto ControlModifier by default, but
                    // not in every embedding.
                    if (event.key === Qt.Key_V
                        && (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier))
                        && !(event.modifiers & Qt.AltModifier)) {
                        if (inputRoot._pasteAttachments()) {
                            event.accepted = true;
                            return;
                        }
                    }

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

                    // Same nav semantics for the slash-command popup.
                    // Up/Down change selection, Tab/Enter accept,
                    // Esc dismisses without submitting. We don't
                    // intercept Enter when no matches remain so the
                    // user can still send `/foo bar` for an unknown
                    // command (it'll fall through as a literal
                    // message — matching Discord's behaviour).
                    if (inputRoot.slashAnchor >= 0) {
                        var matches = inputRoot._slashMatches();
                        if (matches.length === 0) return;
                        if (event.key === Qt.Key_Down) {
                            inputRoot.slashSelected =
                                (inputRoot.slashSelected + 1) % matches.length;
                            event.accepted = true;
                            return;
                        }
                        if (event.key === Qt.Key_Up) {
                            inputRoot.slashSelected =
                                (inputRoot.slashSelected - 1 + matches.length)
                                % matches.length;
                            event.accepted = true;
                            return;
                        }
                        if (event.key === Qt.Key_Tab) {
                            inputRoot._insertSlash(
                                matches[inputRoot.slashSelected]);
                            event.accepted = true;
                            return;
                        }
                        if (event.key === Qt.Key_Escape) {
                            inputRoot.slashQuery = "";
                            inputRoot.slashAnchor = -1;
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
            Layout.preferredWidth: Theme.isMobile ? Theme.touchTarget : 28
            Layout.preferredHeight: Theme.isMobile ? Theme.touchTarget : 28
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

        // Bytes remaining, in the last tenth of the budget only. The banner
        // above says what has gone wrong once the limit is passed; this is the
        // thing that lets a user see it coming while they are still typing,
        // which is the difference between "shorten this" and "start again".
        Text {
            Layout.alignment: Qt.AlignVCenter
            visible: inputRoot.nearLimit
            text: (inputRoot.maxBodyBytes - inputRoot.bodyBytes).toString()
            color: inputRoot.overLimit ? Theme.danger : Theme.fg3
            font.family: Theme.fontSans
            font.pixelSize: Theme.fontSize.xs
            // Bare digits next to a composer read as a countdown; a screen
            // reader gets no such context from them.
            Accessible.role: Accessible.StaticText
            Accessible.name: (inputRoot.maxBodyBytes - inputRoot.bodyBytes)
                             + " bytes remaining"
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
            Layout.preferredWidth: Theme.isMobile ? Theme.touchTarget : 28
            Layout.preferredHeight: Theme.isMobile ? Theme.touchTarget : 28
            Layout.alignment: Qt.AlignVCenter
            radius: Theme.r1
            // Screen readers: a "Send" button at all times — the
            // visual disabled state already covers empty composers.
            Accessible.role: Accessible.Button
            Accessible.name: "Send message"
            // The over-length case gets its own sentence: a screen reader
            // user has no red border and no banner colour to go on, and
            // "nothing to send yet" in front of a composer full of text is
            // actively misleading.
            Accessible.description: sendBtn.armed
                ? "Send the typed message"
                : (inputRoot.overLimit
                   ? "Message is " + (inputRoot.bodyBytes - inputRoot.maxBodyBytes)
                     + " bytes over the " + inputRoot.maxBodyBytes
                     + "-byte limit and cannot be sent"
                   : "Nothing to send yet")
            Accessible.onPressAction: if (sendBtn.armed) sendCurrentMessage()
            // `armed` must include preeditText so Android IMEs (which
            // keep keystrokes in preedit until a commit char like space
            // or newline) arm the send button on the first character,
            // not just after the user types a newline.
            readonly property bool armed:
                (inputArea.text.trim().length > 0
                 || inputArea.preeditText.length > 0)
                && !inputRoot.uploading
                && !inputRoot.overLimit
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
                            // @room has no mxid to show, so it explains
                            // itself here instead.
                            text: modelData.subtitle
                                  || ("@" + modelData.tokenName)
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

    // Slash-command autocomplete popup. Mirrors mentionPopup's
    // visual + interaction language so users don't have to learn
    // a second affordance: same chrome, same arrow/Tab/Esc keys,
    // anchored above the composer in the same spot.
    Popup {
        id: slashPopup
        parent: inputRoot
        y: -height - 4
        x: Theme.sp.s3
        width: 320
        padding: 4
        modal: false
        focus: false
        closePolicy: Popup.NoAutoClose
        visible: inputRoot.slashAnchor >= 0 && matches.length > 0

        property var matches: inputRoot.slashAnchor >= 0
                              ? inputRoot._slashMatches() : []

        background: Rectangle {
            color: Theme.bg1
            border.color: Theme.line
            border.width: 1
            radius: Theme.r2
        }

        contentItem: Column {
            spacing: 0

            Text {
                leftPadding: Theme.sp.s3
                topPadding: Theme.sp.s2
                bottomPadding: Theme.sp.s2
                text: "SLASH COMMANDS"
                font.family: Theme.fontSans
                font.pixelSize: Theme.fontSize.xs
                font.weight: Theme.fontWeight.semibold
                font.letterSpacing: Theme.trackWidest.xs
                color: Theme.fg3
            }

            Repeater {
                model: slashPopup.matches
                delegate: Rectangle {
                    required property int index
                    required property var modelData
                    width: slashPopup.width - 8
                    height: 40
                    radius: Theme.r1
                    color: (index === inputRoot.slashSelected)
                           ? Theme.accent
                           : (slashRowHover.containsMouse ? Theme.bg3 : "transparent")
                    Behavior on color { ColorAnimation { duration: Theme.motion.fastMs } }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.sp.s3
                        anchors.rightMargin: Theme.sp.s3
                        spacing: Theme.sp.s3

                        // Command name — monospaced so users
                        // immediately read it as something to type.
                        Text {
                            text: modelData.name
                                + (modelData.usage ? " " + modelData.usage : "")
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fontSize.sm
                            font.weight: Theme.fontWeight.medium
                            color: index === inputRoot.slashSelected
                                   ? Theme.onAccent : Theme.fg0
                            elide: Text.ElideRight
                        }
                        Text {
                            text: modelData.description
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.fontSize.xs
                            color: index === inputRoot.slashSelected
                                   ? Qt.rgba(0, 0, 0, 0.6) : Theme.fg3
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                    MouseArea {
                        id: slashRowHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            inputRoot._insertSlash(modelData);
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
                inputRoot.noteUploadStarted();
            }
        }
    }

    // One completion per upload — decrement rather than clear, so N
    // files in flight unlock the composer only when the last one lands.
    Connections {
        target: serverManager.activeServer ? serverManager.activeServer : null
        ignoreUnknownSignals: true

        function onMediaSendCompleted() {
            inputRoot._noteUploadFinished();
        }

        function onMediaSendFailed(error) {
            inputRoot._noteUploadFinished();
            console.warn("Media upload failed:", error);
        }
    }

    // ── Paste an image or a file into the composer (U-M14) ───────────
    //
    // Routes to exactly the same upload path as the attach button and
    // the drop area. Returns false when there is nothing pasteable on
    // the clipboard, which is the caller's signal to let the normal
    // text paste happen.
    function _pasteAttachments() {
        if (!inputRoot.canAttach) return false;
        var s = serverManager.activeServer;
        if (!s || !s.activeRoomId || s.activeRoomId.length === 0) return false;
        if (!serverManager.clipboardFileUrls) return false;
        var urls = serverManager.clipboardFileUrls();
        if (!urls || urls.length === 0) return false;
        for (var i = 0; i < urls.length; ++i) {
            s.sendMediaMessage(urls[i]);
            inputRoot.noteUploadStarted();
        }
        return true;
    }

    // Canonical catalogue of supported slash commands. Drives both
    // the autocomplete popup (definitions visible to the UI) and
    // the at-send transformation (`_runSlashCommand` walks the same
    // table). Adding a new command is a single entry here.
    //
    // Each definition:
    //   name        canonical "/foo"
    //   aliases     extra names that map to the same handler
    //   description short one-liner shown in the popup
    //   usage       hint for the argument shape, "" if none
    //   handler(rest) returns { body, msgtype }, or null to skip
    //
    // Handlers run on send; the popup itself doesn't execute them
    // (it only completes the typed text). Server-side / UI side
    // effects (DM creation, etc.) happen via the handler's optional
    // `sideEffect: function(rest)` field, called before send.
    readonly property var slashCatalogue: [
        {
            name: "/me",
            aliases: [],
            description: "Send an action message",
            usage: "<action>",
            handler: function(rest) {
                return { body: rest, msgtype: "m.emote" };
            }
        },
        {
            name: "/shrug",
            aliases: [],
            description: "¯\\_(ツ)_/¯",
            usage: "[message]",
            handler: function(rest) {
                return {
                    body: rest + (rest ? " " : "") + "¯\\_(ツ)_/¯",
                    msgtype: "m.text"
                };
            }
        },
        {
            name: "/tableflip",
            aliases: [],
            description: "(╯°□°)╯︵ ┻━┻",
            usage: "[message]",
            handler: function(rest) {
                return {
                    body: rest + (rest ? " " : "") + "(╯°□°)╯︵ ┻━┻",
                    msgtype: "m.text"
                };
            }
        },
        {
            name: "/unflip",
            aliases: [],
            description: "┬─┬ノ( º _ ºノ)",
            usage: "[message]",
            handler: function(rest) {
                return {
                    body: rest + (rest ? " " : "") + "┬─┬ノ( º _ ºノ)",
                    msgtype: "m.text"
                };
            }
        },
        {
            name: "/lenny",
            aliases: [],
            description: "( ͡° ͜ʖ ͡°)",
            usage: "[message]",
            handler: function(rest) {
                return {
                    body: rest + (rest ? " " : "") + "( ͡° ͜ʖ ͡°)",
                    msgtype: "m.text"
                };
            }
        },
        // Spoiler — Matrix renders <span data-mx-spoiler> with a
        // hidden-by-default style. The plain `body` carries a
        // textual fallback for clients that don't render HTML.
        {
            name: "/spoiler",
            aliases: ["/sp"],
            description: "Hide text behind a spoiler",
            usage: "<text>",
            handler: function(rest) {
                if (!rest) return null;
                // The send path expands HTML when m_richMessage; the
                // command therefore returns a plain body and lets
                // sendCurrentMessage's hasMarkdown path do the
                // rest. We use a raw HTML wrapper distinguishable
                // from any markdown we'd otherwise generate.
                return {
                    body: "[spoiler] " + rest,
                    msgtype: "m.text",
                    formattedHtml: "<span data-mx-spoiler>"
                                   + rest.replace(/&/g, "&amp;")
                                         .replace(/</g, "&lt;")
                                         .replace(/>/g, "&gt;")
                                   + "</span>"
                };
            }
        },
        // Quick DM-jump — completes via the existing DM create
        // path (ServerConnection.createDirectMessage) and skips
        // sending anything in the current channel. The handler
        // returns null (no message sent); sideEffect creates the
        // room.
        {
            name: "/dm",
            aliases: ["/msg"],
            description: "Open a DM with someone",
            usage: "@user:server",
            handler: function(rest) { return null; },
            sideEffect: function(rest) {
                if (!serverManager.activeServer || !rest) return;
                var t = rest.trim();
                if (!t.startsWith("@") && t.indexOf(":") > 0) t = "@" + t;
                serverManager.activeServer.createDirectMessage(t);
            }
        },
        {
            name: "/clear",
            aliases: [],
            description: "Clear the composer (or use Esc)",
            usage: "",
            handler: function(rest) { return null; },
            sideEffect: function(rest) { inputArea.text = ""; }
        }
    ]

    // Active query state. mentionQuery / mentionAnchor have a parallel
    // pair for slash commands; -1 means "no slash popup right now".
    property string slashQuery: ""
    property int slashAnchor: -1
    property int slashSelected: 0

    function _slashMatches() {
        // Return up to N catalogue entries whose name or alias
        // matches the current slashQuery. The first / is included
        // in slashQuery so we can match exactly without re-prepending.
        var q = slashQuery.toLowerCase();
        var out = [];
        for (var i = 0; i < slashCatalogue.length; ++i) {
            var c = slashCatalogue[i];
            var names = [c.name].concat(c.aliases || []);
            var hit = false;
            for (var j = 0; j < names.length; ++j) {
                if (names[j].toLowerCase().indexOf(q) === 0) {
                    hit = true; break;
                }
            }
            if (hit) out.push(c);
        }
        return out;
    }

    // Re-evaluate whether the cursor is on a slash command and what
    // the user has typed so far. Mirrors `_refreshMentionQuery` —
    // walks back from the cursor until a `/` at start-of-input or
    // start-of-line is found, otherwise clears the slash state.
    function _refreshSlashQuery() {
        var text = inputArea.text;
        var cp = inputArea.cursorPosition;
        // Slash commands are only valid at the very start of the
        // composer — typing "hello /me" should not pop the menu,
        // matching every other chat client's convention.
        var hasSlashAtStart = text.startsWith("/");
        if (!hasSlashAtStart) {
            slashQuery = "";
            slashAnchor = -1;
            return;
        }
        // Find the first space; the slash query is everything up to
        // it (or to the cursor).
        var sp = text.indexOf(" ");
        var end = sp >= 0 ? sp : text.length;
        if (cp > end) {
            // Cursor moved past the command word — popup hides; we
            // still let the send path run the command.
            slashQuery = "";
            slashAnchor = -1;
            return;
        }
        slashAnchor = 0;
        slashQuery = text.substring(0, end);
        slashSelected = 0;
    }

    // Replace the partial slash typed so far with the chosen
    // command + a trailing space (or just commit immediately for
    // commands that take no argument and are intended as one-tap
    // macros, like /shrug).
    function _insertSlash(cmd) {
        if (slashAnchor < 0) return;
        var current = inputArea.text;
        var sp = current.indexOf(" ");
        var rest = sp >= 0 ? current.substring(sp) : "";
        inputArea.text = cmd.name + (rest.length > 0 ? rest : " ");
        inputArea.cursorPosition = cmd.name.length + 1;
        slashQuery = "";
        slashAnchor = -1;
    }

    // Transform /me, /shrug, /tableflip, /unflip into the right message
    // shape or body. /me sends an m.emote; the rest are just text macros.
    // Returns { body, msgtype } or null if not a slash-command.
    function _runSlashCommand(raw) {
        if (!raw.startsWith("/")) return null;
        var space = raw.indexOf(" ");
        var cmd = (space < 0 ? raw : raw.substring(0, space)).toLowerCase();
        var rest = space < 0 ? "" : raw.substring(space + 1);
        for (var i = 0; i < slashCatalogue.length; ++i) {
            var c = slashCatalogue[i];
            var names = [c.name].concat(c.aliases || []);
            if (names.indexOf(cmd) < 0) continue;
            if (c.sideEffect) c.sideEffect(rest);
            return c.handler(rest);
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

        // Checked here and not only on the send button, because Enter and the
        // Accessible press action both reach this function directly. The
        // banner is already up by the time anyone can get here — overLimit is
        // a binding on the same text — so this is the refusal, not the
        // notification. Nothing is trimmed, split or sent in part: the
        // message stays in the composer exactly as typed, which is the only
        // state from which the user can fix it.
        //
        // Slash commands are below this line on purpose. "/me <a novel>"
        // becomes an m.emote with the same oversize body and the server
        // refuses it identically.
        if (inputRoot.overLimit) return;

        // Slash-command intercept — takes priority over replies/edits
        // since "/me fixed typo" inside a reply context would be a
        // weird thing to commit to.
        if (inputRoot.editingEventId === "" && inputRoot.replyToEventId === "") {
            // _runSlashCommand can return null even on a recognised
            // command (e.g. /clear, /dm) — those have side-effects
            // only and shouldn't post anything in the channel.
            // We detect that by re-checking whether the catalogue
            // recognises the leading word.
            var lead = text.split(/\s/)[0].toLowerCase();
            var recognised = false;
            for (var i = 0; i < slashCatalogue.length; ++i) {
                var c = slashCatalogue[i];
                var names = [c.name].concat(c.aliases || []);
                if (names.indexOf(lead) >= 0) { recognised = true; break; }
            }
            var cmd = _runSlashCommand(text);
            if (cmd) {
                if (cmd.msgtype === "m.emote") {
                    serverManager.activeServer.sendEmote(cmd.body);
                } else if (cmd.formattedHtml
                    && serverManager.activeServer.sendRichMessage) {
                    // /spoiler returns formattedHtml so the receiver
                    // sees a real <span data-mx-spoiler>; clients
                    // that don't render HTML fall back to the
                    // bracketed plain body.
                    serverManager.activeServer.sendRichMessage(
                        cmd.body, cmd.formattedHtml, []);
                } else {
                    serverManager.activeServer.sendMessage(cmd.body);
                }
                inputArea.text = "";
                inputRoot.lastSentAt = Date.now();
                return;
            }
            if (recognised) {
                // Recognised command with a null handler return
                // (e.g. /clear, /dm). Side-effects already ran;
                // skip posting, just clear.
                inputArea.text = "";
                inputRoot.lastSentAt = Date.now();
                return;
            }
        }

        var rich = _buildRichPayload(text);

        if (inputRoot.editingEventId !== "") {
            // Edit path — server still gates on sender match. Don't apply
            // slowmode / canSend checks to edits; they're intentionally
            // allowed during cooldown (you're refining, not flooding).
            if (text === inputRoot.editingOriginalBody) {
                inputRoot.cancelEditing();
                return;
            }
            // The mention set rides along so the edit doesn't strip the
            // message's highlight (the server folds m.new_content in as the
            // event's content, so an m.mentions-less edit blanks it for anyone
            // loading the room fresh). Note it cannot ADD a mention: the server
            // deliberately records no mention rows for an m.replace, so
            // editing "hi" into "hi @alice" will render as a mention for
            // everyone but will never badge or notify Alice.
            serverManager.activeServer.editMessage(
                inputRoot.editingEventId, text, rich.html, rich.uids);
            inputRoot.cancelEditing();
            inputRoot._clearMentionState();
            return;
        }

        if (!inputRoot.canSend || inputRoot.slowmodeRemaining > 0) return;

        if (inputRoot.replyToEventId !== "") {
            // Reply path — send as a m.in_reply_to-bearing message, then
            // clear both the composer and the reply banner. Mentions in a
            // reply are honoured in full server-side (badge + push), which is
            // why they are passed through here rather than dropped.
            serverManager.activeServer.replyToMessage(
                inputRoot.replyToEventId, text, rich.html, rich.uids);
            inputArea.text = "";
            inputRoot.cancelReplying();
            inputRoot._clearMentionState();
            inputRoot.lastSentAt = Date.now();
            return;
        }

        if (rich.html !== "") {
            serverManager.activeServer.sendRichMessage(text, rich.html, rich.uids);
        } else {
            serverManager.activeServer.sendMessage(text);
        }
        inputArea.text = "";
        inputRoot._clearMentionState();
        inputRoot.lastSentAt = Date.now();
    }

    // Word-boundary matcher for a mention token (U-M11).
    //
    // `text.indexOf("@Al")` is true for "@Alice", so picking Al from the
    // autocomplete and then typing about Alice pinged Al — and picking
    // both put Al's anchor inside Alice's name. \b does not help: `@` is
    // itself a non-word character, so /\b@Al\b/ still matches inside
    // "@Alice". The rule that does work: the token starts at the
    // beginning of the text or after whitespace, and is not followed by
    // another name character. Same shape as the @room test above.
    function _escapeRegExp(s) { return s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"); }
    function _mentionRegExp(token, flags) {
        return new RegExp("(^|\\s)" + inputRoot._escapeRegExp(token)
                          + "(?![A-Za-z0-9_-])", flags || "");
    }

    function _clearMentionState() {
        inputRoot.mentionTokens = [];
        inputRoot.mentionQuery = "";
        inputRoot.mentionAnchor = -1;
    }

    // Build the { html, uids } pair for `text` from the tracked mention tokens
    // plus markdown. `html` is "" when the message needs no formatted_body at
    // all, which is the caller's signal to use the plain-text send path.
    //
    // Extracted from sendCurrentMessage so the reply and edit paths get the
    // same treatment: both used to return before this ran, so a mention typed
    // into a reply reached the server with no m.mentions and produced no badge
    // for the person named, and an edit stripped the mention set outright.
    function _buildRichPayload(text) {
        // Scan the tracked mention tokens against the composer text; the
        // ones still present (user didn't delete them mid-typing) get
        // rewritten into HTML anchors + added to m.mentions.user_ids.
        var activeMentions = [];
        for (var i = 0; i < mentionTokens.length; i++) {
            var t = mentionTokens[i];
            if (t.isRoom) {
                // @room pings everybody, so it gets a word-boundary check
                // rather than a bare substring test — deleting the tail of
                // "@roommate" must not be what fires a channel-wide alert.
                if (/(^|\s)@room(?![A-Za-z0-9_])/.test(text)) activeMentions.push(t);
                continue;
            }
            if (inputRoot._mentionRegExp(t.token).test(text)) activeMentions.push(t);
        }

        var hasMarkdown = _hasMarkdown(text);
        if (activeMentions.length === 0 && !hasMarkdown) return { html: "", uids: [] };

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
            if (m.isRoom) {
                // "@room" is a boolean in m.mentions, not a user id, and
                // it is nobody's profile — so no anchor. MatrixClient
                // lifts this sentinel out of the list; the receiving
                // client's MentionRenderer styles the bare token.
                if (uids.indexOf("@room") < 0) uids.push("@room");
                continue;
            }
            var encId = encodeURIComponent(m.userId);
            var anchor = '<a href="bsfchat://user/' + encId
                         + '" style="color:' + Theme.accent
                         + '; text-decoration:none; font-weight:bold;">'
                         + m.token + '</a>';
            // Boundary-matched, not split/join: a plain substring swap
            // rewrote the "@Al" inside "@Alice" and cut the longer name
            // in half, leaving a broken anchor and a stray "ice".
            html = html.replace(inputRoot._mentionRegExp(m.token, "g"),
                                function(_, lead) { return lead + anchor; });
            if (uids.indexOf(m.userId) < 0) uids.push(m.userId);
        }
        return { html: html, uids: uids };
    }
}
