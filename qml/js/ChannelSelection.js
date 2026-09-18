.pragma library

// Which channel-sidebar row is "the one you are looking at".
//
// The sidebar highlight is not "which room is loaded" — it is "which
// room the main pane is currently SHOWING". Those two are different
// because the voice CONNECTION and the displayed VIEW are orthogonal in
// ServerConnection: `activeRoomId` (the text room) survives joining a
// voice channel, on purpose, so that leaving voice drops you back into
// the channel you were reading. main.qml's StackLayout picks the pane
// off `viewingVoiceRoom` alone.
//
// The bug this file exists for: the row predicates were
//     isActiveText  = !isVoice && roomId === activeRoomId
//     isActiveVoice =  isVoice && roomId === activeVoiceRoomId
// with no reference to `viewingVoiceRoom`. Both are true at once the
// moment you join voice, so the sidebar highlighted TWO rows — the
// voice channel you are in and the text channel you are no longer
// looking at — and the owner reported exactly that. `viewingVoiceRoom`
// is the tiebreak, and it is the same flag the pane swap reads, so the
// highlight cannot drift from what is on screen.
//
// Pure functions over plain objects, same reason as VideoStage.js and
// VideoWindows.js: the BSFChat QML module is compiled into the app
// binary, so no test target can instantiate ChannelList.qml. The tests
// in tests/qml/tst_channelselection.qml exercise THIS file — the one
// ChannelList.qml imports — not a restatement of its rules.
//
// `view` is a plain snapshot of the three ServerConnection properties:
//     { activeRoomId, activeVoiceRoomId, viewingVoiceRoom }
// ChannelList.qml builds it once per change in `selectionView` — the
// reads happen in QML so the bindings' dependencies are captured there
// rather than inside a shared library, and so this file only ever sees
// plain objects a test can construct. A null/undefined view means "no
// server connected": nothing is shown, so nothing is highlighted.

// Normalises the id properties, which are QString on the C++ side and
// therefore "" rather than undefined when unset — but a plain JS object
// in a test, or a connection that has not synced, can hand us either.
function _id(v) {
    return (typeof v === "string") ? v : "";
}

// True when the main pane is showing the VoiceRoom. Anything that is
// not an explicit `true` counts as "showing text", because the pane
// swap in main.qml is written the same way (`... ? 1 : 0`).
function viewingVoice(view) {
    return !!(view && view.viewingVoiceRoom);
}

// A TEXT row (regular channel or DM) is highlighted only while the text
// pane is the one on screen. Note this deliberately does not ask
// whether a voice call is in progress: you can be in voice and reading
// text, and then the text row is the right one to light up.
function textRowSelected(roomId, view) {
    if (!view) return false;
    var id = _id(roomId);
    var active = _id(view.activeRoomId);
    var voiceView = viewingVoice(view);
    if (id === "") return false;
    return !voiceView && id === active;
}

// A VOICE row is highlighted only while the voice pane is on screen.
// Being connected to a voice channel while reading text is shown by the
// participant list under the row and by the VoiceDock, not by the
// selected-row highlight — that one means "this is what you're looking
// at", and there is only ever one of it.
function voiceRowSelected(roomId, view) {
    if (!view) return false;
    var id = _id(roomId);
    var activeVoice = _id(view.activeVoiceRoomId);
    var voiceView = viewingVoice(view);
    if (id === "") return false;
    return voiceView && id === activeVoice;
}

// The predicate the delegates use. `row` is { roomId, isVoice } — the
// shape of an entry in ServerConnection.categorizedRooms.
function rowSelected(row, view) {
    if (!row) return false;
    return row.isVoice ? voiceRowSelected(row.roomId, view)
                       : textRowSelected(row.roomId, view);
}

// Exactly one row can be selected at a time. Handy as a test invariant
// and as a guard for anything that wants to assert the sidebar is not
// double-highlighting; not used by the QML itself.
function selectedRoomId(view) {
    if (!view) return "";
    return viewingVoice(view) ? _id(view.activeVoiceRoomId)
                              : _id(view.activeRoomId);
}
