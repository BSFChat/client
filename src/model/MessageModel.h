#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <bsfchat/MatrixTypes.h>

#include "net/MediaTicketCache.h"
#include "util/HistoryFill.h"
#include "util/MentionRenderer.h"

#include <functional>
#include <optional>

class ThreadFilterModel;

class MessageModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    // True when the server has more history beyond the oldest loaded
    // message in this room. Bound by MessageView to show the "load more"
    // affordance / trigger auto-paginate on scroll-to-top.
    Q_PROPERTY(bool hasMoreHistory READ hasMoreHistory NOTIFY hasMoreHistoryChanged)
    // True while a /messages back-pagination request is in flight.
    // MessageView binds to this to show a spinner and suppress repeated
    // triggers from rapid scroll-to-top events.
    Q_PROPERTY(bool loadingHistory READ loadingHistory NOTIFY loadingHistoryChanged)
    // True once the client has spent its own per-visit page budget
    // (util/HistoryFill.h) and will not fetch more history unprompted.
    // MessageView uses it to offer a "Load older messages" button when the
    // list cannot scroll — the only way left to ask, since scrolling to the
    // top is impossible in a list that does not overflow.
    Q_PROPERTY(bool historyAutoFillSpent READ historyAutoFillSpent NOTIFY historyAutoFillSpentChanged)

