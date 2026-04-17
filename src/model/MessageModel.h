#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVector>

#include <bsfchat/MatrixTypes.h>

class MessageModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        EventIdRole = Qt::UserRole + 1,
        SenderRole,
        SenderDisplayNameRole,
        BodyRole,
        FormattedBodyRole,
        TimestampRole,
        MsgtypeRole,
        IsOwnMessageRole,
        ShowSenderRole,     // Whether to show sender info (for grouping)
        ShowDateSeparator,  // Whether to show a date separator above this message
        MediaUrlRole,       // Resolved HTTP URL for media messages
        MediaFileNameRole,  // Filename from media content
        MediaFileSizeRole,  // File size from media content info
        EditedRole          // Whether this message has been edited at least once
    };

    explicit MessageModel(QObject* parent = nullptr);

    void setHomeserver(const QString& homeserver) { m_homeserver = homeserver; }
    QString homeserver() const { return m_homeserver; }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Display-name cache — pointer to ServerConnection's global map so we
    // can resolve @user:host → "Josh" at render time. Not owned.
    void setDisplayNameCache(const QMap<QString, QString>* cache) { m_dnCache = cache; }

    void appendEvent(const bsfchat::RoomEvent& event, const QString& ownUserId);
    void appendEvents(const QVector<bsfchat::RoomEvent>& events, const QString& ownUserId);
    void prependEvents(const QVector<bsfchat::RoomEvent>& events, const QString& ownUserId);
    void clear();

    // Re-resolve every sender display name from the cache and emit
    // dataChanged so the UI updates when a user changes their profile.
    void refreshDisplayNames();

signals:
    void countChanged();

private:
    struct MessageEntry {
        QString eventId;
        QString sender;
        QString senderDisplayName;
        QString body;
        QString formattedBody;
        qint64 timestamp = 0;
        QString msgtype;
        bool isOwnMessage = false;
        QString mediaUrl;       // Resolved HTTP URL for m.image/m.file
        QString mediaFileName;  // Filename from content
        qint64 mediaFileSize = 0; // Size in bytes
        bool edited = false;    // True if ≥1 m.replace has been applied
        qint64 editedAt = 0;    // Timestamp of the latest edit
    };

    QVector<MessageEntry> m_messages;
    QString m_homeserver;
    const QMap<QString, QString>* m_dnCache = nullptr;

    MessageEntry eventToEntry(const bsfchat::RoomEvent& event, const QString& ownUserId) const;
    QString resolveMediaUrl(const QString& mxcUri) const;
    QString resolveDisplayName(const QString& userId) const;
};
