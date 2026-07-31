#pragma once

// WHERE THE MESSAGE LIST IS SUPPOSED TO BE, AS ARITHMETIC.
//
// MessageView.qml used to answer three separate questions inline, in QML, with
// no way to test any of them:
//
//   1. "am I pinned to the bottom?"        (a tolerance band on contentY)
//   2. "where do I land when a room opens?" (the unread divider, or the end)
//   3. "does this model change move me?"    (append vs prepend vs re-enter)
//
// Every historical scroll-position bug in this client has been a wrong answer
// to one of those, not a missing call to positionViewAtEnd() — so the answers
// live here, where a test can pin them, and the QML asks rather than decides.
//
// Nothing in here touches Qt, a ListView, or an event loop: it is pure
// arithmetic over numbers the view already has.

namespace bsfchat::client {

// ── 1. Stick-to-bottom ──────────────────────────────────────────────────────
//
// True when the viewport is within `tolerance` px of the end of the content.
//
// The band is two-sided ON PURPOSE. A one-sided `dist <= tolerance` test also
// reports "at the bottom" for a contentY parked PAST the valid end, which is
// exactly what transient layout churn produces (an image resolves its
// intrinsic size, contentHeight shrinks under a contentY that was already at
// the old end). Treating that as "at the bottom" latches a position the view
// can never actually reach, and every later auto-scroll then agrees there is
// nothing to do.
//
// Content shorter than the viewport is always "at the end" — there is nowhere
// else to be, and reporting false there makes the jump-to-latest button appear
// over a half-empty channel.
inline bool isPinnedToEnd(double contentHeight, double contentY,
                          double viewportHeight, double tolerance)
{
    if (contentHeight <= viewportHeight) return true;
    if (tolerance < 0) tolerance = 0;
    const double dist = contentHeight - contentY - viewportHeight;
    return dist >= -tolerance && dist <= tolerance;
}

// ── 2. Where a freshly-entered room lands ───────────────────────────────────

struct RestoreTarget {
    enum class Kind {
        // Scroll to the newest message. The default, and what the user means
        // by "open the channel".
        End,
        // Put `index` at the top of the viewport: there are genuinely-unread
        // messages and the user should read forward from the first of them.
        Row,
    };

    Kind kind = Kind::End;
    int index = -1;

    friend bool operator==(const RestoreTarget&, const RestoreTarget&) = default;
};

// `dividerIndex` — row of the first unread message, or -1 when there is none
//   (or when the anchoring event is not in the loaded timeline).
//
// Row 0 deliberately falls back to the end: a divider on the very first loaded
// row is indistinguishable from "top of the loaded history", so honouring it
// parks the user at the top of a back-paginated page that only LOOKS like the
// start of the unread run. Showing them the newest message is the honest
// answer when we cannot prove where unread begins.
//
// An index past the end of the model is a stale anchor from a previous room —
// same fallback, for the same reason. The range check covers the empty model
// too (every index is >= 0 rows), so there is no separate guard for it: a
// fourth branch nothing can reach is a branch nothing can test.
inline RestoreTarget chooseRestoreTarget(int rowCount, int dividerIndex)
{
    if (dividerIndex <= 0) return {};
    if (dividerIndex >= rowCount) return {};
    return {RestoreTarget::Kind::Row, dividerIndex};
}

// ── 3. What a change to the model means for scroll position ─────────────────

enum class PositionPolicy {
    // Leave the user exactly where they are.
    Preserve,
    // Chase the end of the content as it grows.
    FollowEnd,
    // Brand-new (server, room) context: run the initial placement, which picks
    // between the unread divider and the end via chooseRestoreTarget.
    Reenter,
};

// `contextChanged`   — the view is still inside the initial-placement window
//                      for a newly-entered room.
// `paginating`       — older messages were just PREPENDED. This wins over
//                      everything: a user who scrolled up far enough to
//                      trigger back-pagination must not be thrown to the
//                      bottom by the batch they asked for, and must not be
//                      re-anchored by the initial-placement path either if a
//                      page happens to land inside the entry window.
// `pinnedToEnd`      — the latched answer from isPinnedToEnd, sampled BEFORE
//                      the content grew.
// `followLatch`      — the local user just sent a message and has not scrolled
//                      away since; they authored it and expect to see it land,
//                      even from a position that is not the bottom.
//
// The one property worth stating out loud, because breaking it is the usual
// cost of "fixing" a scroll bug: with `pinnedToEnd` false and `followLatch`
// false, the answer is Preserve. A user who has scrolled up is never moved.
inline PositionPolicy positionPolicyForModelChange(bool contextChanged,
                                                   bool paginating,
                                                   bool pinnedToEnd,
                                                   bool followLatch)
{
    if (paginating) return PositionPolicy::Preserve;
    if (contextChanged) return PositionPolicy::Reenter;
    if (pinnedToEnd || followLatch) return PositionPolicy::FollowEnd;
    return PositionPolicy::Preserve;
}

// ── 4. Intent vs. instantaneous geometry ────────────────────────────────────
//
// THE DEFECT BEHIND THREE SEPARATE REPORTS, AND WHY EVERY FIX BEFORE THIS ONE
// FAILED.
//
// The view used to keep ONE flag, `atBottom`, and make it do two jobs:
//
//   (a) the record of whether the user wants to follow the newest message, and
//   (b) a sample of whether the viewport is, right now, at the end of the
//       content.
//
// (b) was re-latched into the same variable on EVERY contentY tick — and most
// contentY ticks are produced by layout, not by the user. Layout emits them
// from transiently inconsistent (contentHeight, contentY, viewportHeight)
// triples: the viewport resizes a frame before delegates re-wrap, a video card
// renders small and grows when its metadata lands, an image resolves its
// intrinsic size, a link preview appears. Any ONE such sample landing outside
// the tolerance band destroyed (a) permanently — and since the model does not
// change in any of those scenarios, nothing ever restored it. The view then
// sat wherever it happened to be, with the jump-to-latest chevron showing,
// which is precisely what all three bug reports describe.
//
// That is also why a better-timed one-shot `positionViewAtEnd()` could never
// have worked: the position was correct when it was set and stale one frame
// later, and the flag that would have re-run it had already been cleared.
//
// So the two jobs are now two flags, and this function is the rule that keeps
// them apart: only a USER-DRIVEN contentY change is evidence about intent.
// Layout may move the view; it may not change the user's mind.
//
// `userDriven` is true for a drag, a flick, a wheel tick or a scrollbar-thumb
// drag — the caller resolves that from real input signals, never by guessing
// from the geometry.
inline bool followEndAfterContentYSample(bool followEnd, bool userDriven,
                                         bool liveAtEnd)
{
    return userDriven ? liveAtEnd : followEnd;
}

// ── 5. Viewport geometry changes ────────────────────────────────────────────
//
// A window resize, a fullscreen enter/exit, a side-panel toggle. The model is
// completely unchanged, so none of the model-change paths fire at all — this
// was simply an unhandled case, and it is why exiting fullscreen video left
// the list scrolled up.
//
// Same two directions as everywhere else: follow the end if that is what the
// user was doing, and otherwise do not move them.
inline PositionPolicy positionPolicyForGeometryChange(bool followEnd)
{
    return followEnd ? PositionPolicy::FollowEnd : PositionPolicy::Preserve;
}

} // namespace bsfchat::client
