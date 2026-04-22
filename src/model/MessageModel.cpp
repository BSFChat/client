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
    case EditedRole: return msg.edited;
    case ReplyToEventIdRole: return msg.replyToEventId;
    case ReplyToSenderRole: return msg.replyToSender;
    case ReplyPreviewRole: return msg.replyPreview;
    case ReactionsRole: return buildReactionsList(msg);
    case ThreadRootIdRole: return msg.threadRootId;
    case ThreadReplyCountRole: return threadReplyCount(msg.eventId);
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
        {MediaFileSizeRole, "mediaFileSize"},
        {EditedRole, "edited"},
        {ReplyToEventIdRole, "replyToEventId"},
        {ReplyToSenderRole, "replyToSender"},
        {ReplyPreviewRole, "replyPreview"},
        {ReactionsRole, "reactions"},
        {ThreadRootIdRole, "threadRootId"},
        {ThreadReplyCountRole, "threadReplyCount"}
    };
}

QVariantList MessageModel::buildReactionsList(const MessageEntry& entry) const
{
    // Build a stable list: sorted by emoji so the UI doesn't reshuffle chips
    // on every sync. (QHash doesn't guarantee iteration order.)
    QVariantList out;
    QStringList keys = entry.reactionsByEmoji.keys();
    std::sort(keys.begin(), keys.end());
    for (const auto& emoji : keys) {
        const auto& list = entry.reactionsByEmoji[emoji];
        if (list.isEmpty()) continue;
        QVariantMap m;
        m[QStringLiteral("emoji")] = emoji;
        m[QStringLiteral("count")] = list.size();
        bool reacted = false;
        QStringList eventIds;
        QStringList userIds;
        for (const auto& p : list) {
            eventIds.append(p.second);
            userIds.append(p.first);
            if (!m_ownUserId.isEmpty() && p.first == m_ownUserId) reacted = true;
        }
        m[QStringLiteral("reacted")] = reacted;
        m[QStringLiteral("eventIds")] = eventIds;
        m[QStringLiteral("userIds")] = userIds;
        out.append(m);
    }
    return out;
}

QVariantMap MessageModel::reactionSummary(int idx) const
{
    QVariantMap out;
    if (idx < 0 || idx >= m_messages.size()) return out;
    out[QStringLiteral("reactions")] = buildReactionsList(m_messages[idx]);
    return out;
}

QString MessageModel::ownReactionEventId(const QString& targetEventId, const QString& emoji,
                                          const QString& userId) const
{
    for (const auto& msg : m_messages) {
        if (msg.eventId != targetEventId) continue;
        auto it = msg.reactionsByEmoji.find(emoji);
        if (it == msg.reactionsByEmoji.end()) return {};
        for (const auto& p : it.value()) {
            if (p.first == userId) return p.second;
        }
        return {};
    }
    return {};
}

int MessageModel::applyReactionToTarget(const QString& targetEventId, const QString& emoji,
                                         const QString& userId, const QString& reactionEventId)
{
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].eventId != targetEventId) continue;
        auto& bucket = m_messages[i].reactionsByEmoji[emoji];
        // Dedupe by reaction event id — sync may replay.
        for (const auto& p : bucket) {
            if (p.second == reactionEventId) return i;
        }
        bucket.append(qMakePair(userId, reactionEventId));
        m_reactionIndex.insert(reactionEventId,
                               ReactionRef{targetEventId, emoji, userId});
        return i;
    }
    return -1;
}

int MessageModel::indexForEventId(const QString& eventId) const
{
    if (eventId.isEmpty()) return -1;
    for (int i = m_messages.size() - 1; i >= 0; --i) {
        if (m_messages[i].eventId == eventId) return i;
    }
    return -1;
}

QVariantList MessageModel::threadReplies(const QString& rootEventId) const
{
    QVariantList out;
    if (rootEventId.isEmpty()) return out;
    for (const auto& m : m_messages) {
        if (m.threadRootId != rootEventId) continue;
        QVariantMap row;
        row[QStringLiteral("eventId")] = m.eventId;
        row[QStringLiteral("sender")] = m.sender;
        row[QStringLiteral("senderDisplayName")] = m.senderDisplayName;
        row[QStringLiteral("body")] = m.body;
        row[QStringLiteral("timestamp")] = m.timestamp;
        row[QStringLiteral("msgtype")] = m.msgtype;
        row[QStringLiteral("isOwnMessage")] = m.isOwnMessage;
        out.append(row);
    }
    return out;
}

