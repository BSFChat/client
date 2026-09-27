import QtQuick
import QtTest
// The real module out of bsfchat-lib's compiled-in resources — see
// tst_connectionbanner_real.qml for what that import buys and why it could
// not be written before PR #34.
import BSFChat

// MemberList.qml — the file that ships, instantiated, fed by the REAL
// MemberListModel, and asked what its rows announce.
//
// WHY THIS IS A REAL-COMPONENT TEST AND NOT A SOURCE SCAN
//
// The C++ side of speaking state is covered in tests/test_models.cpp: the
// threshold, the hold, the release edge, the dataChanged roles. None of that
// says the member row READS the role, and nothing says it reads it into the
// accessible name rather than only into the ring. A screen-reader user gets
// exactly one of those two, and it is the one a text scan cannot see: a grep
// for "isSpeaking" in MemberList.qml passes over a binding that is present
// and wrong, which is the failure mode docs/accessibility.md's guard
// documents about itself.
//
// So every case here reads `Accessible.name` off the live row after driving
// the real model, which is as close to what VoiceOver is handed as anything
// short of a screen reader. It is NOT a substitute for one — see the PR.
//
// The environment (AppSettings/appSettings, serverManager and its member
// surface) is in tests/qml_components_test_main.cpp.
TestCase {
    id: tc
    name: "MemberListReal"
    when: windowShown
    width: 260
    height: 400
    visible: true

    MemberList {
        id: memberList
        anchors.fill: parent
    }

    // ── helpers ──────────────────────────────────────────────────────

    function conn() {
        return serverManager.activeServer;
    }

    function descendants(item) {
        var out = [];
        if (!item || item.children === undefined)
            return out;
        for (var i = 0; i < item.children.length; ++i) {
            var c = item.children[i];
            out.push(c);
            out = out.concat(descendants(c));
        }
        return out;
    }

    // The row items, found by SHAPE rather than by index into the tree: the
    // delegate's named node is the Rectangle that carries
    // Accessible.role === Accessible.ListItem, which in this component is
    // the row and nothing else. Indexing would silently retarget the
    // assertions the next time a sibling is added — which this change adds
    // one of.
    function rows() {
        var out = [];
        var d = descendants(memberList);
        for (var i = 0; i < d.length; ++i) {
            if (d[i].Accessible.role === Accessible.ListItem)
                out.push(d[i]);
        }
        return out;
    }

    function rowNamed(prefix) {
        var r = rows();
        for (var i = 0; i < r.length; ++i) {
            if (String(r[i].Accessible.name).indexOf(prefix) === 0)
                return r[i];
        }
        return null;
    }

    function init() {
        serverManager.resetForTest();
    }

    function cleanup() {
        conn().clearVoiceLevelsForTest();
    }

    // ── 1. the shipped component comes up on the real model ──────────

    function test_a_the_real_component_loaded() {
        verify(memberList !== null);
        verify(memberList.toString().indexOf("MemberList") >= 0,
               "expected a MemberList instance, got " + memberList);
        // The model is the shipped MemberListModel, not a ListModel stand-in.
        var m = conn().memberListModel;
        verify(m !== null);
        verify(m.rowCount() !== undefined);

        conn().addMemberForTest("@alice:test", "Alice");
        tryVerify(function() { return rows().length === 1; });
    }

    // ── 2. a silent member says nothing about speech ─────────────────

    // The negative case matters as much as the positive one: ", not
    // speaking" on every row in a fifty-person list would be the other way
    // to get this wrong.
    function test_b_a_silent_member_is_not_described_as_speaking() {
        conn().addMemberForTest("@alice:test", "Alice");
        tryVerify(function() { return rows().length === 1; });

        var row = rows()[0];
        compare(String(row.Accessible.name).indexOf("speaking"), -1,
                "a silent row must not mention speech, got: "
                + row.Accessible.name);
        // …and it is still named at all. An empty name is the other failure.
        verify(String(row.Accessible.name).indexOf("Alice") === 0);
    }

    // ── 3. the announcement: state is in the NAME ────────────────────

    // The whole point of the change. A level over the floor reaches the row
    // through the real model, and the row's accessible name — the string a
    // screen reader reads when the user navigates to it — says so.
    function test_c_a_speaking_member_is_announced_in_the_row_name() {
        conn().addMemberForTest("@alice:test", "Alice");
        tryVerify(function() { return rows().length === 1; });
        var row = rows()[0];

        conn().setVoiceLevelForTest("@alice:test", 0.3);
        tryVerify(function() {
            return String(row.Accessible.name).indexOf("speaking") >= 0;
        }, 2000, "the row name never picked up speaking state: "
                 + row.Accessible.name);

        // Immediately after the name, not tacked on the end behind presence
        // and role. In a call this is the fact the user is navigating the
        // list to find; it should not be the fifth clause.
        compare(String(row.Accessible.name).indexOf("Alice, speaking"), 0);
    }

    // The binding is live in BOTH directions. A name that latches on is a
    // screen reader telling a user somebody is still talking after they
    // stopped, which is worse than saying nothing.
    function test_d_the_name_follows_the_model_back_to_silence() {
        conn().addMemberForTest("@alice:test", "Alice");
        tryVerify(function() { return rows().length === 1; });
        var row = rows()[0];

        conn().setVoiceLevelForTest("@alice:test", 0.3);
        tryVerify(function() {
            return String(row.Accessible.name).indexOf("speaking") >= 0;
        });

        // The immediate release — what teardownVoiceSession() does. The hold
        // expiry is covered in test_models.cpp with an injected clock; what
        // is being checked here is that the row follows the model, not how
        // long the model waits.
        conn().clearVoiceLevelsForTest();
        tryVerify(function() {
            return String(row.Accessible.name).indexOf("speaking") < 0;
        }, 2000, "the row name latched on speaking: " + row.Accessible.name);
    }

    // A level under the floor must not reach the name. The threshold lives
    // in the model; this states that no second one in the QML overrides it.
    function test_e_room_tone_does_not_reach_the_name() {
        conn().addMemberForTest("@alice:test", "Alice");
        tryVerify(function() { return rows().length === 1; });
        var row = rows()[0];

        conn().setVoiceLevelForTest("@alice:test", 0.01);
        wait(50);
        compare(String(row.Accessible.name).indexOf("speaking"), -1);
    }

    // ── 4. one speaker is one row ────────────────────────────────────

    function test_f_only_the_speaker_is_announced_as_speaking() {
        conn().addMemberForTest("@alice:test", "Alice");
        conn().addMemberForTest("@bob:test", "Bob");
        tryVerify(function() { return rows().length === 2; });

        conn().setVoiceLevelForTest("@bob:test", 0.3);

        tryVerify(function() {
            var b = rowNamed("Bob");
            return b !== null && String(b.Accessible.name).indexOf("speaking") >= 0;
        });
        var alice = rowNamed("Alice");
        verify(alice !== null);
        compare(String(alice.Accessible.name).indexOf("speaking"), -1,
                "Alice is silent but her row says: " + alice.Accessible.name);
    }

    // ── 5. the rest of the sentence survives ─────────────────────────

    // Speaking is inserted into a name that already carried presence and, for
    // a bot, bot-ness. Inserting a clause into a string built by five
    // successive .arg() calls is exactly where one of the others gets lost.
    function test_g_speaking_does_not_displace_the_rest_of_the_row() {
        conn().addMemberForTest("@alice:test", "Alice");
        tryVerify(function() { return rows().length === 1; });
        var row = rows()[0];

        var before = String(row.Accessible.name);
        // The fake reports everyone offline, which is the clause that follows
        // speaking in the assembled sentence.
        verify(before.indexOf("offline") >= 0,
               "expected presence in the row name, got: " + before);

        conn().setVoiceLevelForTest("@alice:test", 0.3);
        tryVerify(function() {
            return String(row.Accessible.name).indexOf("speaking") >= 0;
        });
        var after = String(row.Accessible.name);
        verify(after.indexOf("Alice") >= 0);
        verify(after.indexOf("offline") >= 0,
               "presence fell out of the name when speaking was added: " + after);
    }

    // ── 6. the row is ONE announcement, still ────────────────────────

    // docs/accessibility §7. The ring is a picture of what the name already
    // says; if it were reachable in its own right the reader would stop on
    // an unnamed node inside every speaking row. Asserted as "no row gained
    // a second named node", which is the property that matters rather than
    // the identity of the ring item.
    function test_h_the_speaking_ring_is_not_a_second_node() {
        conn().addMemberForTest("@alice:test", "Alice");
        tryVerify(function() { return rows().length === 1; });

        conn().setVoiceLevelForTest("@alice:test", 0.3);
        tryVerify(function() {
            return String(rows()[0].Accessible.name).indexOf("speaking") >= 0;
        });

        // Still exactly one ListItem for the one member.
        compare(rows().length, 1);

        // And nothing inside the row announces itself separately. The row
        // itself is excluded by identity; everything below it that is not
        // ignored and carries a name would be a second stop.
        var row = rows()[0];
        var d = descendants(row);
        for (var i = 0; i < d.length; ++i) {
            var n = String(d[i].Accessible.name || "");
            verify(n.length === 0 || d[i].Accessible.ignored === true,
                   "a child of the row announces itself separately: " + n);
        }
    }

    // ── 7. the ring itself ───────────────────────────────────────────

    // The sighted half. Found by shape — a transparent Rectangle with an
    // accent border, inside the row — and checked for the property the
    // layout depends on: it fades, it does not appear, so turning it on
    // cannot move the name beside it.
    function test_i_the_ring_lights_without_moving_the_row() {
        conn().addMemberForTest("@alice:test", "Alice");
        tryVerify(function() { return rows().length === 1; });
        var row = rows()[0];
        waitForRendering(memberList);

        function ring() {
            var d = descendants(row);
            for (var i = 0; i < d.length; ++i) {
                var it = d[i];
                if (it.border === undefined || it.radius === undefined)
                    continue;
                if (String(it.border.color) === String(Theme.accent)
                        && it.border.width === 2)
                    return it;
            }
            return null;
        }

        var r = ring();
        verify(r !== null, "no speaking ring found in the row");
        tryCompare(r, "opacity", 0);

        var heightBefore = row.height;
        conn().setVoiceLevelForTest("@alice:test", 0.3);
        tryCompare(r, "opacity", 1);
        // The reason the ring is on negative margins with a constant size:
        // lighting it must not resize the row or reflow the list.
        compare(row.height, heightBefore);
    }
}
