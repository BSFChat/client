#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVector>

class ServerListModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles {
        DisplayNameRole = Qt::UserRole + 1,
        ServerUrlRole,
        IconUrlRole,
        UnreadCountRole,
        // Mentions of the user in the server's channels, shown on the rail
        // even when the channel is muted (a mention outranks a mute).
        MentionCountRole
    };

    explicit ServerListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void addServer(const QString& displayName, const QString& serverUrl);
    void removeServer(int index);
    void updateServer(int index, const QString& displayName, const QString& serverUrl);
    void setUnreadCount(int index, int count);
    void setMentionCount(int index, int count);
    void setIconUrl(int index, const QString& iconUrl);

private:
    struct ServerEntry {
        QString displayName;
        QString serverUrl;
        QString iconUrl;
        int unreadCount = 0;
        int mentionCount = 0;
    };

    QVector<ServerEntry> m_servers;
};
