import QtQuick
import QtTest
import "../../qml/js/ProfileCardAvatar.js" as ProfileCardAvatar

// The profile card's avatar tile (qml/js/ProfileCardAvatar.js), driven the way
// UserProfileCard.qml drives it.
//
// The bug this pins: the card is a single reused Popup. Clicking member A then
// member B assigned B's userId and display name onto it but left A's
// `profileAvatarUrl` in place, and the reply that should have replaced it
// never came — MatrixClient::getProfile drops the reply on any network error,
// and a member with neither a display name nor a picture is precisely the
// account a homeserver answers 404 for. So B's card showed A's photograph.
//
// What this file does NOT cover, because a unit test cannot: that the card
// actually CALLS avatarUrlOnOpen() when it opens. That is a call site, not a
// value, and it is guarded by source text in test_qml_hygiene.cpp
// (profileCardOpensOnACleanSlate).
TestCase {
    name: "ProfileCardAvatar"

    // Stands in for ServerConnection.resolveMediaUrl: mxc:// -> download URL.
    function resolver(mxc) {
        if (!mxc || mxc.indexOf("mxc://") !== 0) return "";
        return "https://chat.example.org/_matrix/media/v3/download/"
             + mxc.substring("mxc://".length);
    }

    // The card, reduced to the state that decides what the tile shows. The
    // three helpers are called exactly where UserProfileCard.qml calls them.
    // Closures rather than `this`, so the state is real per-card state and not
    // whatever object the call happens to be made through.
    function makeCard() {
        var userId = "";
        var displayName = "";
        var avatarUrl = "";
        return {
            // MemberList.qml / MessageView.qml: assign, then open().
            open: function (uid, dn) {
                userId = uid;
                displayName = dn;
                avatarUrl = ProfileCardAvatar.avatarUrlOnOpen(uid);
            },
            // Connections.onProfileFetched.
            profileFetched: function (uid, dn, avatar) {
                avatarUrl = ProfileCardAvatar.avatarUrlOnProfile(
                    userId, avatarUrl, uid, avatar);
                if (uid === userId) displayName = dn || uid;
            },
            source: function () {
                return ProfileCardAvatar.avatarSource(avatarUrl, resolver);
            },
            initial: function () {
                return ProfileCardAvatar.initial(displayName, userId);
            },
            name: function () { return displayName; }
        };
    }

    readonly property string aliceMxc: "mxc://example.org/alice"
    readonly property string aliceUrl:
        "https://chat.example.org/_matrix/media/v3/download/example.org/alice"
    readonly property string bobMxc: "mxc://example.org/bob"
    readonly property string bobUrl:
        "https://chat.example.org/_matrix/media/v3/download/example.org/bob"

    // ---- avatarUrlOnOpen -------------------------------------------------

    function test_openStartsEmpty() {
        compare(ProfileCardAvatar.avatarUrlOnOpen("@alice:example.org"), "");
        compare(ProfileCardAvatar.avatarUrlOnOpen(""), "");
        compare(ProfileCardAvatar.avatarUrlOnOpen(undefined), "");
    }

    // ---- avatarUrlOnProfile ---------------------------------------------

    function test_avatarUrlOnProfile_data() {
        return [
            { tag: "adopts this user's avatar",
              card: "@bob:e", current: "", uid: "@bob:e", avatar: bobMxc,
              expect: bobMxc },
            { tag: "an empty avatar clears, it does not keep",
              card: "@bob:e", current: aliceMxc, uid: "@bob:e", avatar: "",
              expect: "" },
            { tag: "undefined avatar clears",
              card: "@bob:e", current: aliceMxc, uid: "@bob:e", avatar: undefined,
              expect: "" },
            { tag: "someone else's reply is ignored",
              card: "@bob:e", current: bobMxc, uid: "@alice:e", avatar: aliceMxc,
              expect: bobMxc },
            { tag: "someone else's reply cannot fill an empty tile",
              card: "@bob:e", current: "", uid: "@alice:e", avatar: aliceMxc,
              expect: "" },
            { tag: "no user on the card takes nothing",
              card: "", current: "", uid: "@alice:e", avatar: aliceMxc,
              expect: "" },
        ];
    }
    function test_avatarUrlOnProfile(d) {
        compare(ProfileCardAvatar.avatarUrlOnProfile(d.card, d.current, d.uid, d.avatar),
                d.expect);
    }

    // ---- avatarSource ----------------------------------------------------

    function test_avatarSource_data() {
        return [
            { tag: "resolves an mxc", url: bobMxc, expect: bobUrl },
            { tag: "no avatar",       url: "",     expect: "" },
            { tag: "undefined",       url: undefined, expect: "" },
            // An unresolvable URI must fall back to the initial rather than
            // leave a tile with neither picture nor letter in it.
            { tag: "unresolvable",    url: "not-an-mxc", expect: "" },
        ];
    }
    function test_avatarSource(d) {
        compare(ProfileCardAvatar.avatarSource(d.url, resolver), d.expect);
    }

    function test_avatarSourceWithoutAServer() {
        // No active server: nothing to resolve against, so initials.
        compare(ProfileCardAvatar.avatarSource(bobMxc, null), "");
    }

    // ---- initial ---------------------------------------------------------

    function test_initial_data() {
        return [
            { tag: "display name", dn: "bob",  uid: "@bob:e",   expect: "B" },
            { tag: "mxid punctuation skipped", dn: "", uid: "@ada:e", expect: "A" },
            { tag: "leading punctuation in a name", dn: "!!zed", uid: "@z:e", expect: "Z" },
            { tag: "nothing usable", dn: "", uid: "", expect: "?" },
            { tag: "punctuation only", dn: "***", uid: "", expect: "?" },
        ];
    }
    function test_initial(d) {
        compare(ProfileCardAvatar.initial(d.dn, d.uid), d.expect);
    }

    // ---- The owner's report, as a sequence -------------------------------

    // A has a picture, B has none: B's card must show B's initial, not A's
    // picture — even though B's profile reply never arrives at all.
    function test_secondMemberWithNoPictureNeverWearsTheFirstsPhoto() {
        var card = makeCard();

        card.open("@alice:e", "alice");
        card.profileFetched("@alice:e", "alice", aliceMxc);
        compare(card.source(), aliceUrl);

        // Click B. The homeserver 404s the profile of an account with nothing
        // set, so nothing at all comes back for B.
        card.open("@bob:e", "bob");
        compare(card.source(), "");
        compare(card.initial(), "B");
    }

    // Same, when B's reply does arrive and says "no avatar".
    function test_anEmptyAvatarReplyClearsTheTile() {
        var card = makeCard();
        card.open("@alice:e", "alice");
        card.profileFetched("@alice:e", "alice", aliceMxc);

        card.open("@bob:e", "bob");
        card.profileFetched("@bob:e", "bob", "");
        compare(card.source(), "");
    }

    // B does have a picture: it must win, before and after the reply.
    function test_secondMemberWithAPictureGetsTheirOwn() {
        var card = makeCard();
        card.open("@alice:e", "alice");
        card.profileFetched("@alice:e", "alice", aliceMxc);

        card.open("@bob:e", "bob");
        compare(card.source(), "");        // nothing known yet: initial, not A
        card.profileFetched("@bob:e", "bob", bobMxc);
        compare(card.source(), bobUrl);
    }

    // A -> B fast enough that A's reply lands while B is on screen, then A
    // again. The late reply must not paint A onto B's card, and must not
    // count as B's answer either.
    function test_aLateReplyForTheFirstMemberIsDiscarded() {
        var card = makeCard();

        card.open("@alice:e", "alice");   // request for A goes out
        card.open("@bob:e", "bob");       // dismissed, clicked B
        card.profileFetched("@alice:e", "alice", aliceMxc);  // A's reply, late
        compare(card.source(), "");
        compare(card.name(), "bob");

        card.profileFetched("@bob:e", "bob", bobMxc);
        compare(card.source(), bobUrl);

        // Back to A: B's picture must not linger either.
        card.open("@alice:e", "alice");
        compare(card.source(), "");
        card.profileFetched("@alice:e", "alice", aliceMxc);
        compare(card.source(), aliceUrl);
    }

    // Reopening the same member is not a special case — it still starts blank
    // and still ends up with their picture.
    function test_reopeningTheSameMember() {
        var card = makeCard();
        card.open("@alice:e", "alice");
        card.profileFetched("@alice:e", "alice", aliceMxc);
        card.open("@alice:e", "alice");
        compare(card.source(), "");
        card.profileFetched("@alice:e", "alice", aliceMxc);
        compare(card.source(), aliceUrl);
    }
}