int MessageModel::threadReplyCount(const QString& rootEventId) const
{
    if (rootEventId.isEmpty()) return 0;
    int n = 0;
    for (const auto& m : m_messages) {
        if (m.threadRootId == rootEventId) ++n;
    }
    return n;
}

QVariantMap MessageModel::eventPreview(const QString& eventId) const
{
    QVariantMap out;
    int idx = indexForEventId(eventId);
    if (idx < 0) return out;
    const auto& m = m_messages[idx];
    out[QStringLiteral("sender")] = m.senderDisplayName.isEmpty()
        ? m.sender : m.senderDisplayName;
    out[QStringLiteral("body")] = m.body.left(160);
    out[QStringLiteral("timestamp")] = m.timestamp;
    return out;
}

QString MessageModel::firstEventIdAfterTs(qint64 tsMs) const
{
    if (tsMs <= 0) return {};
    // m_messages is ordered oldest → newest. Linear scan from front is
    // fine for the 100-ish loaded events; early-returns on first hit.
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].timestamp > tsMs) return m_messages[i].eventId;
    }
    return {};
}

qint64 MessageModel::newestTimestampMs() const
{
    if (m_messages.isEmpty()) return 0;
    return m_messages.last().timestamp;
}

QVariantList MessageModel::searchMessages(const QString& query, int limit) const
{
    QVariantList out;
    if (query.trimmed().isEmpty()) return out;
    const QString needle = query.trimmed();
    // Newest-first so the most recent matches top the list.
    for (int i = m_messages.size() - 1; i >= 0 && out.size() < limit; --i) {
        const auto& m = m_messages[i];
        if (!m.body.contains(needle, Qt::CaseInsensitive)
            && !m.senderDisplayName.contains(needle, Qt::CaseInsensitive)) continue;
        QVariantMap row;
        row[QStringLiteral("eventId")] = m.eventId;
        row[QStringLiteral("sender")] = m.senderDisplayName.isEmpty()
            ? m.sender : m.senderDisplayName;
        row[QStringLiteral("body")] = m.body;
        row[QStringLiteral("timestamp")] = m.timestamp;
        out.append(row);
    }
    return out;
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
    entry.senderDisplayName = resolveDisplayName(entry.sender);
    entry.timestamp = event.origin_server_ts;
    entry.isOwnMessage = (entry.sender == ownUserId);

    entry.msgtype = QString::fromStdString(event.content.data.value("msgtype", ""));
    entry.body = QString::fromStdString(event.content.data.value("body", ""));
    entry.formattedBody = QString::fromStdString(event.content.data.value("formatted_body", ""));

    // Parse m.in_reply_to — pointer to the message this one replies to.
    // We don't mind m.replace (that's handled above); only genuine replies
    // carry an m.in_reply_to key.
    if (event.content.data.contains("m.relates_to")
        && event.content.data["m.relates_to"].is_object()) {
        const auto& rel = event.content.data["m.relates_to"];
        if (rel.contains("m.in_reply_to") && rel["m.in_reply_to"].is_object()) {
            entry.replyToEventId = QString::fromStdString(
                rel["m.in_reply_to"].value("event_id", ""));
        }
        // Thread relation — rel_type = "m.thread", event_id points at
        // the thread root. Spec'd under Matrix threading (MSC3440).
        if (rel.value("rel_type", "") == "m.thread") {
            entry.threadRootId = QString::fromStdString(rel.value("event_id", ""));
        }
    }

    if (!entry.replyToEventId.isEmpty()) {
        // Resolve the target from the already-ingested timeline, walking
        // backward on the assumption the reply target is usually recent.
        for (int i = m_messages.size() - 1; i >= 0; --i) {
            if (m_messages[i].eventId != entry.replyToEventId) continue;
            entry.replyToSender = m_messages[i].senderDisplayName;
            QString preview = m_messages[i].body;
            if (preview.size() > 80) preview = preview.left(80) + "…";
            entry.replyPreview = preview;
            break;
        }
    }

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
    // Cache the caller's identity so buildReactionsList() can mark chips the
    // current user has reacted to.
    m_ownUserId = ownUserId;

    // --- m.reaction (m.annotation) -------------------------------------
    // A reaction is a sibling event; it doesn't live in the message list,
    // but we fold its state into the target message's reactions map.
    if (event.type == "m.reaction") {
        const auto& data = event.content.data;
        if (!data.contains("m.relates_to") || !data["m.relates_to"].is_object())
            return;
        const auto& rel = data["m.relates_to"];
        if (rel.value("rel_type", "") != "m.annotation") return;
        QString targetId = QString::fromStdString(rel.value("event_id", ""));
        QString key = QString::fromStdString(rel.value("key", ""));
        if (targetId.isEmpty() || key.isEmpty()) return;
        QString reactionEventId = QString::fromStdString(event.event_id);
        QString sender = QString::fromStdString(event.sender);
        // Dedupe globally — if we've indexed this reaction id already, skip.
        if (m_reactionIndex.contains(reactionEventId)) return;
        int row = applyReactionToTarget(targetId, key, sender, reactionEventId);
        if (row >= 0) {
            auto idx = index(row);
            emit dataChanged(idx, idx, {ReactionsRole});
        } else {
            // Target not loaded yet — stash for drainage on append.
            m_pendingReactions[targetId].append(
                PendingReaction{key, sender, reactionEventId});
        }
        return;
    }

    // --- m.room.redaction ----------------------------------------------
    // Currently only the server plumbs redactions for reactions; message
    // redactions are handled elsewhere. We only care about reactions being
    // redacted: find the reaction in our index and remove it from its target.
    if (event.type == std::string(bsfchat::event_type::kRoomRedaction)) {
        const auto& data = event.content.data;
        QString target = QString::fromStdString(data.value("redacts", ""));
        if (target.isEmpty()) return;
        auto it = m_reactionIndex.find(target);
        if (it == m_reactionIndex.end()) return;
        ReactionRef ref = it.value();
        m_reactionIndex.erase(it);
        for (int i = 0; i < m_messages.size(); ++i) {
            if (m_messages[i].eventId != ref.targetEventId) continue;
            auto bIt = m_messages[i].reactionsByEmoji.find(ref.emoji);
            if (bIt == m_messages[i].reactionsByEmoji.end()) return;
            auto& bucket = bIt.value();
            for (int j = 0; j < bucket.size(); ++j) {
                if (bucket[j].second == target) {
                    bucket.removeAt(j);
                    break;
                }
            }
            if (bucket.isEmpty()) m_messages[i].reactionsByEmoji.erase(bIt);
            auto idx = index(i);
            emit dataChanged(idx, idx, {ReactionsRole});
            return;
        }
        return;
    }

    // Only add message events
    if (event.type != std::string(bsfchat::event_type::kRoomMessage))
        return;

    // Detect edit: m.relates_to.rel_type == "m.replace" + target event_id.
    // The edit's "body" has an asterisk prefix for clients that don't
    // understand edits; the real replacement lives under "m.new_content".
    const auto& data = event.content.data;
    bool isEdit = false;
    QString targetId;
    if (data.contains("m.relates_to") && data["m.relates_to"].is_object()) {
        const auto& rel = data["m.relates_to"];
        if (rel.value("rel_type", "") == "m.replace") {
            isEdit = true;
            targetId = QString::fromStdString(rel.value("event_id", ""));
        }
    }

    if (isEdit && !targetId.isEmpty()) {
        // Look up the target. If we don't have it yet (edit arrived before
        // the original via out-of-order sync), silently drop — a future
        // sync/backfill will bring the original, and we'll see this edit
        // again or via its own m_new_content chain. Keeping edits in the
        // timeline would double-render the message.
        for (int i = 0; i < m_messages.size(); ++i) {
            if (m_messages[i].eventId != targetId) continue;
            // Prefer m.new_content; fall back to stripping the "* " prefix.
            QString newBody;
            QString newFormatted;
            if (data.contains("m.new_content") && data["m.new_content"].is_object()) {
                const auto& nc = data["m.new_content"];
                newBody = QString::fromStdString(nc.value("body", ""));
                newFormatted = QString::fromStdString(nc.value("formatted_body", ""));
            } else {
                QString raw = QString::fromStdString(data.value("body", ""));
                if (raw.startsWith("* ")) raw = raw.mid(2);
                newBody = raw;
            }
            if (newFormatted.isEmpty() && !newBody.isEmpty()
                && m_messages[i].msgtype == "m.text") {
                newFormatted = MarkdownParser::toHtml(newBody);
            }
            m_messages[i].body = newBody;
            m_messages[i].formattedBody = newFormatted;
            m_messages[i].edited = true;
            m_messages[i].editedAt = event.origin_server_ts;
            auto idx = index(i);
            emit dataChanged(idx, idx, {BodyRole, FormattedBodyRole, EditedRole});
            return;
        }
        // Target not found — ignore silently.
        return;
    }

    // Regular new message — dedupe + append.
    QString eventId = QString::fromStdString(event.event_id);
    for (const auto& msg : m_messages) {
        if (msg.eventId == eventId) return;
    }

    beginInsertRows(QModelIndex(), m_messages.size(), m_messages.size());
    m_messages.append(eventToEntry(event, ownUserId));
    endInsertRows();
    emit countChanged();

    // Drain any reactions we received before this message landed.
    auto pIt = m_pendingReactions.find(eventId);
    if (pIt != m_pendingReactions.end()) {
        int row = m_messages.size() - 1;
        for (const auto& pr : pIt.value()) {
            if (m_reactionIndex.contains(pr.reactionEventId)) continue;
            auto& bucket = m_messages[row].reactionsByEmoji[pr.emoji];
            bucket.append(qMakePair(pr.userId, pr.reactionEventId));
            m_reactionIndex.insert(pr.reactionEventId,
                                   ReactionRef{eventId, pr.emoji, pr.userId});
        }
        m_pendingReactions.erase(pIt);
        auto idx = index(row);
        emit dataChanged(idx, idx, {ReactionsRole});
    }
}

