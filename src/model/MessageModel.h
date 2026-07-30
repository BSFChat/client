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

    // Unread-divider helpers. `firstEventIdAfterTs` returns the oldest
    // loaded event whose ts is strictly greater than `tsMs` (empty if
    // none). `newestTimestampMs` returns the newest loaded event's ts
    // or 0 if empty.
    Q_INVOKABLE QString firstEventIdAfterTs(qint64 tsMs) const;
    Q_INVOKABLE qint64 newestTimestampMs() const;

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
