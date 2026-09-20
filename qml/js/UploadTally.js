// The composer's in-flight upload bookkeeping, as pure functions over a
// value. MessageInput.qml holds the value in a QML property and derives its
// disabled "Uploading…" state from it; tests/qml/tst_uploadtally.qml pins the
// rules here rather than a transcription of them.
//
// It lives out here for the reason ChannelSelection.js does: MessageInput.qml
// imports the BSFChat module, which is compiled into the app binary and cannot
// be loaded from a test executable, so the only way to get these rules under
// test at all is to take them out of the component. Until this split, the
// nearest thing to a test was a hand-written C++ copy of the arithmetic in
// tests/test_composer_upload_lock.cpp — which can only ever pin what somebody
// believed the QML did.
//
// ── Why a tally is a hazard at all ───────────────────────────────────────
//
// The composer counts uploads it started and decrements on the connection's
// mediaSendCompleted / mediaSendFailed. Both are bare signals: they say an
// upload ended and nothing about WHICH one. So the count is only correct while
// two things hold, and each has been violated in shipped code:
//
//   * ORDER — no decrement arrives before its increment. Broken by the two
//     pre-flight checks in ServerConnection::sendMediaMessage emitting
//     synchronously, between the call and the count on the next line. Fixed
//     in ServerConnection::emitPreflightMediaFailure.
//   * OWNERSHIP — every decrement is for an upload this composer started.
//     Broken by avatar and server-icon uploads sharing mediaSendFailed. Fixed
//     by splitting the signal (ServerConnection::emitAvatarUploadFailed), and
//     broken a second time from the other direction by a caller that started
//     a composer upload without telling the composer — see
//     MobileMain.qml's share-intent handler.
//
// Both presented as a stuck or prematurely-unlocked message box rather than an
// error, because the decrement was silently clamped at zero. `finished` below
// still clamps — a negative count would disable the composer by arithmetic,
// which is a worse failure than the one being reported — but it now COUNTS the
// clamp, and MessageInput warns on it. An unmatched decrement is a bug in
// somebody's wiring; it must not be free.
//
// ── The one legitimate unmatched decrement ───────────────────────────────
//
// `contextChanged` is called when the composer's room changes. Uploads that
// were in flight were started against the room the user just left, and the
// composer must not stay locked for them, so the count is dropped outright.
// Their terminal signal still arrives afterwards with nothing left to cancel.
// That is a real unmatched decrement and it is not a bug, so it is credited
// explicitly to `orphaned` and redeemed silently, and everything NOT credited
// is allowed to complain.
//
// That credit is redeemable only on the connection that minted it. On a SERVER
// switch the composer's Connections block re-targets to the new connection, so
// the old connection's terminal signals are never delivered here at all and the
// credits can never be redeemed. Left in the pool they would sit there as a
// standing licence to absorb one future unmatched decrement each, silently, on
// an unrelated server — which is exactly the behaviour the warn exists to
// remove. So `contextChanged` takes whether the CONNECTION changed, and drops
// the pool along with the count when it did.
//
// `unmatched` is never reset. It is a diagnostic: a session that ends with it
// above zero had a wiring bug in it somewhere, whether or not the user noticed.

function empty()
{
    return { inFlight: 0, orphaned: 0, unmatched: 0 };
}

// An upload this composer started.
function started(t)
{
    return { inFlight: t.inFlight + 1, orphaned: t.orphaned, unmatched: t.unmatched };
}

// A terminal signal — mediaSendCompleted or mediaSendFailed. Exactly one
// arrives per upload that reached the wire, and exactly one per upload that
// failed pre-flight (deferred, so never before the caller has counted it).
function finished(t)
{
    if (t.inFlight > 0)
        return { inFlight: t.inFlight - 1, orphaned: t.orphaned, unmatched: t.unmatched };
    if (t.orphaned > 0)
        return { inFlight: 0, orphaned: t.orphaned - 1, unmatched: t.unmatched };
    return { inFlight: 0, orphaned: 0, unmatched: t.unmatched + 1 };
}

// The composer moved to a different room, and possibly to a different
// connection. See the header: the count always goes, the credit only survives
// when the connection that owes it is still the one being listened to.
function contextChanged(t, connectionChanged)
{
    if (connectionChanged)
        return { inFlight: 0, orphaned: 0, unmatched: t.unmatched };
    return { inFlight: 0, orphaned: t.orphaned + t.inFlight, unmatched: t.unmatched };
}

// True when a `finished` was absorbed by neither the count nor the credit —
// i.e. when the caller should say so out loud.
function wasUnmatched(before, after)
{
    return after.unmatched !== before.unmatched;
}

function uploading(t)
{
    return t.inFlight > 0;
}
