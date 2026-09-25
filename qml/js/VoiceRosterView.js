.pragma library

// Which roster the channel sidebar shows under a voice channel, when there
// are two answers available and they disagree.
//
// ── The bug this file exists for ─────────────────────────────────────────
//
// Photographed a minute apart on the same phone: the call header said "2 in
// call", and the drawer badged the same voice channel `1` and listed only the
// remote user. The local user — the one holding the phone, in the call, on
// camera — was missing from the drawer.
//
// The two surfaces are fed by two different transports of the SAME
// server-side fact (each user's m.call.member state event):
//
//   header  ServerConnection.voiceMembers — the 5-second poll of
//           GET /rooms/{id}/voice/members for the room we are IN. The server
//           includes the caller's own row, and buildVoiceMembers() stamps
//           `peerState: "connected"` onto it by user id, so self is not
//           merely tolerated there, it is expected.
//
//   drawer  RoomListModel's per-room roster, folded out of m.call.member
//           events as they arrive through sync. It covers every voice
//           channel, including ones we are not in — which is the whole point
//           of it, since "who is in there already" is what makes you decide
//           to join.
//
// NEITHER path filters the local user. There is no self branch anywhere in
// the fold (src/util/VoiceRoster.h), and VoiceParticipantList.qml renders a
// dedicated self row — bold name, a speaking ring driven by the local mic
// level — which would be dead code if self could never appear. The drawer
// omitting self is a DEFECT, not the membership-vs-visibility distinction
// this codebase draws elsewhere: the server deliberately broadcasts
// m.call.member to the whole room precisely so the roster is not private
// (server/src/store/CallSignalling.h, and the test that pins it,
// TheRosterStateEventStillReachesTheWholeRoom).
//
// So the header is the authoritative one, and the rule below is: for the one
// room we are actually connected to, show the roster we actually have.
// Everywhere else the sync-derived roster is the only answer there is, and it
// is returned untouched.
//
// ── What this does NOT fix ───────────────────────────────────────────────
//
// It does not explain why self's m.call.member was missing from the folded
// roster in the first place. That is a delivery question — an event that
// never arrived, or one overwritten by a later `active: false` for the same
// state key — and it lives in sync, not in the sidebar. It matters
// independently of anything here, because if the event really did not
// propagate then REMOTE users cannot see us in that channel either, and no
// amount of local display logic would tell us so.
//
// This rule is therefore deliberately narrow. It does not synthesise a
// participant, it does not invent a row for a channel we are not in, and it
// never adds anybody to a roster it was not handed by a real source. It only
// prefers the better of two sources for the single room where a better one
// exists, which is also what makes the badge and the call header agree by
// construction rather than by luck.

// `view` is a plain snapshot built in QML so the binding's dependencies are
// captured there — the same shape and the same reason as MainSurface.js's and
// ConnectionBanner.js's:
//
//     { roomId,            the channel row being drawn
//       activeVoiceRoomId, the room we are connected to, "" for none
//       connectedRoster,   ServerConnection.voiceMembers (poll-fed)
//       foldedRoster }     RoomListModel's roster for THIS row
//
// Rows are participant maps — { user_id, displayName, muted, deafened,
// cameraOn, screenSharing } — and both sources already produce that shape,
// which is why one can stand in for the other without the delegate knowing.
function rosterFor(view) {
    if (!view) return [];
    var folded = view.foldedRoster || [];
    var roomId = view.roomId || "";
    var activeId = view.activeVoiceRoomId || "";

    // Not the room we are in: the folded roster is the only answer, and for
    // every channel but one it is a perfectly good answer.
    if (roomId === "" || roomId !== activeId) return folded;

    // We are in this room. Prefer the poll, but only when it actually has
    // something to say: it is empty both before the first poll lands and
    // after a teardown, and falling back to an empty list there would blank
    // a roster the fold had got right.
    var connected = view.connectedRoster || [];
    if (connected.length === 0) return folded;
    return connected;
}

// The badge is sized from whatever the list renders, never counted
// separately. RoomListModel already holds that discipline for its own roster
// ("the badge can never outlive the last participant it was counting") and
// substituting a different source for one room is exactly the kind of change
// that would quietly break it.
function rosterCount(view) {
    return rosterFor(view).length;
}
