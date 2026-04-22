#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QPair>
#include <QString>
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
        EditedRole,         // Whether this message has been edited at least once
        ReplyToEventIdRole, // Event ID of the message being replied to (empty if not a reply)
        ReplyToSenderRole,  // Display name of the replied-to message's sender
        ReplyPreviewRole,   // Short excerpt (<=80 chars) of the replied-to message body
        ReactionsRole,      // Aggregated reactions: list of {emoji, count, reacted, eventIds}
        ThreadRootIdRole,   // If non-empty, this message is part of that thread
        ThreadReplyCountRole // Count of m.thread replies anchored on this message
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
        bool edited = false;    // True if ≥1 m.replace has been applied
        qint64 editedAt = 0;    // Timestamp of the latest edit
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

    MessageEntry eventToEntry(const bsfchat::RoomEvent& event, const QString& ownUserId) const;
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
