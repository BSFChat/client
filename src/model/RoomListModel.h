#pragma once

#include <QAbstractListModel>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVector>

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
        ParentIdRole,
        RoomTypeRole,
        SortOrderRole
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
    // Absolute set — used when the server provides authoritative unread count.
    void setUnreadCount(const QString& roomId, int count);
    int totalUnreadCount() const;
    Q_INVOKABLE QString roomDisplayName(const QString& roomId) const;
    // Reverse lookup: find a text channel by exact case-insensitive name.
    // Used by the #channel-mention click handler. Returns "" if no match.
    Q_INVOKABLE QString roomIdForName(const QString& name) const;
    Q_INVOKABLE QString roomTopic(const QString& roomId) const;
    void updateVoiceState(const QString& roomId, bool isVoice);
    void updateVoiceMemberCount(const QString& roomId, int count);
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
        QString lastMessage;
        qint64 lastMessageTime = 0;
        bool isVoice = false;
        int voiceMemberCount = 0;
        QString parentId;
        QString roomType;
        int sortOrder = 0;
    };

    QVector<RoomEntry> m_rooms;
};
