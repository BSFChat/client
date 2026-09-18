.pragma library

// What the profile card's avatar tile should show.
//
// Extracted from UserProfileCard.qml because that file imports the BSFChat
// module, which only the app binary has, so no headless test can instantiate
// the card and look at it. See tests/qml/tst_profilecardavatar.qml.
//
// The card is ONE reused Popup. MemberList.qml and MessageView.qml assign
// `userId` / `profileDisplayName` onto the same instance and call open() again
// for the next member, so everything it holds about the PREVIOUS member has to
// be cleared on open — the replacement value may never arrive. Two ways it
// does not:
//
//   * the profile GET is dropped silently on any network error
//     (MatrixClient::getProfile returns early, emitting nothing), and a member
//     who has set neither a display name nor a picture is exactly the account
//     a homeserver is entitled to answer 404 for;
//   * even when it does arrive, it arrives a round trip after the popup has
//     already animated in.
//
// That is how B's card came to wear A's photograph.

// The avatar URL a card freshly opened on `userId` starts from. Always empty:
// nothing is known about this member until their profile reply lands, and
// "not known yet" has to render as initials, never as whoever was shown last.
function avatarUrlOnOpen(userId) {
    return "";
}

// Fold a profileFetched(uid, displayName, avatarUrl) signal into the card's
// avatar URL. A reply for anybody else leaves it alone: the card is shared and
// the user can click A, dismiss, then click B well before A's reply lands, so
// replies arrive out of order with respect to what is on screen.
function avatarUrlOnProfile(cardUserId, currentUrl, uid, avatarUrl) {
    if (!cardUserId || uid !== cardUserId) return currentUrl || "";
    return avatarUrl || "";
}

// The Image's `source`. "" means "draw the initial instead" — including the
// case where there IS an mxc URI but it does not resolve to a download URL,
// which otherwise leaves a tile holding neither a picture nor a letter.
// `resolve` is ServerConnection.resolveMediaUrl, or null when there is no
// active server.
function avatarSource(avatarUrl, resolve) {
    if (!avatarUrl || !resolve) return "";
    return resolve(avatarUrl) || "";
}

// The letter drawn when there is no picture. Leading punctuation is skipped so
// an mxid ("@ada:host") reads as "A" rather than "@".
function initial(displayName, userId) {
    var n = displayName || userId || "?";
    var s = String(n).replace(/^[^a-zA-Z0-9]+/, "");
    return (s.length > 0 ? s.charAt(0) : "?").toUpperCase();
}
