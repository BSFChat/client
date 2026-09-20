import QtQuick
import QtTest
import "../../qml/js/UploadTally.js" as UploadTally

// The composer's in-flight upload bookkeeping — U-H6's counter.
//
// This exercises qml/js/UploadTally.js, the file MessageInput.qml imports, not
// a copy of its rules. That distinction is the point of the file existing:
// until it was split out, the arithmetic lived inline in MessageInput.qml,
// which imports the BSFChat module and therefore cannot be loaded from any
// test binary, and the closest thing to a test was a hand-written C++
// transcription in tests/test_composer_upload_lock.cpp. A transcription pins
// what somebody believed the QML did, which is exactly the thing that was
// wrong three times running.
//
// The wiring around it — which signals reach the composer, and when — is still
// C++'s to pin, in test_composer_upload_lock.cpp. This file is only about what
// the tally does with what it is handed.
TestCase {
    id: tc
    name: "UploadTally"

    // ── The ordinary lifecycle ───────────────────────────────────────

    function test_an_upload_locks_the_composer_until_it_ends() {
        var t = UploadTally.empty();
        verify(!UploadTally.uploading(t));

        t = UploadTally.started(t);
        verify(UploadTally.uploading(t), "the composer must be locked while a file is on the wire");

        t = UploadTally.finished(t);
        verify(!UploadTally.uploading(t));
        compare(t.unmatched, 0);
    }

    function test_n_files_unlock_only_on_the_last() {
        // MessageView's drop area: N sends, then N counts, then N terminal
        // signals. The composer must not come back after the first one.
        var t = UploadTally.empty();
        for (var i = 0; i < 3; ++i) t = UploadTally.started(t);
        compare(t.inFlight, 3);

        t = UploadTally.finished(t);
        verify(UploadTally.uploading(t), "two files are still going");
        t = UploadTally.finished(t);
        verify(UploadTally.uploading(t), "one file is still going");
        t = UploadTally.finished(t);
        verify(!UploadTally.uploading(t));
        compare(t.unmatched, 0);
    }

    // ── The clamp, and why it now says something ─────────────────────

    function test_an_unmatched_decrement_is_clamped_and_counted() {
        // The shape of both fixed defects: a decrement with nothing to take it
        // from. A pre-flight failure landing before the increment (defect 1),
        // or an avatar's failure on the composer's signal (defect 2), or a
        // caller that started an upload without counting it (MobileMain's
        // share intent). The clamp must stay — a negative count disables the
        // composer by arithmetic, which is worse than the bug being reported —
        // but it must not be free.
        var t = UploadTally.empty();
        var after = UploadTally.finished(t);

        compare(after.inFlight, 0, "the count must never go negative");
        compare(after.unmatched, 1);
        verify(UploadTally.wasUnmatched(t, after), "the caller must be told to complain");
        verify(!UploadTally.uploading(after));
    }

    function test_a_matched_decrement_says_nothing() {
        var t = UploadTally.started(UploadTally.empty());
        var after = UploadTally.finished(t);
        verify(!UploadTally.wasUnmatched(t, after));
        compare(after.unmatched, 0);
    }

    function test_unmatched_decrements_accumulate() {
        // A diagnostic, not a latch: three stray decrements are three bugs'
        // worth of evidence, not one.
        var t = UploadTally.empty();
        for (var i = 0; i < 3; ++i) t = UploadTally.finished(t);
        compare(t.unmatched, 3);
        compare(t.inFlight, 0);
    }

    function test_a_stray_decrement_cannot_drain_a_real_upload() {
        // The damaging form the clamp could never have caught, and the reason
        // avatarUploadFailed exists: the count is above zero, so the stray
        // lands on somebody's real upload. The tally cannot tell them apart —
        // nothing at this level can — so this pins the consequence that makes
        // scoping the signals a C++ responsibility rather than a QML one.
        var t = UploadTally.started(UploadTally.empty());
        t = UploadTally.finished(t);      // the stranger's
        verify(!UploadTally.uploading(t), "documented, not endorsed: this is why the signal was split");
        compare(t.unmatched, 0, "and it is silent, which is why it had to be split rather than warned about");
    }

    // ── The room switch: the one legitimate unmatched decrement ──────

    function test_a_room_switch_unlocks_and_still_balances() {
        // Leaving a channel mid-upload must not leave the composer locked in
        // the channel you arrive at. The upload still reports back on the same
        // connection afterwards, and that decrement is expected.
        var t = UploadTally.started(UploadTally.empty());
        t = UploadTally.contextChanged(t, false);   // room only

        verify(!UploadTally.uploading(t), "the new channel's composer is not the old channel's upload");
        compare(t.orphaned, 1);

        var after = UploadTally.finished(t);
        verify(!UploadTally.wasUnmatched(t, after),
               "an orphan reporting in is expected, not a bug");
        compare(after.unmatched, 0);
        compare(after.orphaned, 0);
    }

    function test_an_orphan_credit_is_spent_once() {
        var t = UploadTally.contextChanged(UploadTally.started(UploadTally.empty()), false);
        t = UploadTally.finished(t);                 // the orphan
        var after = UploadTally.finished(t);         // one too many
        compare(after.unmatched, 1, "the credit covers exactly the upload it was minted for");
    }

    function test_a_room_switch_does_not_credit_an_upload_started_after_it() {
        // The credit is for what was in flight AT the switch. An upload
        // started in the new room is the new room's, and its completion must
        // decrement the count rather than quietly spend a credit.
        var t = UploadTally.contextChanged(UploadTally.started(UploadTally.empty()), false);
        t = UploadTally.started(t);                  // a new one, here
        compare(t.inFlight, 1);
        compare(t.orphaned, 1);

        t = UploadTally.finished(t);
        compare(t.inFlight, 0, "the live upload's own completion must clear the lock");
        compare(t.orphaned, 1, "and must not have been paid for out of the credit");
    }

    // ── The server switch: a credit that can never be redeemed ───────

    function test_a_server_switch_drops_the_credit_with_the_count() {
        // The bug this test exists for.
        //
        // MessageInput's Connections blocks target serverManager.activeServer.
        // On a SERVER switch they re-target, so the old connection's terminal
        // signals are never delivered to this composer at all — the credits
        // minted for its in-flight uploads can never be redeemed. Kept in the
        // pool they become a standing licence to absorb one future unmatched
        // decrement each, silently, on an unrelated server: the clamp's
        // original sin, laundered through a mechanism added to fix it.
        var t = UploadTally.empty();
        for (var i = 0; i < 2; ++i) t = UploadTally.started(t);
        t = UploadTally.contextChanged(t, true);     // connection changed too

        verify(!UploadTally.uploading(t));
        compare(t.orphaned, 0,
                "credits are redeemable only on the connection that owes them");

        // Now a genuine wiring bug on the new server. It must be reported, not
        // paid for out of the old server's leftovers.
        var after = UploadTally.finished(t);
        compare(after.unmatched, 1);
        verify(UploadTally.wasUnmatched(t, after));
    }

    function test_returning_to_a_server_does_not_resurrect_its_credits() {
        // A → B → A. The uploads left on A finished while nobody here was
        // listening; coming back must not make the composer expect them.
        var t = UploadTally.started(UploadTally.empty());
        t = UploadTally.contextChanged(t, true);     // to B
        t = UploadTally.contextChanged(t, true);     // back to A
        compare(t.orphaned, 0);
        compare(t.inFlight, 0);
        compare(t.unmatched, 0, "switching servers is not itself an error");
    }

    function test_a_server_switch_keeps_the_diagnostic() {
        // inFlight and orphaned are state; unmatched is evidence. Clearing
        // state must not clear the record that something went wrong.
        var t = UploadTally.finished(UploadTally.empty());
        compare(t.unmatched, 1);
        compare(UploadTally.contextChanged(t, true).unmatched, 1);
        compare(UploadTally.contextChanged(t, false).unmatched, 1);
    }

    // ── The tally is a value ─────────────────────────────────────────

    function test_every_operation_returns_a_new_object() {
        // MessageInput binds `uploading` to a `property var`, and QML only
        // re-evaluates that binding when the property is ASSIGNED. A function
        // here that mutated its argument in place would leave the composer
        // showing a stale lock state — which looks exactly like U-H6.
        var t = UploadTally.empty();
        var started = UploadTally.started(t);
        verify(t !== started);
        compare(t.inFlight, 0, "the input must not have been mutated");
        compare(started.inFlight, 1);

        var finished = UploadTally.finished(started);
        verify(started !== finished);
        compare(started.inFlight, 1);

        var switched = UploadTally.contextChanged(started, false);
        verify(started !== switched);
        compare(started.inFlight, 1);
        compare(started.orphaned, 0);
    }
}
