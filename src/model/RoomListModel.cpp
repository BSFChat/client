#include "model/RoomListModel.h"

#include <QVariantMap>
#include <algorithm>

using bsfchat::client::ChannelRestoreCandidate;
using bsfchat::client::ChannelRestoreChoice;
using bsfchat::client::VoiceParticipant;
using bsfchat::client::VoiceRoster;

namespace {

// One participant as QML consumes it. Keys match the wire names the voice
// panel already reads (`user_id`, `muted`, …) so the sidebar rows and the
// in-call tiles can share delegates without a translation layer.
QVariantList voiceRosterToVariantList(const VoiceRoster& roster)
{
    QVariantList out;
    out.reserve(roster.size());
    for (const auto& p : roster) {
        QVariantMap m;
        m[QStringLiteral("user_id")] = p.userId;
        m[QStringLiteral("muted")] = p.muted;
        m[QStringLiteral("deafened")] = p.deafened;
        m[QStringLiteral("cameraOn")] = p.cameraOn;
        m[QStringLiteral("screenSharing")] = p.screenSharing;
        m[QStringLiteral("joined_at")] = p.joinedAt;
        out.append(m);
    }
    return out;
}

} // namespace

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
    case MentionCountRole: return room.mentionCount;
    case LastMessageRole: return room.lastMessage;
    case LastMessageTimeRole: return room.lastMessageTime;
    case IsVoiceRole: return room.isVoice;
    case VoiceMemberCountRole: return int(room.voiceMembers.size());
    case VoiceMembersRole: return voiceRosterToVariantList(room.voiceMembers);
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
        {MentionCountRole, "mentionCount"},
        {LastMessageRole, "lastMessage"},
        {LastMessageTimeRole, "lastMessageTime"},
        {IsVoiceRole, "isVoice"},
        {VoiceMemberCountRole, "voiceMemberCount"},
        {VoiceMembersRole, "voiceMembers"},
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
    // Only the id is known at this point; everything else keeps its in-class
    // default. (Was a positional brace-init, which silently shifted every
    // field the first time one was inserted into the middle of RoomEntry.)
    RoomEntry entry;
    entry.roomId = roomId;
    m_rooms.append(std::move(entry));
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
    // Opening a room (or advancing its read marker) clears its mention badge
    // too — you have now seen the mention. Done here rather than at the call
    // sites so both of them get it without touching ServerConnection.
    int idx = findRoom(roomId);
    if (idx >= 0 && m_rooms[idx].mentionCount != 0) {
        m_rooms[idx].mentionCount = 0;
        emit dataChanged(index(idx), index(idx), {MentionCountRole});
    }
    setUnreadCount(roomId, 0);
}

void RoomListModel::incrementMentionCount(const QString& roomId, int count)
{
    if (count <= 0) return;
    int idx = findRoom(roomId);
    if (idx < 0) return;
    m_rooms[idx].mentionCount += count;
    emit dataChanged(index(idx), index(idx), {MentionCountRole});
}

void RoomListModel::setMentionCount(const QString& roomId, int count)
{
    if (count < 0) count = 0;
    int idx = findRoom(roomId);
    if (idx < 0) return;
    // Silent when unchanged — the server re-reports the same highlight_count on
    // every poll for as long as the mention stays unread, and repainting the
    // row each time would make the sidebar's dataChanged traffic proportional
    // to the sync rate rather than to actual changes.
    if (m_rooms[idx].mentionCount == count) return;
    m_rooms[idx].mentionCount = count;
    emit dataChanged(index(idx), index(idx), {MentionCountRole});
}

int RoomListModel::mentionCountFor(const QString& roomId) const
{
    int idx = findRoom(roomId);
    if (idx < 0) return 0;
    return m_rooms[idx].mentionCount;
}