void MessageModel::appendEvents(const QVector<bsfchat::RoomEvent>& events, const QString& ownUserId)
{
    for (const auto& event : events) {
        appendEvent(event, ownUserId);
    }
}

void MessageModel::prependEvents(const QVector<bsfchat::RoomEvent>& events, const QString& ownUserId)
{
    m_ownUserId = ownUserId;
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

void MessageModel::setPrevBatchToken(const QString& token)
{
    if (m_prevBatchToken == token) return;
    const bool hadMore = hasMoreHistory();
    m_prevBatchToken = token;
    if (hadMore != hasMoreHistory()) emit hasMoreHistoryChanged();
}

void MessageModel::setLoadingHistory(bool v)
{
    if (m_loadingHistory == v) return;
    m_loadingHistory = v;
    emit loadingHistoryChanged();
}

void MessageModel::clear()
{
    beginResetModel();
    m_messages.clear();
    m_pendingReactions.clear();
    m_reactionIndex.clear();
    endResetModel();
    // A room switch invalidates the pagination state too — otherwise a
    // stale token from the previous room would drive the next scroll-up.
    const bool hadMore = hasMoreHistory();
    m_prevBatchToken.clear();
    if (hadMore) emit hasMoreHistoryChanged();
    if (m_loadingHistory) { m_loadingHistory = false; emit loadingHistoryChanged(); }
    emit countChanged();
}

QString MessageModel::resolveDisplayName(const QString& userId) const
{
    // 1. Check the global cache (populated from m.room.member events).
    if (m_dnCache) {
        auto it = m_dnCache->find(userId);
        if (it != m_dnCache->end() && !it->isEmpty()) return *it;
    }
    // 2. Fallback: strip @localpart:host → localpart.
    if (userId.startsWith('@')) {
        int colon = userId.indexOf(':');
        if (colon > 1) return userId.mid(1, colon - 1);
    }
    return userId;
}

void MessageModel::refreshDisplayNames()
{
    bool any = false;
    for (int i = 0; i < m_messages.size(); ++i) {
        QString resolved = resolveDisplayName(m_messages[i].sender);
        if (resolved != m_messages[i].senderDisplayName) {
            m_messages[i].senderDisplayName = resolved;
            any = true;
        }
    }
    if (any && !m_messages.isEmpty()) {
        emit dataChanged(index(0), index(m_messages.size() - 1), {SenderDisplayNameRole});
    }
}
