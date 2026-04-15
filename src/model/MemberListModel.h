#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVector>

#include <bsfchat/MatrixTypes.h>

class MemberListModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles {
        UserIdRole = Qt::UserRole + 1,
        DisplayNameRole,
        AvatarUrlRole,
        MembershipRole
    };

    explicit MemberListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void processEvent(const bsfchat::RoomEvent& event);
    void clear();

    QString displayNameForUser(const QString& userId) const;

private:
    int findMember(const QString& userId) const;

    struct MemberEntry {
        QString userId;
        QString displayName;
        QString avatarUrl;
        QString membership;
    };

    QVector<MemberEntry> m_members;
};
