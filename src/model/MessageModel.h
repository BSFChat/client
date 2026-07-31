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
        MediaUrlRole,       // Resolved HTTP URL for media messages
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
        MentionsRoomRole    // m.mentions.room — an @room broadcast
    };

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

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Display-name cache — pointer to ServerConnection's global map so we
    // can resolve @user:host → "Josh" at render time. Not owned.
    void setDisplayNameCache(const QMap<QString, QString>* cache) { m_dnCache = cache; }

    void appendEvent(const bsfchat::RoomEvent& event, const QString& ownUserId);
    void appendEvents(const QVector<bsfchat::RoomEvent>& events, const QString& ownUserId);
    void prependEvents(const QVector<bsfchat::RoomEvent>& events, const QString& ownUserId);
    void clear();

    // Back-pagination state. ServerConnection writes these as sync+messages
    // responses come in; MessageView reads them to drive the scroll-to-top
    // trigger and the reply-jump paginate-until-found loop.
    QString prevBatchToken() const { return m_prevBatchToken; }
    void setPrevBatchToken(const QString& token);
    bool hasMoreHistory() const { return !m_prevBatchToken.isEmpty(); }
    bool loadingHistory() const { return m_loadingHistory; }
    void setLoadingHistory(bool v);

    // Re-resolve every sender display name from the cache and emit
    // dataChanged so the UI updates when a user changes their profile.
    void refreshDisplayNames();

signals:
    void countChanged();
    void hasMoreHistoryChanged();
    void loadingHistoryChanged();

private:
    struct MessageEntry {
        QString eventId;
        QString sender;
        QString senderDisplayName;
        QString body;
        QString formattedBody;
        qint64 timestamp = 0;
        QString msgtype;
        bool isOwnMessage = false;
        QString mediaUrl;       // Resolved HTTP URL for m.image/m.file
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
    QString m_prevBatchToken;
    bool m_loadingHistory = false;
    const QMap<QString, QString>* m_dnCache = nullptr;
    const QString* m_accessToken = nullptr;

    MessageEntry eventToEntry(const bsfchat::RoomEvent& event, const QString& ownUserId) const;
    // True when `event` is an edit *sibling* — content.m.relates_to with
    // rel_type "m.replace". Such an event must never become a row of its own:
    // its body is the "* new text" fallback for clients that can't apply
    // edits. appendEvent folds it into its target; prependEvents drops it.
    static bool isReplacementEvent(const bsfchat::RoomEvent& event);
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
    QVariantList buildReactionsList(const MessageEntry& entry) const;
    // Apply a single reaction record to the target message. Returns the row
    // index so the caller can emit dataChanged, or -1 if the target wasn't
    // found (caller should stash as pending).
    int applyReactionToTarget(const QString& targetEventId, const QString& emoji,
                               const QString& userId, const QString& reactionEventId);
};
