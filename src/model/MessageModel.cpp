#include "model/MessageModel.h"

#include <QDateTime>
#include <bsfchat/Constants.h>
#include "util/MarkdownParser.h"

MessageModel::MessageModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int MessageModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) return 0;
    return m_messages.size();
}

QVariant MessageModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_messages.size())
        return {};

    const auto& msg = m_messages[index.row()];
    switch (role) {
    case EventIdRole: return msg.eventId;
    case SenderRole: return msg.sender;
    case SenderDisplayNameRole: return msg.senderDisplayName;
    case BodyRole: return msg.body;
    case FormattedBodyRole: return msg.formattedBody;
    case TimestampRole: return msg.timestamp;
    case MsgtypeRole: return msg.msgtype;
    case IsOwnMessageRole: return msg.isOwnMessage;
    case ShowSenderRole: {
        if (index.row() == 0) return true;
        const auto& prev = m_messages[index.row() - 1];
        if (prev.sender != msg.sender) return true;
        // Show sender again if more than 5 minutes have passed
        return (msg.timestamp - prev.timestamp) > 5 * 60 * 1000;
    }
    case ShowDateSeparator: {
        if (index.row() == 0) return true;
        const auto& prev = m_messages[index.row() - 1];
        // Show separator if different calendar day
        QDateTime prevDt = QDateTime::fromMSecsSinceEpoch(prev.timestamp);
        QDateTime curDt = QDateTime::fromMSecsSinceEpoch(msg.timestamp);
        return prevDt.date() != curDt.date();
    }
    case MediaUrlRole: return msg.mediaUrl;
    case MediaFileNameRole: return msg.mediaFileName;
    case MediaFileSizeRole: return msg.mediaFileSize;
    default: return {};
    }
}

QHash<int, QByteArray> MessageModel::roleNames() const
{
    return {
        {EventIdRole, "eventId"},
        {SenderRole, "sender"},
        {SenderDisplayNameRole, "senderDisplayName"},
        {BodyRole, "body"},
        {FormattedBodyRole, "formattedBody"},
        {TimestampRole, "timestamp"},
        {MsgtypeRole, "msgtype"},
        {IsOwnMessageRole, "isOwnMessage"},
        {ShowSenderRole, "showSender"},
        {ShowDateSeparator, "showDateSeparator"},
        {MediaUrlRole, "mediaUrl"},
        {MediaFileNameRole, "mediaFileName"},
        {MediaFileSizeRole, "mediaFileSize"}
    };
}

QString MessageModel::resolveMediaUrl(const QString& mxcUri) const
{
    if (!mxcUri.startsWith("mxc://") || m_homeserver.isEmpty())
        return {};
    QString path = mxcUri.mid(6); // strip "mxc://"
    return m_homeserver + QString::fromUtf8(bsfchat::api_path::kMediaDownload) + path;
}

MessageModel::MessageEntry MessageModel::eventToEntry(const bsfchat::RoomEvent& event, const QString& ownUserId) const
{
    MessageEntry entry;
    entry.eventId = QString::fromStdString(event.event_id);
    entry.sender = QString::fromStdString(event.sender);
    entry.senderDisplayName = entry.sender; // TODO: resolve display names
    entry.timestamp = event.origin_server_ts;
    entry.isOwnMessage = (entry.sender == ownUserId);

    entry.msgtype = QString::fromStdString(event.content.data.value("msgtype", ""));
    entry.body = QString::fromStdString(event.content.data.value("body", ""));
    entry.formattedBody = QString::fromStdString(event.content.data.value("formatted_body", ""));

    // Extract media fields for image/file messages
    if (entry.msgtype == "m.image" || entry.msgtype == "m.file" ||
        entry.msgtype == "m.audio" || entry.msgtype == "m.video") {
        QString mxcUrl = QString::fromStdString(event.content.data.value("url", ""));
        entry.mediaUrl = resolveMediaUrl(mxcUrl);
        entry.mediaFileName = entry.body; // body is the filename in media messages

        if (event.content.data.contains("info") && event.content.data["info"].is_object()) {
            const auto& info = event.content.data["info"];
            entry.mediaFileSize = info.value("size", 0);
        }
    }

    // If no formatted_body from server, apply local markdown rendering
    if (entry.formattedBody.isEmpty() && !entry.body.isEmpty() && entry.msgtype == "m.text") {
        entry.formattedBody = MarkdownParser::toHtml(entry.body);
    }

    return entry;
}

void MessageModel::appendEvent(const bsfchat::RoomEvent& event, const QString& ownUserId)
{
    // Only add message events
    if (event.type != std::string(bsfchat::event_type::kRoomMessage))
        return;

    // Check for duplicates
    QString eventId = QString::fromStdString(event.event_id);
    for (const auto& msg : m_messages) {
        if (msg.eventId == eventId) return;
    }

    beginInsertRows(QModelIndex(), m_messages.size(), m_messages.size());
    m_messages.append(eventToEntry(event, ownUserId));
    endInsertRows();
    emit countChanged();
}

void MessageModel::appendEvents(const QVector<bsfchat::RoomEvent>& events, const QString& ownUserId)
{
    for (const auto& event : events) {
        appendEvent(event, ownUserId);
    }
}

void MessageModel::prependEvents(const QVector<bsfchat::RoomEvent>& events, const QString& ownUserId)
{
    QVector<MessageEntry> newEntries;
    for (const auto& event : events) {
        if (event.type != std::string(bsfchat::event_type::kRoomMessage))
            continue;
        QString eventId = QString::fromStdString(event.event_id);
        bool duplicate = false;
        for (const auto& msg : m_messages) {
            if (msg.eventId == eventId) { duplicate = true; break; }
        }
        if (!duplicate) {
            newEntries.append(eventToEntry(event, ownUserId));
        }
    }

    if (newEntries.isEmpty()) return;

    beginInsertRows(QModelIndex(), 0, newEntries.size() - 1);
    for (int i = newEntries.size() - 1; i >= 0; --i) {
        m_messages.prepend(newEntries[i]);
    }
    endInsertRows();
    emit countChanged();
}

void MessageModel::clear()
{
    beginResetModel();
    m_messages.clear();
    endResetModel();
    emit countChanged();
}
