#include "model/RoomListModel.h"

#include <QVariantMap>
#include <algorithm>

RoomListModel::RoomListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int RoomListModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) return 0;
    return m_rooms.size();
}

QVariant RoomListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rooms.size())
        return {};

    const auto& room = m_rooms[index.row()];
    switch (role) {
    case RoomIdRole: return room.roomId;
    case DisplayNameRole: return room.displayName.isEmpty() ? room.roomId : room.displayName;
    case TopicRole: return room.topic;
    case AvatarUrlRole: return room.avatarUrl;
    case UnreadCountRole: return room.unreadCount;
    case LastMessageRole: return room.lastMessage;
    case LastMessageTimeRole: return room.lastMessageTime;
    case IsVoiceRole: return room.isVoice;
    case VoiceMemberCountRole: return room.voiceMemberCount;
    case ParentIdRole: return room.parentId;
    case RoomTypeRole: return room.roomType;
    case SortOrderRole: return room.sortOrder;
    default: return {};
    }
}

QHash<int, QByteArray> RoomListModel::roleNames() const
{
    return {
        {RoomIdRole, "roomId"},
        {DisplayNameRole, "displayName"},
        {TopicRole, "topic"},
        {AvatarUrlRole, "avatarUrl"},
        {UnreadCountRole, "unreadCount"},
        {LastMessageRole, "lastMessage"},
        {LastMessageTimeRole, "lastMessageTime"},
        {IsVoiceRole, "isVoice"},
        {VoiceMemberCountRole, "voiceMemberCount"},
        {ParentIdRole, "parentId"},
        {RoomTypeRole, "roomType"},
        {SortOrderRole, "sortOrder"}
    };
}

int RoomListModel::findRoom(const QString& roomId) const
{
    for (int i = 0; i < m_rooms.size(); ++i) {
        if (m_rooms[i].roomId == roomId) return i;
    }
    return -1;
}

void RoomListModel::ensureRoom(const QString& roomId)
{
    if (findRoom(roomId) >= 0) return;
    beginInsertRows(QModelIndex(), m_rooms.size(), m_rooms.size());
    m_rooms.append({roomId, {}, {}, {}, 0, {}, 0, false, 0, {}, {}, 0});
    endInsertRows();
}

void RoomListModel::updateRoomName(const QString& roomId, const QString& name)
{
    ensureRoom(roomId);
    int idx = findRoom(roomId);
    if (idx < 0) return;
    m_rooms[idx].displayName = name;
    emit dataChanged(index(idx), index(idx), {DisplayNameRole});
}

void RoomListModel::updateRoomTopic(const QString& roomId, const QString& topic)
{
    ensureRoom(roomId);
    int idx = findRoom(roomId);
    if (idx < 0) return;
    m_rooms[idx].topic = topic;
    emit dataChanged(index(idx), index(idx), {TopicRole});
}

void RoomListModel::updateLastMessage(const QString& roomId, const QString& message, qint64 timestamp)
{
    ensureRoom(roomId);
    int idx = findRoom(roomId);
    if (idx < 0) return;
    m_rooms[idx].lastMessage = message;
    m_rooms[idx].lastMessageTime = timestamp;
    emit dataChanged(index(idx), index(idx), {LastMessageRole, LastMessageTimeRole});
}

void RoomListModel::incrementUnreadCount(const QString& roomId, int count)
{
    int idx = findRoom(roomId);
    if (idx < 0) return;
    m_rooms[idx].unreadCount += count;
    emit dataChanged(index(idx), index(idx), {UnreadCountRole});
}

void RoomListModel::resetUnreadCount(const QString& roomId)
{
    setUnreadCount(roomId, 0);
}

void RoomListModel::setUnreadCount(const QString& roomId, int count)
{
    int idx = findRoom(roomId);
    if (idx < 0) return;
    if (m_rooms[idx].unreadCount == count) return;
    m_rooms[idx].unreadCount = count;
    emit dataChanged(index(idx), index(idx), {UnreadCountRole});
}

int RoomListModel::totalUnreadCount() const
{
    int total = 0;
    for (const auto& room : m_rooms) {
        total += room.unreadCount;
    }
    return total;
}

QString RoomListModel::roomDisplayName(const QString& roomId) const
{
    int idx = findRoom(roomId);
    if (idx < 0) return roomId;
    return m_rooms[idx].displayName.isEmpty() ? m_rooms[idx].roomId : m_rooms[idx].displayName;
}

QString RoomListModel::roomTopic(const QString& roomId) const
{
    int idx = findRoom(roomId);
    if (idx < 0) return {};
    return m_rooms[idx].topic;
}

void RoomListModel::updateVoiceState(const QString& roomId, bool isVoice)
{
    ensureRoom(roomId);
    int idx = findRoom(roomId);
    if (idx < 0) return;
    if (m_rooms[idx].isVoice == isVoice) return;
    m_rooms[idx].isVoice = isVoice;
    emit dataChanged(index(idx), index(idx), {IsVoiceRole});
}

void RoomListModel::updateVoiceMemberCount(const QString& roomId, int count)
{
    int idx = findRoom(roomId);
    if (idx < 0) return;
    if (m_rooms[idx].voiceMemberCount == count) return;
    m_rooms[idx].voiceMemberCount = count;
    emit dataChanged(index(idx), index(idx), {VoiceMemberCountRole});
}

void RoomListModel::removeRoom(const QString& roomId)
{
    int idx = findRoom(roomId);
    if (idx < 0) return;
    beginRemoveRows(QModelIndex(), idx, idx);
    m_rooms.removeAt(idx);
    endRemoveRows();
}

void RoomListModel::clear()
{
    beginResetModel();
    m_rooms.clear();
    endResetModel();
}

