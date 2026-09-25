.pragma library

// What the signed-in-account row in the channel drawer should say about who
// you are.
//
// The bug this exists for, photographed for the store screenshots: the row
// rendered the display name over `serverManager.activeServer.userId`, elided
// right, in a 240px sidebar with the mute / deafen / settings trio eating
// most of it. Accounts here are provisioned through OIDC, so every localpart
// is the provider's opaque subject — and the row read
//
//     test
//     @oidc_fe982c2…
//
// A truncated GUID. Not the user's handle, not the server, not anything a
// person can act on, and it is what EVERY user of an OIDC homeserver sees in
// the corner of every screen. Eliding right is the worst of the available
// truncations too: it keeps the meaningless prefix and throws away the
// domain, which was the one part of that string still carrying information.
//
// The mxid itself is not the problem and is not being hidden. It keeps two
// homes, both of which already existed and both of which show it in full:
//
//   * the account menu, one tap away on the same row (ChannelList.qml,
//     `userMenu`) — its own comment already says the footer's elided copy is
//     "worse than useless", which is this defect, written down before the
//     screenshots found it;
//   * Settings → Account → USER ID (UserSettings.qml), reachable on both
//     shells including the phone.
//
// Hover tooltips do NOT count as a home: there is no hover on a phone, and
// the phone is where this was found.
//
// See tests/qml/tst_useridentity.qml.

// Split an mxid at the FIRST colon after the sigil, not the last and not
// with a bare split(":"). A homeserver may carry a port — `@a:example.org:8448`
// is a legal user id — so the domain is everything after that first colon,
// colons and all. A localpart cannot contain one.
function _parse(userId) {
    var s = (userId === undefined || userId === null) ? "" : String(userId);
    if (s.charAt(0) !== "@") return { local: "", domain: "", valid: false };
    var i = s.indexOf(":");
    if (i < 1) return { local: s.substring(1), domain: "", valid: false };
    return { local: s.substring(1, i), domain: s.substring(i + 1), valid: true };
}

function localpart(userId) { return _parse(userId).local; }

function domain(userId) { return _parse(userId).domain; }

// Is this localpart an identity-provider subject rather than a handle a human
// chose?
//
// Kept deliberately narrow, because the cost of the two mistakes is not
// symmetric. Calling a real handle opaque hides it from a row that has the
// full mxid one tap away — mildly annoying. Calling a GUID a handle puts the
// GUID back on screen, which is the defect. So the test only fires on
// something that is unmistakably machine-generated: at least 16 characters,
// hex digits and dashes only (a UUID with or without its dashes, or a hash),
// and at least one actual digit in it.
//
// The digit requirement exists solely to spare the handful of real English
// words spellable in hex — "deadbeef", "facade", "decade" — and words are
// what a person picks. A 16-character all-letter hex string is possible in
// principle and has probability about 1e-14 of coming out of a UUID.
//
// One provider prefix is stripped first: Synapse writes `oidc_`, `sso_`,
// `saml_` or `cas_` in front of the subject it was given. The prefix is not
// on its own evidence of anything — an IdP whose subject claim is readable
// produces `@oidc_josh:host`, which IS a handle and is left alone.
function isOpaqueLocalpart(local) {
    var s = (local === undefined || local === null) ? "" : String(local);
    var m = /^(?:oidc_|sso_|saml_|cas_)/.exec(s);
    if (m) s = s.substring(m[0].length);
    if (s.length < 16) return false;
    if (!/^[0-9a-fA-F-]+$/.test(s)) return false;
    return /[0-9]/.test(s);
}

// The row's first line: who you are.
//
// The display name, and it usually is one. When the account has not set one
// the old row rendered an empty string and left the GUID underneath as the
// only thing on it, so there is a fallback chain now: display name, then the
// localpart, then whatever we were given. The last two are both poor, and
// they are poor honestly — there is no name to show, and a blank line does
// not say that.
function accountTitle(userId, displayName) {
    var dn = (displayName === undefined || displayName === null)
           ? "" : String(displayName).trim();
    if (dn.length > 0) return dn;
    var p = _parse(userId);
    if (p.local.length > 0) return p.local;
    return (userId === undefined || userId === null) ? "" : String(userId);
}

// The row's second line: which account, in the width actually available.
//
// An opaque localpart contributes nothing, so what is left is the homeserver
// — which is real information in a client with a server rail, where you can
// be signed in to several at once and the question the row can answer in one
// short line is "which server is this". A readable localpart is a handle, so
// the whole mxid stays: that is what people quote at each other, and on a
// non-OIDC server this line is unchanged from what it has always shown.
//
// Returns "" when there is nothing worth a line, and the caller hides the
// line rather than reserving blank space for it.
function accountSubtitle(userId) {
    var p = _parse(userId);
    if (!p.valid) {
        return (userId === undefined || userId === null) ? "" : String(userId);
    }
    if (isOpaqueLocalpart(p.local)) return p.domain;
    return "@" + p.local + ":" + p.domain;
}
