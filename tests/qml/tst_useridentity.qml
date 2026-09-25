import QtQuick
import QtTest
import "../../qml/js/UserIdentity.js" as UserIdentity

// What the signed-in-account row in the channel drawer says about who you are
// (qml/js/UserIdentity.js), driven the way ChannelList.qml drives it.
//
// The defect, photographed on an iPhone 16 Pro Max for the store screenshots:
//
//     test
//     @oidc_fe982c2…
//
// Accounts here are provisioned through OIDC, so every localpart is the
// provider's opaque subject, and the row rendered it elide-right in a 240px
// sidebar. Every user of an OIDC homeserver saw a truncated GUID in the
// corner of every screen — and elide-right is the worst cut available, since
// it keeps the meaningless prefix and discards the domain, the one part of
// that string still carrying information.
//
// What this file does NOT cover, because a unit test cannot: that
// ChannelList.qml still CALLS these instead of rendering `userId` again. That
// is a call site, not a value, and it is guarded by source text in
// test_qml_hygiene.cpp (theAccountRowNeverShowsARawUserId).
TestCase {
    name: "UserIdentity"

    // ---- parsing ---------------------------------------------------------

    function test_parts_data() {
        return [
            { tag: "ordinary",  id: "@josh:bsfchat.com",
              local: "josh", domain: "bsfchat.com" },
            { tag: "oidc",      id: "@oidc_fe982c2a4b1d4e0fa1c3d5e7b9081726:bsfchat.com",
              local: "oidc_fe982c2a4b1d4e0fa1c3d5e7b9081726", domain: "bsfchat.com" },
            // A homeserver may carry a port, so the split is at the FIRST
            // colon and the domain keeps everything after it. A localpart
            // cannot contain a colon, so this direction is unambiguous.
            { tag: "port",      id: "@josh:bsfchat.com:8448",
              local: "josh", domain: "bsfchat.com:8448" },
            { tag: "dotted",    id: "@josh.mcgrath:bsfchat.com",
              local: "josh.mcgrath", domain: "bsfchat.com" },
            // Not an mxid at all. Nothing is invented from it.
            { tag: "no sigil",  id: "josh:bsfchat.com", local: "", domain: "" },
            { tag: "no colon",  id: "@josh",            local: "josh", domain: "" },
            { tag: "empty",     id: "",                 local: "", domain: "" },
            { tag: "undefined", id: undefined,          local: "", domain: "" },
            { tag: "null",      id: null,               local: "", domain: "" },
        ];
    }
    function test_parts(d) {
        compare(UserIdentity.localpart(d.id), d.local);
        compare(UserIdentity.domain(d.id), d.domain);
    }

    // ---- what counts as an identity-provider subject ---------------------

    function test_isOpaqueLocalpart_data() {
        return [
            // The shape this homeserver actually issues.
            { tag: "synapse oidc uuid", lp: "oidc_fe982c2a4b1d4e0fa1c3d5e7b9081726",
              expect: true },
            { tag: "dashed uuid", lp: "oidc_fe982c2a-4b1d-4e0f-a1c3-d5e7b9081726",
              expect: true },
            { tag: "bare uuid, no prefix", lp: "fe982c2a4b1d4e0fa1c3d5e7b9081726",
              expect: true },
            { tag: "other providers", lp: "saml_fe982c2a4b1d4e0fa1c3d5e7b9081726",
              expect: true },
            { tag: "sso prefix", lp: "sso_fe982c2a4b1d4e0fa1c3d5e7b9081726",
              expect: true },
            { tag: "cas prefix", lp: "cas_1234567890abcdef1234", expect: true },

            // Handles. None of these may be hidden.
            { tag: "short handle", lp: "josh", expect: false },
            { tag: "long handle", lp: "josh.mcgrath.the.longer.one", expect: false },
            { tag: "handle with digits", lp: "ravi2026", expect: false },
            // The prefix alone proves nothing: an IdP whose subject claim is
            // readable produces exactly this, and it IS a handle.
            { tag: "readable oidc subject", lp: "oidc_josh", expect: false },
            { tag: "readable oidc subject, long", lp: "oidc_josh.mcgrath.work",
              expect: false },
            // Hex-shaped but too short to be a subject.
            { tag: "short hex", lp: "deadbeef", expect: false },
            // Hex-spellable English. The digit requirement exists for these:
            // words are what a person picks.
            { tag: "hex words", lp: "deadbeefcafebabe", expect: false },
            { tag: "empty", lp: "", expect: false },
            { tag: "undefined", lp: undefined, expect: false },
        ];
    }
    function test_isOpaqueLocalpart(d) {
        compare(UserIdentity.isOpaqueLocalpart(d.lp), d.expect);
    }

    // ---- the row's first line --------------------------------------------

    function test_accountTitle_data() {
        return [
            { tag: "display name wins", id: "@oidc_fe982c2a4b1d4e0fa1c3d5e7b9081726:b.com",
              dn: "test", expect: "test" },
            // No display name set. The old row bound straight to
            // `displayName` and rendered an empty first line, leaving the
            // GUID underneath as the only thing on the row.
            { tag: "falls back to the localpart", id: "@josh:bsfchat.com", dn: "",
              expect: "josh" },
            { tag: "whitespace is not a name", id: "@josh:bsfchat.com", dn: "   ",
              expect: "josh" },
            { tag: "undefined name", id: "@josh:bsfchat.com", dn: undefined,
              expect: "josh" },
            // An opaque subject and no display name is a genuinely nameless
            // account. Showing the subject is poor, and it is poor honestly:
            // a blank line does not say "there is no name".
            { tag: "nameless oidc account",
              id: "@oidc_fe982c2a4b1d4e0fa1c3d5e7b9081726:b.com", dn: "",
              expect: "oidc_fe982c2a4b1d4e0fa1c3d5e7b9081726" },
            { tag: "nothing at all", id: "", dn: "", expect: "" },
            { tag: "no server yet", id: undefined, dn: undefined, expect: "" },
        ];
    }
    function test_accountTitle(d) {
        compare(UserIdentity.accountTitle(d.id, d.dn), d.expect);
    }

    // ---- the row's second line -------------------------------------------

    function test_accountSubtitle_data() {
        return [
            // The defect. Nothing resembling the subject may survive.
            { tag: "oidc subject becomes the server",
              id: "@oidc_fe982c2a4b1d4e0fa1c3d5e7b9081726:bsfchat.com",
              expect: "bsfchat.com" },
            { tag: "dashed uuid too",
              id: "@oidc_fe982c2a-4b1d-4e0f-a1c3-d5e7b9081726:bsfchat.com",
              expect: "bsfchat.com" },
            { tag: "port is kept — it is part of which server",
              id: "@oidc_fe982c2a4b1d4e0fa1c3d5e7b9081726:bsfchat.com:8448",
              expect: "bsfchat.com:8448" },
            // A readable handle is what people quote at each other, so a
            // non-OIDC server sees exactly what it always saw.
            { tag: "handle is unchanged", id: "@josh:bsfchat.com",
              expect: "@josh:bsfchat.com" },
            { tag: "readable oidc subject is a handle", id: "@oidc_josh:bsfchat.com",
              expect: "@oidc_josh:bsfchat.com" },
            // Not an mxid: pass it through rather than inventing structure.
            { tag: "not an mxid", id: "garbage", expect: "garbage" },
            { tag: "empty", id: "", expect: "" },
            { tag: "no server yet", id: undefined, expect: "" },
        ];
    }
    function test_accountSubtitle(d) {
        compare(UserIdentity.accountSubtitle(d.id), d.expect);
    }

    // The property the whole file exists for, stated once on its own: after
    // this row renders, no fragment of the provider's subject is on screen.
    // A test written only against "equals bsfchat.com" would still pass if
    // somebody later prepended a shortened subject to it.
    function test_noFragmentOfTheSubjectIsEverRendered() {
        var subject = "fe982c2a4b1d4e0fa1c3d5e7b9081726";
        var id = "@oidc_" + subject + ":bsfchat.com";
        var rendered = UserIdentity.accountTitle(id, "test") + " "
                     + UserIdentity.accountSubtitle(id);

        // Every prefix of the subject down to the length the screenshot
        // showed ("fe982c2" — seven characters and an ellipsis).
        for (var n = 7; n <= subject.length; ++n) {
            verify(rendered.indexOf(subject.substring(0, n)) === -1,
                   "subject prefix of length " + n + " leaked into: " + rendered);
        }
        verify(rendered.indexOf("oidc_") === -1, "provider prefix leaked");
    }

    // The row as ChannelList.qml assembles it, for the account on the
    // screenshots and for an ordinary one.
    function test_theRowForAnOidcAccount() {
        var id = "@oidc_fe982c2a4b1d4e0fa1c3d5e7b9081726:bsfchat.com";
        compare(UserIdentity.accountTitle(id, "test"), "test");
        compare(UserIdentity.accountSubtitle(id), "bsfchat.com");
    }

    function test_theRowForAPasswordAccount() {
        var id = "@josh:bsfchat.com";
        compare(UserIdentity.accountTitle(id, "Josh"), "Josh");
        compare(UserIdentity.accountSubtitle(id), "@josh:bsfchat.com");
    }
}
