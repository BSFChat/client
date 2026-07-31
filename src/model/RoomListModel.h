#pragma once

#include <QAbstractListModel>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVector>

#include "util/ChannelRestore.h"
#include "util/VoiceRoster.h"

class RoomListModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles {
        RoomIdRole = Qt::UserRole + 1,
        DisplayNameRole,
        TopicRole,
        AvatarUrlRole,
        UnreadCountRole,
        LastMessageRole,
        LastMessageTimeRole,
        IsVoiceRole,
        VoiceMemberCountRole,
        VoiceMembersRole,
        ParentIdRole,
        RoomTypeRole,
        SortOrderRole,
        MentionCountRole
    };

    explicit RoomListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void ensureRoom(const QString& roomId);
    void updateRoomName(const QString& roomId, const QString& name);
    void updateRoomTopic(const QString& roomId, const QString& topic);
    void updateLastMessage(const QString& roomId, const QString& message, qint64 timestamp);
    void incrementUnreadCount(const QString& roomId, int count);
    void resetUnreadCount(const QString& roomId);

    // Mentions are tracked separately from unread because they survive
    // muting: a muted channel hides its unread dot, but a message that names
    // you still has to be visible in the channel list. Cleared by
    // resetUnreadCount (i.e. whenever the room is opened or its read marker
    // is advanced), never by setUnreadCount — a server-reported unread total
    // says nothing about whether the mention has been seen.
    //
    // The increment form is the LEGACY path, kept only for servers that don't
    // report unread_notifications.highlight_count: it can only ever count
    // mentions the client actually witnessed arriving, so a session that
    // resumed from a persisted sync token under-reports every mention older
    // than its resume token. Prefer setMentionCount().
    void incrementMentionCount(const QString& roomId, int count);
    // Absolute set — the server's unread_notifications.highlight_count, which
    // is computed against the server-side read marker and therefore counts
    // mentions this client never saw arrive. Idempotent by construction, so
    // unlike the increment form it is safe to apply on every sync (including a
    // cache-hydration replay) without double-counting.
    void setMentionCount(const QString& roomId, int count);
    Q_INVOKABLE int mentionCountFor(const QString& roomId) const;
    int totalMentionCount() const;
    // Absolute set — used when the server provides authoritative unread count.
    void setUnreadCount(const QString& roomId, int count);
    int totalUnreadCount() const;
    Q_INVOKABLE QString roomDisplayName(const QString& roomId) const;
    // Reverse lookup: find a text channel by exact case-insensitive name.
    // Used by the #channel-mention click handler. Returns "" if no match.
    Q_INVOKABLE QString roomIdForName(const QString& name) const;
    Q_INVOKABLE QString roomTopic(const QString& roomId) const;
    // Unread count for a specific room. Authoritative source —
    // updated from server-side state, not derived from any local
    // last-read timestamp. Returns 0 if the room is unknown.
    Q_INVOKABLE int unreadCountFor(const QString& roomId) const;

    // First text channel (non-voice) in display order. Used by the
    // mobile shell to auto-select a landing channel on login so the
    // user doesn't get dropped into an empty-state screen they have
    // to actively dismiss. Returns "" if we have no text channels.
    Q_INVOKABLE QString firstTextRoomId() const;

    // Classifier used by the mobile shell's "remember last text
    // channel" logic — voice rooms deliberately don't get auto-
    // restored on launch (would transmit mic without consent).
    Q_INVOKABLE bool isVoiceRoom(const QString& roomId) const;
    Q_INVOKABLE bool hasRoom(const QString& roomId) const;

    // Which channel to open when this server comes to the foreground. Returns
    // "" for both "wait, sync hasn't delivered the list" and "there is nothing
    // to open" — callers that need to tell those apart use
    // bsfchat::client::chooseChannelToRestore directly. `syncComplete` says
    // whether the room list is the whole world yet; see ChannelRestore.h.
    Q_INVOKABLE QString restoreTargetRoomId(const QString& remembered,
                                            bool syncComplete) const;
    bsfchat::client::ChannelRestoreChoice restoreChoice(const QString& remembered,
                                                 bool syncComplete) const;

    void updateVoiceState(const QString& roomId, bool isVoice);

    // Voice occupancy, folded one m.call.member state event at a time —
    // `active` decides insert/update vs. remove. The roster IS the count:
    // there is no separately stored number that could disagree with the list,
    // and that disagreement is exactly how a user who left kept their seat in
    // the sidebar. Returns true iff the roster changed, so callers can skip
    // republishing the sidebar when a re-delivered event says nothing new.
    bool applyCallMember(const QString& roomId,
                         const bsfchat::client::VoiceParticipant& participant,
                         bool active);
    const bsfchat::client::VoiceRoster& voiceMembers(const QString& roomId) const;
    Q_INVOKABLE int voiceMemberCount(const QString& roomId) const;

    void removeRoom(const QString& roomId);
    // Remove any room whose ID is not in the given set. Used to drop rooms
    // the server has made invisible to us (e.g. after permission change).
    // Returns the list of removed IDs.
    QStringList pruneRoomsNotIn(const QSet<QString>& keep);
    void clear();

    // Category support
    void updateParentId(const QString& roomId, const QString& parentId);
    void updateRoomType(const QString& roomId, const QString& roomType);
    void updateSortOrder(const QString& roomId, int order);
    Q_INVOKABLE QVariantList getCategoriesWithChannels() const;

private:
    int findRoom(const QString& roomId) const;

    struct RoomEntry {
        QString roomId;
        QString displayName;
        QString topic;
        QString avatarUrl;
        int unreadCount = 0;
        int mentionCount = 0;
        QString lastMessage;
        qint64 lastMessageTime = 0;
        bool isVoice = false;
        // Exactly the users whose newest m.call.member said active. The
        // sidebar's participant count is voiceMembers.size(), never a
        // separately tracked integer.
        bsfchat::client::VoiceRoster voiceMembers;
        QString parentId;
        QString roomType;
        int sortOrder = 0;
    };

    QVector<RoomEntry> m_rooms;
};
