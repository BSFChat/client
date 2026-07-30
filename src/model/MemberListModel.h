#pragma once

#include <QAbstractListModel>
#include <QMap>
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
        MembershipRole,
        // The per-server nickname, empty when the member has none.
        //
        // DisplayNameRole is already the name to RENDER — the server puts the
        // effective name (nickname if set, else global) in the member event's
        // `displayname`, so nothing has to choose between them at paint time. This
        // role exists so the UI can tell WHY a name is what it is: an admin needs
        // "clear nickname" to be distinguishable from "reset to nothing", and a
        // profile card wants to show the global name underneath.
        NicknameRole
    };

    explicit MemberListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void processEvent(const bsfchat::RoomEvent& event);
    void clear();

    // Invokable because QML calls it (MessageBubble's reaction tooltip did, and
    // silently got "not a function" because it was a plain member).
    Q_INVOKABLE QString displayNameForUser(const QString& userId) const;

    // The member's per-server nickname, or an empty string when they have none.
    Q_INVOKABLE QString nicknameForUser(const QString& userId) const;

    // Global user display-name cache (owned by ServerConnection). Used as a
    // fallback when the member event didn't carry a displayname (older
    // rooms, or events written before the server's broadcast was added).
    void setDisplayNameCache(const QMap<QString, QString>* cache) { m_dnCache = cache; }
    // Re-resolve every member's display name from the cache and refresh.
    void refreshDisplayNames();

private:
    int findMember(const QString& userId) const;

    struct MemberEntry {
        QString userId;
        QString displayName;
        QString avatarUrl;
        QString membership;
        QString nickname;
    };

    QVector<MemberEntry> m_members;
    const QMap<QString, QString>* m_dnCache = nullptr;

    QString resolveName(const QString& userId, const QString& localName) const;
};
