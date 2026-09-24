#pragma once

// How much history a channel loads: counted in ROWS THE USER CAN SEE, not in
// events the server sent.
//
// ── The incident (2026-09-22) ────────────────────────────────────────────
//
// The owner opened #notifications, saw exactly one message — a bot's level-up
// notice — and asked whether the bot was deleting messages. It was not. The
// production database held 832 m.room.message events and ZERO redactions.
// 786 of the 832 were m.replace EDITS: an integration bot keeps a live
// "who's online" board as ONE message and edits it roughly every 2.2
// minutes. Only 46 events were real, visible messages, and the most recent
// 20 events in the room were all edits.
//
// Opening a room asked /messages for the newest 50 RAW events and stopped.
// An edit is never a row of its own (MessageModel folds it into its target,
// and drops it when the target is not loaded), so a window of 50 events that
// is ~94% board edits renders as one or two rows. Back-pagination was only
// ever triggered by scrolling near the top — and a list holding one message
// is not scrollable, so the trigger could never fire. The channel looked
// empty, indefinitely, with nothing wrong on the server at all.
//
// ── The rule ─────────────────────────────────────────────────────────────
//
// A history load is a FILL: it keeps paginating backwards until either
//   * enough renderable rows have been gathered (Filled),
//   * the server says there is nothing older (StartOfRoom), or
//   * the page cap is hit (PageCap) — so a room that is nothing but edits,
//     reactions or state cannot turn one click into an unbounded crawl.
// "Renderable" is decided by MessageModel (it is exactly "would become a
// row"); this header only does the counting, which is why it can be tested
// without a model, a network or an event loop.
//
// Two caps, because there are two ways to loop:
//   * kHistoryMaxPagesPerFill bounds ONE fill, whatever started it.
//   * kHistoryMaxAutoPagesPerOpen bounds everything the client does on its
//     own for one room visit — the open fill plus every "the view is not
//     scrollable yet, fetch more" fill MessageView asks for. Without it the
//     viewport trigger would restart a capped fill forever in a room of pure
//     edits. Once it is spent, only the user can ask for more (scroll to the
//     top, or the "Load older messages" button MessageView shows when the
//     list cannot scroll). A user gesture is not budgeted: it is one click,
//     and each fill it starts is still capped by kHistoryMaxPagesPerFill.
//
// Header-only and Qt-Core-free so tests/test_models.cpp drives it directly.

namespace bsfchat::client {

// Rows an OPEN fill aims for: a tall desktop timeline shows ~15-20 rows, so
// 30 is a screenful plus a margin to scroll into. MessageView still asks for
// more if the rows it got do not fill the viewport (very short messages, a
// very tall window) — the target is a floor, not the only safeguard.
inline constexpr int kHistoryOpenTargetRows = 30;
// Rows a scroll-to-top / viewport / jump fill adds on top of what is loaded.
inline constexpr int kHistoryMoreTargetRows = 20;
// Page sizes. The first page of an open stays at the 50 it always was, so a
// normal channel costs exactly the one request it did before. Follow-up pages
// are only ever requested for a room whose pages are mostly invisible, where
// a bigger page means fewer round trips for the same rows.
inline constexpr int kHistoryFirstPageLimit = 50;
inline constexpr int kHistoryFollowPageLimit = 100;
// One fill fetches at most this many pages: 50 + 9 x 100 = 950 raw events.
inline constexpr int kHistoryMaxPagesPerFill = 10;
// ── The second incident (2026-09-23) ─────────────────────────────────────
//
// An OPEN gets a much smaller cap than that, and here is why. Production,
// measured: two channels of 1300+ events each contained ZERO renderable rows
// in their newest 150 — they are voice channels, and the server writes an
// m.call.member on every join, leave and reap sweep, 791 of them in the
// newest 2000 events server-wide. Opening either ran the fill to the 10-page
// cap, then MessageView's "the list does not fill the viewport" trigger spent
// the rest of a 20-page budget, and after up to TWENTY serial round trips
// (p50 117 ms, p90 679 ms) the channel displayed nothing at all. The owner
// reported it as "slow to load" from a phone.
//
// The cap cannot be "enough pages to crawl any room", because a room can
// always be sparser than the cap. It has to be "enough for a room with a
// normal message density", and the client has to be honest the moment it
// stops: MessageView already shows "Nothing recent to show" with a "Load
// older messages" button once the automatic budget is spent, so a SMALL
// budget is what puts that in front of the user instead of a spinner.
//
// 3 pages is 50 + 100 + 100 = 250 raw events. #notifications — the room the
// row-counting fill was written for — now renders 50 rows in its newest 50
// and is satisfied by page one; at its worst (832 events, 46 visible) it
// still had a visible row every ~18 events, so 250 covers it several times
// over. A room that is sparser than that is not a room a further seven
// requests would have rescued.
inline constexpr int kHistoryOpenMaxPages = 3;
// Pages the client may fetch on its OWN in one room visit: the open's three,
// plus one top-up for a viewport the open did not fill. Past this only the
// user can ask (scroll to the top, or the button), and the button appearing
// IS the client admitting it stopped.
inline constexpr int kHistoryMaxAutoPagesPerOpen = 4;

enum class HistoryFillKind {
    Open,     // the room was just opened
    Viewport, // MessageView: the loaded rows do not fill the viewport
    Gesture,  // the user asked: scroll to top, reply jump, the load button
};

enum class HistoryFillStop {
    None,        // a fill is running, or none has run yet
    Filled,      // enough renderable rows
    StartOfRoom, // the server has nothing older
    PageCap,     // a page cap stopped it with more history still out there
    Failed,      // a request failed; what had arrived was kept
};

class HistoryFill {
public:
    // A new room visit: forget everything, including the automatic budget.
    void reset() { *this = HistoryFill{}; }