public:
    enum Roles {
        EventIdRole = Qt::UserRole + 1,
        SenderRole,
        SenderDisplayNameRole,
        BodyRole,
        FormattedBodyRole,
        TimestampRole,
        MsgtypeRole,
        IsOwnMessageRole,
        ShowSenderRole,     // Whether to show sender info (for grouping)
        ShowDateSeparator,  // Whether to show a date separator above this message
        MediaUrlRole,       // Resolved HTTP URL for media messages (may arrive late)
        MediaMxcRole,       // The mxc:// URI the row's media came from
        MediaFileNameRole,  // Filename from media content
        MediaFileSizeRole,  // File size from media content info
        MediaWidthRole,     // Intrinsic width  in px from m.image/m.video info.w
        MediaHeightRole,    // Intrinsic height in px from m.image/m.video info.h
        EditedRole,         // Whether this message has been edited at least once
        ReplyToEventIdRole, // Event ID of the message being replied to (empty if not a reply)
        ReplyToSenderRole,  // Display name of the replied-to message's sender
        ReplyPreviewRole,   // Short excerpt (<=80 chars) of the replied-to message body
        ReactionsRole,      // Aggregated reactions: list of {emoji, count, reacted, eventIds}
        ThreadRootIdRole,   // If non-empty, this message is part of that thread
        ThreadReplyCountRole, // Count of m.thread replies anchored on this message
        MentionsMeRole,     // m.mentions.user_ids contains the local user
        MentionsRoomRole,   // m.mentions.room — an @room broadcast
        // True when this message's SENDER is a bot account, for the BOT
        // badge on the bubble.
        //
        // Stamped onto the row at append time from the bot-user cache and
        // re-resolved by refreshBotFlags(), exactly as senderDisplayName is.
        // A message event carries no `bsfchat.bot` of its own — the flag
        // rides on m.room.member — so the sender's membership is the source
        // and this model reads it through ServerConnection the same way it
        // reads display names.
        SenderIsBotRole
    };

    // A live, role-preserving view of one thread over this model (U-M8),
    // for the thread drawer to bind its ListView to. One proxy per model,
    // re-pointed as the user opens different threads; owned by this object
    // and valid for its lifetime, which is what makes it safe to hand to
    // QML. Returns nullptr for an empty root.
    //
    // Prefer this over threadReplies() anywhere the answer drives a view:
    // the snapshot list below cannot see an edit or a reaction, because
    // neither changes the row count that was the only thing re-running it.
    Q_INVOKABLE QAbstractItemModel* threadModel(const QString& rootEventId);

    // Thread helpers. `threadReplies` returns the messages whose
    // threadRootId == rootEventId, oldest-first, as {eventId, sender,
    // body, timestamp, msgtype}. `threadReplyCount` is a count-only
    // variant for badges.
    Q_INVOKABLE QVariantList threadReplies(const QString& rootEventId) const;
    Q_INVOKABLE int threadReplyCount(const QString& rootEventId) const;

    // Convenience for QML — the reactions for a given row. Same shape as
    // ReactionsRole.
    Q_INVOKABLE QVariantMap reactionSummary(int index) const;

    // If `userId` has reacted to `targetEventId` with `emoji`, return the
    // reaction event id (needed for redaction). Empty string otherwise.
    QString ownReactionEventId(const QString& targetEventId, const QString& emoji,
                                 const QString& userId) const;

    // Given a known event ID in this room, return its list index or -1.
    // Used by the UI to scroll to a replied-to message.
    Q_INVOKABLE int indexForEventId(const QString& eventId) const;

    // Preview map for a loaded event — {sender, body, timestamp}. Used
    // by the pinned-messages popover. Empty map if the event isn't
    // loaded. `body` is trimmed to ~160 chars for display.
    Q_INVOKABLE QVariantMap eventPreview(const QString& eventId) const;

    // Returns the full edit history for a given event: a list of
    // {body, timestamp} maps in chronological order, INCLUDING the
    // original (index 0) and the current body (last index). Empty
    // if the event isn't loaded or has no edits.
    Q_INVOKABLE QVariantList editHistory(const QString& eventId) const;

    // The mention set recorded for a loaded event, as it arrived in
    // `m.mentions`: the user ids, plus MatrixClient::kRoomMentionSentinel
    // appended when `m.mentions.room` was true — i.e. exactly the shape the
    // send/edit paths take. Empty if the event isn't loaded or named nobody.
    //
    // Exists so an EDIT can carry the original's mentions forward. The server
    // folds an edit's m.new_content in as the event's content, so an edit that
    // omitted m.mentions blanked the message's highlight for anyone loading the
    // room fresh. The server also refuses to narrow a mention set on edit, so
    // preserving-and-unioning here is the behaviour that matches it.
    QStringList mentionSetFor(const QString& eventId) const;

    // THE MENTION SET THAT ACTUALLY NOTIFIED SOMEBODY, for `event`.
    //
    // Returns the `m.mentions` object to trust, or an empty object when the
    // event names nobody. Read this instead of `event.content.data["m.mentions"]`
    // anywhere the answer drives a highlight, a badge or a toast.
    //
    // Why it is not simply the event's content: the server DELIBERATELY records
    // no mention rows and fires no push for an m.replace
    // (server/src/api/EventHandler.cpp, at the record_mentions call), because a
    // replacement lands at a brand-new stream position — past everybody's read
    // marker — so honouring its mentions would let anyone edit an old,
    // long-since-read message into a ping with no new message to explain it.
    // That is the right call, but it splits "what the message says" from "what
    // was notified": the server folds an edit's m.new_content in as the event's
    // content, so `content["m.mentions"]` on a reloaded message is the EDIT's
    // mention set. Rendering a highlight from it shows a pill to somebody who
    // was never told, which is worse than showing nothing — it looks like a
    // message they missed.
    //
    // The bundle the server sends makes the two distinguishable: an edited
    // event carries unsigned.m.relations.m.replace plus
    // unsigned.bsfchat.original_content, and original_content is the ORIGINAL
    // event's full content — mentions included (see read_event_row in
    // server/src/store/SqliteStore.cpp). So for an edited event the answer is
    // the original's block; for everything else it is the event's own.
    //
    // Mentions the edit REMOVED stay in the set, deliberately: the server does
    // not narrow a mention set on edit either (the mention row survives), so
    // dropping them here would show less than what was notified.
    static const nlohmann::json& notifiedMentions(const bsfchat::RoomEvent& event);

    // Unread-divider helpers. `firstEventIdAfterTs` returns the oldest
    // loaded event whose ts is strictly greater than `tsMs` (empty if
    // none). `newestTimestampMs` returns the newest loaded event's ts
    // or 0 if empty.
    Q_INVOKABLE QString firstEventIdAfterTs(qint64 tsMs) const;
    Q_INVOKABLE qint64 newestTimestampMs() const;

    // THE ANCHOR THE UNREAD DIVIDER IS ALLOWED TO USE.
    //
    // Same scan as firstEventIdAfterTs, but a message the LOCAL USER SENT is
    // never a candidate: you have read what you just typed, and a divider
    // above your own message is a claim you missed it.
    //
    // This is not cosmetic. The divider anchor doubles as the scroll-restore
    // target when a room is re-entered, so counting your own messages as
    // unread is how "send a screenful of messages, leave, come back" lands
    // you above the first message YOU sent instead of at the bottom. Own
    // messages are skipped rather than terminating the scan, so somebody
    // else's message that arrived after yours still anchors the divider.
    Q_INVOKABLE QString firstUnreadEventIdAfterTs(qint64 tsMs) const;

    // ── Scroll-position policy (see util/ScrollAnchor.h) ─────────────────
    //
    // Thin Q_INVOKABLE wrappers so MessageView.qml asks these questions
    // instead of answering them inline, where nothing could test the answer.
    // The model is the only C++ object the view holds a handle to, which is
    // why the two geometry-only helpers hang off it.

    // Row to restore to when this room is (re-)entered, or -1 for "scroll to
    // the newest message". `dividerEventId` may be empty, or an anchor left
    // over from another room — both resolve to -1.
    Q_INVOKABLE int restoreIndexForDivider(const QString& dividerEventId) const;

    // THE NEXT TWO ANSWER FROM THEIR ARGUMENTS ALONE — NEVER FROM ROWS.
    //
    // That is a load-bearing invariant, not an implementation detail. Both
    // are called from MessageView's contentY / contentHeight handlers, and
    // those handlers are re-entered from INSIDE QQuickItemView::setModel()
    // while the view's delegate model is null. The view therefore cannot
    // reach them through its own `model` property (that read segfaults —
    // see the comment on `messageModelRef` in MessageView.qml); it reaches
    // them through ServerConnection's `messageModel` instead, which during
    // a server switch may already be the INCOMING server's model.
    //
    // Handing a geometry question to the "wrong" model has to be harmless,
    // and it is only harmless while the answer cannot depend on which model
    // was asked. If either of these ever consults m_messages, that stops
    // being true. testScrollGeometryAnswersAreModelIndependent pins it.

    // Whether the viewport is inside the tolerance band at the end of the
    // content. Two-sided: a contentY parked past the end is NOT at the end.
    Q_INVOKABLE bool isPinnedToEnd(qreal contentHeight, qreal contentY,
                                   qreal viewportHeight, qreal tolerance) const;

    // What a change to this model means for scroll position.
    // 0 = Preserve (do not move the user), 1 = FollowEnd, 2 = Reenter
    // (run initial placement). Mirrors bsfchat::client::PositionPolicy.
    Q_INVOKABLE int scrollPolicy(bool contextChanged, bool paginating,
                                 bool pinnedToEnd, bool followLatch) const;

    // Whether a contentY sample may revoke follow-the-end intent. Only a
    // user-driven scroll may; layout-driven ticks may not. See the long note
    // on bsfchat::client::followEndAfterContentYSample — this is the rule
    // that stops content growing after placement from silently unpinning the
    // view, which is what made three separate scroll reports look unfixable.
    Q_INVOKABLE bool followEndAfterContentYSample(bool followEnd, bool userDriven,
                                                  bool liveAtEnd) const;

    // What a viewport resize (window, fullscreen, panel toggle) means. Same
    // encoding as scrollPolicy.
    Q_INVOKABLE int geometryChangePolicy(bool followEnd) const;

    // Case-insensitive substring search over loaded message bodies +
    // sender display names. Returns up to `limit` matches, newest
    // first, each a map with {eventId, sender, body, timestamp}.
    Q_INVOKABLE QVariantList searchMessages(const QString& query, int limit = 50) const;

    explicit MessageModel(QObject* parent = nullptr);

    void setHomeserver(const QString& homeserver) { m_homeserver = homeserver; }
    QString homeserver() const { return m_homeserver; }

    // Access token used to authenticate media downloads — pointer to
    // ServerConnection's copy, not owned. It's a pointer rather than a
    // value because the token is assigned on four separate paths (restored
    // credentials, password login, registration, OIDC) that all run after
    // this model is constructed; reading through the owner means none of
    // them can forget to push an update here.
    void setAccessTokenSource(const QString* token) { m_accessToken = token; }

    // The shared ticket cache (MatrixClient's), not owned.
    //
    // Media URLs used to be built here from the homeserver and the token. They
    // are now built from a short-lived server-signed ticket, which has to be
    // fetched — so resolveMediaUrl() can answer "" for a row that will get a URL
    // a moment later, and this model listens for that and repaints the row.
    // Without a cache set, resolveMediaUrl() returns "" and no media resolves:
    // that is deliberate, because the only other thing it could do is put the
    // session token back in the URL.
    void setMediaTicketCache(bsfchat::client::MediaTicketCache* cache);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Display-name cache — pointer to ServerConnection's global map so we
    // can resolve @user:host → "Josh" at render time. Not owned.
    void setDisplayNameCache(const QMap<QString, QString>* cache) { m_dnCache = cache; }

    // The set of user ids known to be bots — ServerConnection's, not owned,
    // populated from m.room.member content across every room. Same
    // arrangement and same reasoning as the display-name cache above: a
    // member event for a room this model is not showing still tells us
    // something about a sender whose messages it is.
    void setBotUserCache(const QSet<QString>* cache) { m_botUsers = cache; }

    // Resolves the role ids an event named into the role mentions that ACTUALLY
    // NOTIFIED somebody, for rendering and for the highlight.
    //
    // A callback rather than a pointer to a cache, because the answer is not a
    // lookup: it is the server's own rule — the role exists, and either it is
    // `mentionable` or the SENDER holds MENTION_EVERYONE — plus "and do I hold
    // it". That needs the role table, the sender's role assignment and the
    // channel's permission overrides, all of which live in ServerConnection.
    // Reproducing it here would be a second implementation of an authorisation
    // rule, which is exactly how a client ends up drawing a pill for a ping
    // that never happened (or, worse, suppressing one that did).
    //
    // Roles the rule rejects are simply absent from the result, which is how a
    // non-mentionable role ends up rendered as plain text.
    using RoleMentionResolver =
        std::function<QVector<bsfchat::client::RoleMentionTarget>(
            const QStringList& roleIds, const QString& sender, const QString& roomId)>;
    void setRoleMentionResolver(RoleMentionResolver resolver) {
        m_roleResolver = std::move(resolver);
    }
    // Re-resolve every loaded row's flag from that cache, emitting only the
    // rows that actually moved — the same coalescing refreshDisplayNames
    // does, and for the same reason: this runs whenever a member event
    // arrives, which in a busy room is often.
    void refreshBotFlags();

    void appendEvent(const bsfchat::RoomEvent& event, const QString& ownUserId);
    void appendEvents(const QVector<bsfchat::RoomEvent>& events, const QString& ownUserId);
    void prependEvents(const QVector<bsfchat::RoomEvent>& events, const QString& ownUserId);
    // One /messages page (or a whole buffered fill) into the timeline: the
    // rows through append or prepend as the model's state demands, and then
    // the reactions, redactions and edits the same page carried. Public
    // because it is also how a cached window is replayed on a room switch.
    void ingestHistoryEvents(const QVector<bsfchat::RoomEvent>& chronological,
                             const QString& ownUserId);
    // A whole cached window into an empty (or newer-only) model, as ONE
    // insertion rather than one per event. Used by the room switch, where the
    // per-event path's cost is paid at the moment the user is waiting.
    void ingestCachedWindow(const QVector<bsfchat::RoomEvent>& chronological,
                            const QString& ownUserId);
    void clear();

    // Take a message out of the timeline (U-H5). Returns true if a row was
    // actually removed.
    //
    // Two callers: the m.room.redaction branch of appendEvent, which is how
    // somebody *else's* deletion reaches us, and ServerConnection::redactEvent,
    // which removes optimistically so the author doesn't watch their own
    // deleted message sit there until the next sync. Both go through here so
    // the second one is idempotent against the first.
    //
    // Does the whole job, not just the rows: the reaction index entries that
    // pointed into this message are dropped (otherwise a later reaction
    // redaction resolves a row that has since moved), the thread reply count
    // on its root is decremented, and the row that inherits this slot is
    // repainted because its sender header / date separator is computed from
    // its predecessor.
    bool removeMessage(const QString& eventId);

    // Back-pagination state. ServerConnection writes these as sync+messages
    // responses come in; MessageView reads them to drive the scroll-to-top
    // trigger and the reply-jump paginate-until-found loop.
    QString prevBatchToken() const { return m_prevBatchToken; }
    void setPrevBatchToken(const QString& token);
    bool hasMoreHistory() const { return !m_prevBatchToken.isEmpty(); }
    bool loadingHistory() const { return m_loadingHistory; }
    void setLoadingHistory(bool v);

    // ── History fills (util/HistoryFill.h) ────────────────────────────────
    //
    // The paging LOOP lives here rather than in ServerConnection so that it
    // can be driven with synthetic pages in tests/test_models.cpp; the
    // connection only performs the requests this hands it.
    //
    // A fill's pages are BUFFERED and put into the model in one go when the
    // fill ends. Two reasons. MessageView anchors the scroll position on
    // "the page I asked for landed" (its _paginationRequested/_Landed
    // pair): one request, one prepend, one olderMessagesLoaded. A fill that
    // prepended page by page would land rows the view never asked for, and
    // a reader scrolled up would be shoved by each of them. And counting
    // what a page WOULD add is the same question as dedupe — events already
    // loaded, or already in an earlier page of this fill, are not new rows.
    struct HistoryRequest {
        QString from;   // "" for the newest page (a room open)
        int limit = 0;
    };
    enum class HistoryPageOutcome {
        Ignored,   // not a page of the running fill (stale, or no fill)
        FetchMore, // request `next`
        Done,      // the fill is over and its rows are in the model
    };
    struct HistoryPageResult {
        HistoryPageOutcome outcome = HistoryPageOutcome::Ignored;
        HistoryRequest next;
    };

    // Start a fill, or refuse with nullopt: one already running, no token to
    // continue from (start of room — the guard that stops a view at the top
    // of a short room from re-asking forever), or an automatic fill with the
    // per-visit budget spent. An Open fill always starts from the newest
    // page. Sets loadingHistory for the whole fill, not per request.
    std::optional<HistoryRequest> beginHistoryFill(bsfchat::client::HistoryFillKind kind,
                                                   int firstPageLimit);
    // Absorb one /messages page. `requestedFrom` is the `from` the page was
    // requested with ("" for the newest page) and is how a page is matched to
    // the running fill; `chronological` is oldest-first; `endToken` is the
    // server's token for going further back, empty at the start of the room.
    HistoryPageResult absorbHistoryPage(const QString& requestedFrom,
                                        const QVector<bsfchat::RoomEvent>& chronological,
                                        const QString& endToken,
                                        const QString& ownUserId);
    // The request for `requestedFrom` failed. Keeps whatever the fill had
    // already gathered, ends it, and returns true if a fill was ended (the
    // caller then reports completion exactly as for Done).
    bool failHistoryFill(const QString& requestedFrom, const QString& ownUserId);
    bool historyAutoFillSpent() const { return m_historyFill.autoBudgetSpent(); }
    const bsfchat::client::HistoryFill& historyFill() const { return m_historyFill; }

    // Whether `event` would become a row of this timeline, i.e. whether it is
    // something the user can SEE. An m.room.message that is not an edit
    // sibling. Edits fold into their target, reactions fold into a row's
    // chips, redactions remove rows, and state events are not drawn — none of
    // them may count towards "enough history is loaded". Deliberately the
    // same test prependEvents and appendEvent use to decide what becomes a
    // row, so the count and the rows cannot disagree.
    static bool rendersAsRow(const bsfchat::RoomEvent& event);

    // Edits that arrived for a message that is not loaded, newest per target.
    // Exposed for tests.
    int pendingEditCount() const { return static_cast<int>(m_pendingEdits.size()); }

    // Re-resolve every sender display name from the cache and emit
    // dataChanged so the UI updates when a user changes their profile.
    void refreshDisplayNames();

