.pragma library

// Which surface owns the mobile shell's main column: the message timeline,
// the voice room, or the shell's own empty state.
//
// ── The bug this file exists for ─────────────────────────────────────────
//
// MobileMain.qml asked that question twice, in two incompatible ways. The
// page swap read the CONNECTION:
//
//     StackLayout { currentIndex: activeServer.viewingVoiceRoom ? 1 : 0
//                   MessageView { id: chatView
//                                 visible: activeServer.activeRoomId !== "" }
//                   VoiceRoom { } }
//
// and the empty state read the PAGE:
//
//     ColumnLayout { visible: !chatView.visible ... }   // "No channel selected"
//
// A StackLayout writes `visible` on each of its children every time
// currentIndex changes — that is how it shows one page and hides the rest.
// So `chatView.visible` never meant "there is a channel to show"; it meant
// "the chat page is on top". Two writers, and they disagree in BOTH
// directions. Measured, on a real StackLayout, in tests/qml/tst_mainsurface.qml:
//
//   * Flip to the voice view with a text channel still active. The layout
//     sets chatView.visible = false. The empty state concludes there is no
//     channel and paints "No channel selected" ACROSS the live video tiles.
//     (Owner's Pixel 6 Pro, 2026-09-24: in voice #spikk with two
//     participants, #penis the active text room, the empty state composited
//     over a screen-share tile and a camera tile.)
//
//   * The reverse, and the nastier one, because the layout's write does NOT
//     kill the declared binding. Let a channel arrive WHILE the voice view is
//     up — a reconnect restoring the last text room, a search jump, the first
//     sync — and that binding re-fires and sets chatView.visible = true while
//     the layout still has the voice page visible. BOTH pages render, and the
//     timeline is on top of the video.
//
// The fix is not a z-order and not a longer condition. Stacking the empty
// state behind the video would still show it through the gaps between tiles,
// and a longer condition leaves the second writer in place. The three
// surfaces are three PAGES of the one StackLayout, so the layout itself makes
// them mutually exclusive and there is nothing left to disagree with. The
// hygiene rule `theMobileMainColumnHasExactlyOneWriter()` in
// tests/test_qml_hygiene.cpp is what keeps a `visible:` from growing back on
// one of them.
//
// The rest of the mobile shell was swept for the same class at the same time.
// The empty state was the only unconditional painter over the column: every
// other thing that can cover it is either user-summoned (the drawers, the
// settings and profile popups, LoginDialog) or deliberately allowed over
// everything (ToastHost, which is transient and self-dismissing). The
// timeline's own overlays — the pagination spinner, the scroll-to-latest
// chevron, the "It's quiet in here" state, the sync banner — are all real
// children of MessageView, so the layout hides them along with the page and
// they were never able to reach the video. The desktop shell was never
// affected: qml/main.qml's StackLayout has no child carrying its own
// `visible:` and no sibling empty state.
//
// This lives out here, rather than inline in MobileMain.qml, for the reason
// ChannelSelection.js does: MobileMain.qml imports the BSFChat module, which
// is compiled into the app binary, so no test target can instantiate it, and
// a rule that cannot be exercised is a rule that drifts.
//
// `view` is a plain snapshot of two ServerConnection properties,
//     { activeRoomId, viewingVoiceRoom }
// built in QML so the binding's dependencies are captured there — the same
// shape and the same reason as ChannelSelection.js's `selectionView`. A
// null/undefined view means "no server connected", which is the empty state.

// StackLayout child order in qml/mobile/MobileMain.qml. These ARE the indices:
// reorder the pages and these constants move with them, which is the point of
// naming them at all.
var Chat  = 0;
var Voice = 1;
var Empty = 2;

// The ids are QString on the C++ side, so "" rather than undefined when
// unset — but a plain JS object in a test, or a connection that has not
// synced, can hand us either. Same normaliser as ChannelSelection._id.
function _id(v) {
    return (typeof v === "string") ? v : "";
}

// True when the voice room is the surface on screen. The voice CONNECTION
// (inVoiceChannel) is orthogonal and deliberately not consulted: you can be
// in a call and reading text, and then text is what the column shows. That
// is also why the VoiceDock's own `visible:` reads inVoiceChannel and not
// this — the dock is chrome under the column, not a surface in it.
// Anything that is not an explicit `true` counts as "not viewing voice",
// matching how the old `... ? 1 : 0` swap read it.
function viewingVoice(view) {
    return !!(view && view.viewingVoiceRoom);
}

// True when there is a text room to draw. Not "the chat page is current" —
// that conflation is the bug above.
function hasTextRoom(view) {
    return _id(view && view.activeRoomId) !== "";
}

// The one answer. Voice wins over everything else, because the user asked
// for it explicitly by tapping the voice channel, the VoiceStatusCard or the
// VoiceDock; the empty state is only ever the fallback when there is nothing
// else to show.
function mainPage(view) {
    if (viewingVoice(view)) return Voice;
    return hasTextRoom(view) ? Chat : Empty;
}

// Named predicates for the same answer, so tests and callers can say "the
// empty state is not showing" without knowing that 2 is what that means.
function showsChat(view)       { return mainPage(view) === Chat; }
function showsVoice(view)      { return mainPage(view) === Voice; }
function showsEmptyState(view) { return mainPage(view) === Empty; }