    bool active() const { return m_active; }
    HistoryFillKind kind() const { return m_kind; }
    int pagesThisFill() const { return m_pages; }
    int autoPagesThisOpen() const { return m_autoPages; }
    HistoryFillStop lastStop() const { return m_stop; }
    bool autoBudgetSpent() const { return m_autoPages >= kHistoryMaxAutoPagesPerOpen; }

    // May a fill of this kind start now? One at a time, and the client's own
    // fills only while the per-open budget lasts.
    bool canStart(HistoryFillKind kind) const
    {
        if (m_active) return false;
        if (kind == HistoryFillKind::Gesture) return true;
        return !autoBudgetSpent();
    }

    // Begin a fill. `rowsNow` is how many rows are already loaded: an OPEN
    // fill counts them towards its target (live /sync events can land before
    // the first page does), every other kind wants rows on top of them.
    // `firstPageLimit` is the caller's size for the first request.
    void start(HistoryFillKind kind, int rowsNow, int firstPageLimit)
    {
        m_active = true;
        m_kind = kind;
        m_pages = 0;
        m_stop = HistoryFillStop::None;
        m_rows = rowsNow;
        m_target = kind == HistoryFillKind::Open ? kHistoryOpenTargetRows
                                                 : rowsNow + kHistoryMoreTargetRows;
        m_firstLimit = firstPageLimit > 0 ? firstPageLimit : kHistoryFirstPageLimit;
    }

    // Pages this fill may fetch. An open is capped far below a gesture's
    // fill: it is on the critical path of a tap, and the two production
    // channels that motivated the number would otherwise spend ten requests
    // to display nothing (see kHistoryOpenMaxPages).
    int pageCap() const
    {
        return m_kind == HistoryFillKind::Open ? kHistoryOpenMaxPages
                                               : kHistoryMaxPagesPerFill;
    }

    // Size of the next request in this fill.
    int nextPageLimit() const
    {
        if (m_pages == 0) return m_firstLimit;
        return m_firstLimit > kHistoryFollowPageLimit ? m_firstLimit
                                                      : kHistoryFollowPageLimit;
    }

    // One page came back. `newRows` is how many NEW renderable rows it holds —
    // edits, reactions, redactions, state and duplicates already excluded by
    // the caller. `reachedStart` is "the server returned no token to go on
    // from". Returns true when another page should be requested.
    bool onPage(int newRows, bool reachedStart)
    {
        if (!m_active) return false;
        ++m_pages;
        if (m_kind != HistoryFillKind::Gesture) ++m_autoPages;
        m_rows += newRows > 0 ? newRows : 0;
        if (reachedStart) return finish(HistoryFillStop::StartOfRoom);
        if (m_rows >= m_target) return finish(HistoryFillStop::Filled);
        if (m_pages >= pageCap()) return finish(HistoryFillStop::PageCap);
        if (m_kind != HistoryFillKind::Gesture && autoBudgetSpent())
            return finish(HistoryFillStop::PageCap);
        return true;
    }

    // A request failed. The failed request still counts against the budget,
    // so a server answering 500 cannot be hammered by the viewport trigger.
    void fail()
    {
        if (!m_active) return;
        ++m_pages;
        if (m_kind != HistoryFillKind::Gesture) ++m_autoPages;
        finish(HistoryFillStop::Failed);
    }

private:
    bool finish(HistoryFillStop why)
    {
        m_active = false;
        m_stop = why;
        return false;
    }

    bool m_active = false;
    HistoryFillKind m_kind = HistoryFillKind::Open;
    int m_pages = 0;
    int m_autoPages = 0;
    int m_rows = 0;
    int m_target = 0;
    int m_firstLimit = kHistoryFirstPageLimit;
    HistoryFillStop m_stop = HistoryFillStop::None;
};

} // namespace bsfchat::client
