#pragma once

#include <QAbstractListModel>
#include <QMap>
#include <QString>
#include <QVector>

#include <bsfchat/MatrixTypes.h>

#include "util/BotRegistry.h"

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
        NicknameRole,
        // True when this member is a bot account, for the BOT badge.
        //
        // Read through the shared BotRegistry rather than stored on the row,
        // because it does not arrive with the member event that creates the
        // row — the flag lives on the user's PROFILE, and the profile reply
        // for a member typically lands after the roster has already been
        // built. Storing it would mean reconciling two arrival orders; a
        // lookup at read time means the answer is simply whatever is known
        // now, and the registry's owner repaints when that changes.
        IsBotRole
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

    // Bot-flag cache (owned by ServerConnection). Same not-owned pointer
    // arrangement as the display-name cache above and for the same reason:
    // the answers arrive on their own schedule from profile replies, and
    // reading through the owner means nothing here has to be told twice.
    void setBotRegistry(const bsfchat::client::BotRegistry* registry)
    {
        m_botRegistry = registry;
    }
    // Repaint every row's badge. Called when a batch of profile replies has
    // moved at least one flag; cheap enough to do wholesale because it names
    // the single role that can have changed.
    void refreshBotFlags();

    // Whether a member is a bot, for QML that has a user id but not a row
    // (the profile card, the message bubble's sender). Invokable for the
    // same reason displayNameForUser is — QML calls it directly.
    Q_INVOKABLE bool isBot(const QString& userId) const;

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
    const bsfchat::client::BotRegistry* m_botRegistry = nullptr;

    QString resolveName(const QString& userId, const QString& localName) const;
};