void RoomListModel::updateParentId(const QString& roomId, const QString& parentId)
{
    ensureRoom(roomId);
    int idx = findRoom(roomId);
    if (idx < 0) return;
    if (m_rooms[idx].parentId == parentId) return;
    m_rooms[idx].parentId = parentId;
    emit dataChanged(index(idx), index(idx), {ParentIdRole});
}

void RoomListModel::updateRoomType(const QString& roomId, const QString& roomType)
{
    ensureRoom(roomId);
    int idx = findRoom(roomId);
    if (idx < 0) return;
    if (m_rooms[idx].roomType == roomType) return;
    m_rooms[idx].roomType = roomType;
    // If marked as voice via room type, update voice state too
    if (roomType == QStringLiteral("voice")) {
        m_rooms[idx].isVoice = true;
    }
    emit dataChanged(index(idx), index(idx), {RoomTypeRole, IsVoiceRole});
}

void RoomListModel::updateSortOrder(const QString& roomId, int order)
{
    int idx = findRoom(roomId);
    if (idx < 0) return;
    if (m_rooms[idx].sortOrder == order) return;
    m_rooms[idx].sortOrder = order;
    emit dataChanged(index(idx), index(idx), {SortOrderRole});
}

QVariantList RoomListModel::getCategoriesWithChannels() const
{
    // Collect categories
    struct CategoryInfo {
        QString categoryId;
        QString categoryName;
        int sortOrder;
    };

    QVector<CategoryInfo> categories;
    QMap<QString, QVector<int>> categoryChannels; // categoryId -> room indices

    for (int i = 0; i < m_rooms.size(); ++i) {
        const auto& room = m_rooms[i];
        if (room.roomType == QStringLiteral("category")) {
            categories.append({room.roomId,
                               room.displayName.isEmpty() ? room.roomId : room.displayName,
                               room.sortOrder});
            // Ensure entry exists in map
            if (!categoryChannels.contains(room.roomId)) {
                categoryChannels[room.roomId] = {};
            }
        }
    }

    // Sort categories by sortOrder
    std::sort(categories.begin(), categories.end(),
              [](const CategoryInfo& a, const CategoryInfo& b) {
                  return a.sortOrder < b.sortOrder;
              });

    // Assign channels to categories
    bool hasUncategorized = false;
    for (int i = 0; i < m_rooms.size(); ++i) {
        const auto& room = m_rooms[i];
        if (room.roomType == QStringLiteral("category")) continue;

        QString catId = room.parentId;
        if (catId.isEmpty() || !categoryChannels.contains(catId)) {
            // Uncategorized
            categoryChannels[QString()].append(i);
            hasUncategorized = true;
        } else {
            categoryChannels[catId].append(i);
        }
    }

    // Build result
    QVariantList result;

    for (const auto& cat : categories) {
        QVariantMap catMap;
        catMap[QStringLiteral("categoryId")] = cat.categoryId;
        catMap[QStringLiteral("categoryName")] = cat.categoryName;
        catMap[QStringLiteral("sortOrder")] = cat.sortOrder;

        QVariantList channels;
        auto indices = categoryChannels.value(cat.categoryId);
        // Sort channels by sortOrder
        std::sort(indices.begin(), indices.end(),
                  [this](int a, int b) {
                      return m_rooms[a].sortOrder < m_rooms[b].sortOrder;
                  });
        for (int idx : indices) {
            const auto& room = m_rooms[idx];
            QVariantMap ch;
            ch[QStringLiteral("roomId")] = room.roomId;
            ch[QStringLiteral("displayName")] = room.displayName.isEmpty() ? room.roomId : room.displayName;
            ch[QStringLiteral("roomType")] = room.roomType;
            ch[QStringLiteral("isVoice")] = room.isVoice;
            ch[QStringLiteral("unreadCount")] = room.unreadCount;
            ch[QStringLiteral("voiceMemberCount")] = room.voiceMemberCount;
            ch[QStringLiteral("topic")] = room.topic;
            ch[QStringLiteral("sortOrder")] = room.sortOrder;
            channels.append(ch);
        }
        catMap[QStringLiteral("channels")] = channels;
        result.append(catMap);
    }

    // Add uncategorized if any
    if (hasUncategorized || categoryChannels.contains(QString())) {
        auto indices = categoryChannels.value(QString());
        if (!indices.isEmpty()) {
            QVariantMap catMap;
            catMap[QStringLiteral("categoryId")] = QString();
            catMap[QStringLiteral("categoryName")] = QStringLiteral("Uncategorized");
            catMap[QStringLiteral("sortOrder")] = 999999;

            QVariantList channels;
            std::sort(indices.begin(), indices.end(),
                      [this](int a, int b) {
                          return m_rooms[a].sortOrder < m_rooms[b].sortOrder;
                      });
            for (int idx : indices) {
                const auto& room = m_rooms[idx];
                QVariantMap ch;
                ch[QStringLiteral("roomId")] = room.roomId;
                ch[QStringLiteral("displayName")] = room.displayName.isEmpty() ? room.roomId : room.displayName;
                ch[QStringLiteral("roomType")] = room.roomType;
                ch[QStringLiteral("isVoice")] = room.isVoice;
                ch[QStringLiteral("unreadCount")] = room.unreadCount;
                ch[QStringLiteral("voiceMemberCount")] = room.voiceMemberCount;
                ch[QStringLiteral("topic")] = room.topic;
                ch[QStringLiteral("sortOrder")] = room.sortOrder;
                channels.append(ch);
            }
            catMap[QStringLiteral("channels")] = channels;
            result.append(catMap);
        }
    }

    return result;
}