signals:
    void countChanged();
    void hasMoreHistoryChanged();
    void loadingHistoryChanged();
    void historyAutoFillSpentChanged();

private:
    struct MessageEntry {
        QString eventId;
        QString sender;
        QString senderDisplayName;
        bool senderIsBot = false;
        QString body;
        QString formattedBody;
        qint64 timestamp = 0;
        QString msgtype;
        bool isOwnMessage = false;
        QString mediaUrl;       // Resolved HTTP URL for m.image/m.file ("" until ticketed)
        QString mediaMxc;       // The mxc:// URI mediaUrl was resolved from
        QString mediaFileName;  // Filename from content
        qint64 mediaFileSize = 0; // Size in bytes
        int    mediaWidth  = 0;   // intrinsic px from info.w (0 = unknown)
        int    mediaHeight = 0;   // intrinsic px from info.h (0 = unknown)
        bool edited = false;    // True if ≥1 m.replace has been applied
        qint64 editedAt = 0;    // Timestamp of the latest edit
        // Previous bodies (oldest → newest before the current one).
        // Each pair is {body, editAt}. Seeded from the server's
        // unsigned.bsfchat.original_content when a timeline event arrives
        // already-reconciled, and extended in place by live edits.
        QVector<QPair<QString, qint64>> history;
        // Event ids of the m.replace events whose result is ALREADY in
        // `body`. Two sources feed it: the server's bundled
        // unsigned.m.relations.m.replace (the edit that produced the body we
        // were handed), and every live edit we apply ourselves.
        //
        // This is what stops the double-render: the replacement is *also* an
        // ordinary timeline event, so the same edit reaches us twice — once
        // folded into the original's content, once as its own sibling. It
        // also makes a replayed sibling idempotent, which the old code was
        // not (it appended to `history` every time).
        QSet<QString> appliedEdits;
        // m.mentions (MSC3952) as sent by the author. `mentionsMe` is
        // resolved against the local user at ingest time; `mentionedUserIds`
        // is kept so the renderer can locate each mention's token in the
        // body without re-parsing the event.
        bool mentionsMe = false;
        bool mentionsRoom = false;
        QStringList mentionedUserIds;
        // Role mentions that took effect, resolved once at ingest through
        // m_roleResolver. Only roles that actually notified are in here; a
        // non-mentionable one is absent and so renders as plain text.
        // `mentionsMe` is OR'd with "I hold one of these", because a role
        // mention is a mention of every holder — the badge, the push and the
        // highlight all have to agree on that or the three disagree about the
        // same message.
        QVector<bsfchat::client::RoleMentionTarget> roleMentions;
        // Reply metadata — populated when content.m.relates_to.m.in_reply_to
        // is present. replyToSender/replyPreview are best-effort snapshots
        // resolved from the local timeline when this message was ingested;
        // if the target arrives later, a future pass can backfill them.
        QString replyToEventId;
        QString replyToSender;
        QString replyPreview;
        // Threading — if this message is part of an m.thread relation,
        // `threadRootId` points to the thread's top-level event. The
        // root message's own `threadRootId` stays empty; we compute
        // its reply count by scanning children.
        QString threadRootId;
        // Reactions aggregated from m.reaction events targeting this entry.
        // Keyed by emoji; value is the list of (userId, reactionEventId) pairs
        // so we can (a) count unique reactors, (b) detect whether the current
        // user has reacted, and (c) find the reaction event id to redact when
        // toggling off.
        QHash<QString, QVector<QPair<QString, QString>>> reactionsByEmoji;
    };

    // Reaction events that arrived before their target message. Keyed by
    // target event id, flushed on the next appendEvent that lands the target.
    struct PendingReaction {
        QString emoji;
        QString userId;
        QString reactionEventId;
    };

    // THE NEWEST EDIT for a message that is not loaded yet, keyed by the
    // target's event id.
    //
    // These used to be dropped on the floor ("a future sync/backfill will
    // bring the original, and we'll see this edit again") — and nothing
    // guaranteed that. /messages returns the original reconciled with the
    // winning edit AS OF THE FETCH, so an edit that lands over /sync between
    // the server answering a back-page and the client absorbing it was lost:
    // the original showed its older text until the room was re-opened. The
    // board in #notifications (edited every ~2 minutes, its original far
    // outside the loaded window) makes that window routine rather than rare.
    //
    // Only the newest per target is kept — the body it carries is the whole
    // answer, older ones would lose to it anyway — which bounds this by the
    // number of distinct unloaded targets, and kMaxPendingEditTargets bounds
    // that. Cleared with the model on a room switch.
    struct PendingEdit {
        QString editEventId;
        qint64 timestamp = 0;
        QString body;
        QString formattedBody; // raw; rendered against the target's msgtype
    };
    static constexpr int kMaxPendingEditTargets = 256;
    QHash<QString, PendingEdit> m_pendingEdits;

    QVector<MessageEntry> m_messages;
    // event id -> row index. Every "find the message this event refers to"
    // path — edits, reactions, redactions, reply-preview resolution, append
    // dedupe — used to be its own linear scan over m_messages, so a sync
    // batch of N reactions against a room with M loaded rows cost N*M
    // comparisons. Kept in step with m_messages by append/prepend/clear
    // (nothing else mutates the row set).
    QHash<QString, int> m_indexByEventId;
    // thread root event id -> count of loaded m.thread replies. data() reads
    // this for ThreadReplyCountRole on *every* row, which made a full-model
    // repaint quadratic while it was a scan.
    QHash<QString, int> m_threadReplyCounts;
    QHash<QString, QVector<PendingReaction>> m_pendingReactions;
    // Map reaction event id -> (target event id, emoji, userId) so when a
    // redaction arrives we can find which message's aggregate to update.
    struct ReactionRef { QString targetEventId; QString emoji; QString userId; };
    QHash<QString, ReactionRef> m_reactionIndex;
    QString m_ownUserId; // cached from the most recent append, used by redactions
    QString m_homeserver;
    // Opaque token pointing at events older than the current oldest row.
    // Empty => no more history (or never populated). The exact format is
    // server-defined; we pass it back verbatim as the `from` param on
    // /rooms/{id}/messages.
    ThreadFilterModel* m_threadProxy = nullptr;
    QString m_prevBatchToken;
    bool m_loadingHistory = false;
    // The running fill, and the pages it has gathered so far (each
    // oldest-first, in arrival order — so NEWEST page first) plus the ids of
    // the rows they will add, for dedupe across pages.
    bsfchat::client::HistoryFill m_historyFill;
    QString m_historyFillFrom;
    QVector<QVector<bsfchat::RoomEvent>> m_historyFillPages;
    QSet<QString> m_historyFillRowIds;
    void finishHistoryFill(const QString& ownUserId, bool wasSpent);
    RoleMentionResolver m_roleResolver;
    const QMap<QString, QString>* m_dnCache = nullptr;
    const QSet<QString>* m_botUsers = nullptr;
    const QString* m_accessToken = nullptr;
    bsfchat::client::MediaTicketCache* m_mediaTickets = nullptr;

    // A ticket landed for `mxcUri`: re-resolve every row that names it and
    // repaint just those. One mxc can legitimately appear in several rows (a
    // repost, a thumbnail reused as the full image), so this is a scan rather
    // than a single lookup — bounded by the loaded timeline, and it only runs
    // once per object per five-minute ticket.
    void onMediaTicketReady(const QString& mxcUri, const QString& url);

    MessageEntry eventToEntry(const bsfchat::RoomEvent& event, const QString& ownUserId) const;
    // True when `event` is an edit *sibling* — content.m.relates_to with
    // rel_type "m.replace". Such an event must never become a row of its own:
    // its body is the "* new text" fallback for clients that can't apply
    // edits. appendEvent folds it into its target; prependEvents drops it.
    static bool isReplacementEvent(const bsfchat::RoomEvent& event);
    // An edit's payload, parsed once so a live edit and a stashed one are
    // applied by the same code.
    static PendingEdit editPayload(const bsfchat::RoomEvent& event);
    // Apply an edit to an entry. Returns true when the displayed body changed.
    // An edit OLDER than the one the entry already shows (by server
    // timestamp) never overwrites it — see the note in the .cpp.
    bool applyEditToEntry(MessageEntry& entry, const PendingEdit& edit) const;
    void stashPendingEdit(const QString& targetId, const PendingEdit& edit);
    // A row is about to be inserted: fold in any edit that beat it here.
    void drainPendingEdit(MessageEntry& entry);
    // Rewrite `entry.formattedBody` so the mentions recorded on the entry are
    // rendered as highlighted anchors. Reads m_dnCache for display names and
    // m_ownUserId to pick the self-mention style.
    void applyMentionMarkup(MessageEntry& entry) const;
    // Row for an event id, or -1. The single accessor for m_indexByEventId so
    // callers can't accidentally reintroduce a scan.
    int rowForEventId(const QString& eventId) const;
    // Rebuild m_indexByEventId / m_threadReplyCounts from m_messages. Needed
    // after a prepend, where every existing row shifts; O(n) against an
    // operation that already inserted n rows.
    void rebuildIndices();
public:
    // Resolves an mxc:// URI to an authenticated HTTP URL against the
    // homeserver. Exposed (was private) because ServerConnection needs
    // it for resolving server-icon avatars in m.room.pinned_events and
    // bsfchat.server.info state.
    QString resolveMediaUrl(const QString& mxcUri) const;
private:
    QString resolveDisplayName(const QString& userId) const;
    bool resolveIsBot(const QString& userId) const;
    QVariantList buildReactionsList(const MessageEntry& entry) const;
    // Apply a single reaction record to the target message. Returns the row
    // index so the caller can emit dataChanged, or -1 if the target wasn't
    // found (caller should stash as pending).
    int applyReactionToTarget(const QString& targetEventId, const QString& emoji,
                               const QString& userId, const QString& reactionEventId);
};
