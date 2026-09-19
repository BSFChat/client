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
    case NicknameRole: return member.nickname;
    case IsBotRole: return member.isBot;
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

bool MemberListModel::isBot(const QString& userId) const
{
    const int idx = findMember(userId);
    return idx >= 0 && m_members[idx].isBot;
}

QHash<int, QByteArray> MemberListModel::roleNames() const
{
    return {
        {UserIdRole, "userId"},
        {DisplayNameRole, "displayName"},
        {AvatarUrlRole, "avatarUrl"},
        {MembershipRole, "membership"},
        {NicknameRole, "nickname"},
        {IsBotRole, "isBot"}
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
    // The server writes the EFFECTIVE name into `displayname` and the nickname
    // separately, so `displayName` above is already the string to render and this
    // is only for telling the two apart in admin UI.
    QString nickname = QString::fromStdString(
        event.content.data.value(std::string("bsfchat.nickname"), std::string()));
    // Absent is the server's encoding for "human" — it never writes `false` —
    // so a default of false is the whole of the rule, and there is no third
    // "not known yet" state to represent.
    bool isBotAccount = event.content.data.value(std::string("bsfchat.bot"), false);

    int idx = findMember(userId);

    if (membership == "join") {
        if (idx >= 0) {
            // Update existing member
            m_members[idx].displayName = displayName;
            m_members[idx].avatarUrl = avatarUrl;
            m_members[idx].membership = membership;
            m_members[idx].nickname = nickname;
            m_members[idx].isBot = isBotAccount;
            // U-M15: an empty role vector means "every role changed", which
            // makes every delegate binding on this row re-evaluate. Name the
            // five fields this branch can actually move.
            emit dataChanged(index(idx), index(idx),
                             {DisplayNameRole, AvatarUrlRole,
                              MembershipRole, NicknameRole, IsBotRole});
        } else {
            // Add new member
            beginInsertRows(QModelIndex(), m_members.size(), m_members.size());
            m_members.append({userId, displayName, avatarUrl, membership,
                              nickname, isBotAccount});
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

QString MemberListModel::nicknameForUser(const QString& userId) const
{
    int idx = findMember(userId);
    if (idx >= 0) return m_members[idx].nickname;
    return {};
}

void MemberListModel::clear()
{
    beginResetModel();
    m_members.clear();
    endResetModel();
}
