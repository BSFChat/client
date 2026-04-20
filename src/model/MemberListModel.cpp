#include "model/MemberListModel.h"

#include <bsfchat/Constants.h>

MemberListModel::MemberListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int MemberListModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) return 0;
    return m_members.size();
}

QVariant MemberListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_members.size())
        return {};

    const auto& member = m_members[index.row()];
    switch (role) {
    case UserIdRole: return member.userId;
    case DisplayNameRole: return resolveName(member.userId, member.displayName);
    case AvatarUrlRole: return member.avatarUrl;
    case MembershipRole: return member.membership;
    default: return {};
    }
}

QString MemberListModel::resolveName(const QString& userId, const QString& localName) const
{
    if (!localName.isEmpty()) return localName;
    if (m_dnCache) {
        auto it = m_dnCache->find(userId);
        if (it != m_dnCache->end() && !it->isEmpty()) return *it;
    }
    // Strip @localpart:host → localpart as final fallback.
    if (userId.startsWith('@')) {
        int colon = userId.indexOf(':');
        if (colon > 1) return userId.mid(1, colon - 1);
    }
    return userId;
}

void MemberListModel::refreshDisplayNames()
{
    if (m_members.isEmpty()) return;
    // Tell views to re-read DisplayNameRole for every row.
    emit dataChanged(index(0), index(m_members.size() - 1), {DisplayNameRole});
}

QHash<int, QByteArray> MemberListModel::roleNames() const
{
    return {
        {UserIdRole, "userId"},
        {DisplayNameRole, "displayName"},
        {AvatarUrlRole, "avatarUrl"},
        {MembershipRole, "membership"}
    };
}

int MemberListModel::findMember(const QString& userId) const
{
    for (int i = 0; i < m_members.size(); ++i) {
        if (m_members[i].userId == userId) return i;
    }
    return -1;
}

void MemberListModel::processEvent(const bsfchat::RoomEvent& event)
{
    if (event.type != std::string(bsfchat::event_type::kRoomMember))
        return;

    QString userId = event.state_key.has_value()
        ? QString::fromStdString(*event.state_key)
        : QString::fromStdString(event.sender);

    QString membership = QString::fromStdString(event.content.data.value("membership", ""));
    QString displayName = QString::fromStdString(event.content.data.value("displayname", ""));
    QString avatarUrl = QString::fromStdString(event.content.data.value("avatar_url", ""));

    int idx = findMember(userId);

    if (membership == "join") {
        if (idx >= 0) {
            // Update existing member
            m_members[idx].displayName = displayName;
            m_members[idx].avatarUrl = avatarUrl;
            m_members[idx].membership = membership;
            emit dataChanged(index(idx), index(idx));
        } else {
            // Add new member
            beginInsertRows(QModelIndex(), m_members.size(), m_members.size());
            m_members.append({userId, displayName, avatarUrl, membership});
            endInsertRows();
        }
    } else if (membership == "leave" || membership == "ban") {
        if (idx >= 0) {
            beginRemoveRows(QModelIndex(), idx, idx);
            m_members.removeAt(idx);
            endRemoveRows();
        }
    }
}

QString MemberListModel::displayNameForUser(const QString& userId) const
{
    int idx = findMember(userId);
    if (idx >= 0) {
        return resolveName(userId, m_members[idx].displayName);
    }
    return {};
}

void MemberListModel::clear()
{
    beginResetModel();
    m_members.clear();
    endResetModel();
}