int RoomListModel::totalMentionCount() const
{
    int total = 0;
    for (const auto& room : m_rooms) total += room.mentionCount;
    return total;
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

QString RoomListModel::roomIdForName(const QString& name) const
{
    if (name.isEmpty()) return {};
    const QString needle = name.trimmed();
    for (const auto& r : m_rooms) {
        // Only match text channels — voice channels wouldn't make sense
        // as a #mention target. roomType is empty for regular text rooms.
        if (r.isVoice) continue;
        if (QString::compare(r.displayName, needle, Qt::CaseInsensitive) == 0)
            return r.roomId;
    }
    return {};
}

QString RoomListModel::roomTopic(const QString& roomId) const
{
    int idx = findRoom(roomId);
    if (idx < 0) return {};
    return m_rooms[idx].topic;
}

int RoomListModel::unreadCountFor(const QString& roomId) const
{
    int idx = findRoom(roomId);
    if (idx < 0) return 0;
    return m_rooms[idx].unreadCount;
}

bool RoomListModel::isVoiceRoom(const QString& roomId) const
{
    int idx = findRoom(roomId);
    if (idx < 0) return false;
    return m_rooms[idx].isVoice;
}

bool RoomListModel::hasRoom(const QString& roomId) const
{
    return findRoom(roomId) >= 0;
}

QString RoomListModel::firstTextRoomId() const
{
    // Pick the first room that isn't a voice channel, in current
    // model order. We don't re-sort here — the channel list as the
    // user sees it already follows category + sortOrder, and picking
    // a different room than what's visually on top of the drawer
    // would be confusing.
    for (const auto& r : m_rooms) {
        if (!r.isVoice && r.roomType != "m.space")
            return r.roomId;
    }
    return {};
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

bool RoomListModel::applyCallMember(const QString& roomId,
                                    const VoiceParticipant& participant,
                                    bool active)
{
    // ensureRoom, not a findRoom guard: an m.call.member can legitimately be
    // the first thing we learn about a voice channel during an initial sync,
    // and dropping it would leave the channel showing an empty roster until
    // somebody joined again.
    ensureRoom(roomId);
    int idx = findRoom(roomId);
    if (idx < 0) return false;
    if (!bsfchat::client::applyCallMember(m_rooms[idx].voiceMembers,
                                          participant, active))
        return false;
    emit dataChanged(index(idx), index(idx),
                     {VoiceMemberCountRole, VoiceMembersRole});
    return true;
}

const VoiceRoster& RoomListModel::voiceMembers(const QString& roomId) const
{
    static const VoiceRoster kEmpty;
    int idx = findRoom(roomId);
    if (idx < 0) return kEmpty;
    return m_rooms[idx].voiceMembers;
}

int RoomListModel::voiceMemberCount(const QString& roomId) const
{
    return int(voiceMembers(roomId).size());
}

ChannelRestoreChoice RoomListModel::restoreChoice(const QString& remembered,
                                           bool syncComplete) const
{
    QVector<ChannelRestoreCandidate> candidates;
    candidates.reserve(m_rooms.size());
    for (const auto& r : m_rooms) {
        candidates.append({r.roomId, r.isVoice,
                           r.roomType == QStringLiteral("category")
                               || r.roomType == QStringLiteral("m.space")});
    }
    return bsfchat::client::chooseChannelToRestore(remembered, candidates,
                                                 syncComplete);
}

QString RoomListModel::restoreTargetRoomId(const QString& remembered,
                                           bool syncComplete) const
{
    return restoreChoice(remembered, syncComplete).roomId;
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

QStringList RoomListModel::pruneRoomsNotIn(const QSet<QString>& keep)
{
    QStringList removed;
    // Walk backwards so indices stay valid as we delete.
    for (int i = m_rooms.size() - 1; i >= 0; --i) {
        if (!keep.contains(m_rooms[i].roomId)) {
            removed.append(m_rooms[i].roomId);
            beginRemoveRows(QModelIndex(), i, i);
            m_rooms.removeAt(i);
            endRemoveRows();
        }
    }
    return removed;
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

    // Build result. Uncategorized comes first (Discord convention) so orphan
    // channels sit above any named categories and can't be visually confused
    // with channels that actually live inside the first category.
    QVariantList result;

    auto roomToMap = [this](int idx) {
        const auto& room = m_rooms[idx];
        QVariantMap ch;
        ch[QStringLiteral("roomId")] = room.roomId;
        ch[QStringLiteral("displayName")] = room.displayName.isEmpty() ? room.roomId : room.displayName;
        ch[QStringLiteral("roomType")] = room.roomType;
        ch[QStringLiteral("isVoice")] = room.isVoice;
        ch[QStringLiteral("unreadCount")] = room.unreadCount;
        ch[QStringLiteral("mentionCount")] = room.mentionCount;
        // Count and roster come out of the same vector, so the badge can
        // never outlive the last participant it was counting.
        ch[QStringLiteral("voiceMemberCount")] = int(room.voiceMembers.size());
        ch[QStringLiteral("voiceMembers")] = voiceRosterToVariantList(room.voiceMembers);
        ch[QStringLiteral("topic")] = room.topic;
        ch[QStringLiteral("sortOrder")] = room.sortOrder;
        ch[QStringLiteral("lastMessageTime")] = room.lastMessageTime;
        return ch;
    };

    // Uncategorized section first, if any.
    {
        auto indices = categoryChannels.value(QString());
        if (!indices.isEmpty()) {
            QVariantMap catMap;
            catMap[QStringLiteral("categoryId")] = QString();
            catMap[QStringLiteral("categoryName")] = QStringLiteral("Uncategorized");
            catMap[QStringLiteral("sortOrder")] = -1;

            QVariantList channels;
            std::sort(indices.begin(), indices.end(),
                      [this](int a, int b) {
                          return m_rooms[a].sortOrder < m_rooms[b].sortOrder;
                      });
            for (int idx : indices) channels.append(roomToMap(idx));
            catMap[QStringLiteral("channels")] = channels;
            result.append(catMap);
        }
        Q_UNUSED(hasUncategorized);
    }

    for (const auto& cat : categories) {
        QVariantMap catMap;
        catMap[QStringLiteral("categoryId")] = cat.categoryId;
        catMap[QStringLiteral("categoryName")] = cat.categoryName;
        catMap[QStringLiteral("sortOrder")] = cat.sortOrder;

        QVariantList channels;
        auto indices = categoryChannels.value(cat.categoryId);
        std::sort(indices.begin(), indices.end(),
                  [this](int a, int b) {
                      return m_rooms[a].sortOrder < m_rooms[b].sortOrder;
                  });
        for (int idx : indices) channels.append(roomToMap(idx));
        catMap[QStringLiteral("channels")] = channels;
        result.append(catMap);
    }

    return result;
}
