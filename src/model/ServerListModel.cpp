#include "model/ServerListModel.h"

ServerListModel::ServerListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ServerListModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) return 0;
    return m_servers.size();
}

QVariant ServerListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_servers.size())
        return {};

    const auto& server = m_servers[index.row()];
    switch (role) {
    case DisplayNameRole: return server.displayName;
    case ServerUrlRole: return server.serverUrl;
    case IconUrlRole: return server.iconUrl;
    case UnreadCountRole: return server.unreadCount;
    default: return {};
    }
}

QHash<int, QByteArray> ServerListModel::roleNames() const
{
    return {
        {DisplayNameRole, "displayName"},
        {ServerUrlRole, "serverUrl"},
        {IconUrlRole, "iconUrl"},
        {UnreadCountRole, "unreadCount"}
    };
}

void ServerListModel::addServer(const QString& displayName, const QString& serverUrl)
{
    beginInsertRows(QModelIndex(), m_servers.size(), m_servers.size());
    m_servers.append({displayName, serverUrl, {}, 0});
    endInsertRows();
}

void ServerListModel::removeServer(int index)
{
    if (index < 0 || index >= m_servers.size()) return;
    beginRemoveRows(QModelIndex(), index, index);
    m_servers.removeAt(index);
    endRemoveRows();
}

void ServerListModel::updateServer(int index, const QString& displayName, const QString& serverUrl)
{
    if (index < 0 || index >= m_servers.size()) return;
    m_servers[index].displayName = displayName;
    m_servers[index].serverUrl = serverUrl;
    emit dataChanged(this->index(index), this->index(index));
}

void ServerListModel::setUnreadCount(int index, int count)
{
    if (index < 0 || index >= m_servers.size()) return;
    if (m_servers[index].unreadCount == count) return;
    m_servers[index].unreadCount = count;
    emit dataChanged(this->index(index), this->index(index), {UnreadCountRole});
}
