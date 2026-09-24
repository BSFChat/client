#include "model/MessageModel.h"
#include "model/ThreadFilterModel.h"

#include <QDateTime>
#include <QSet>
#include <bsfchat/Constants.h>
#include "util/MarkdownParser.h"
#include "util/MediaUrl.h"
#include "util/MentionRenderer.h"
#include "util/ScrollAnchor.h"

namespace {

// Media rows render a filename card, not prose — no body HTML is built for
// them, and MessageBubble never shows their TextEdit.
bool isMediaMsgtype(const QString& msgtype)
{
    return msgtype == QLatin1String("m.image") || msgtype == QLatin1String("m.file")
        || msgtype == QLatin1String("m.audio") || msgtype == QLatin1String("m.video");
}

// The one place a body with no sender-supplied formatted_body becomes the HTML
// the timeline renders.
//
// Two promotions, and they used to disagree about newlines. m.text goes
// through markdown, which ends by turning "\n" into <br>. Everything else
// must NOT go through markdown — an m.notice from a bot, or an /me, has to
// keep its literal asterisks and underscores literal — so it is escaped only,
// and the escape-only path dropped the line breaks. HTML folds a newline into
// a space, so the moment anything promoted such a body (the "(edited)" badge,
// the m.emote prefix, a mention anchor) a three-line message rendered as one.
//
// Returning markup for every prose msgtype, not just m.text, is what lets the
// view concatenate and never escape: the rule about which bodies markdown may
// touch lives here, next to the rule about line breaks, instead of being
// half-remembered in QML.
QString renderBodyHtml(const QString& body, const QString& msgtype)
{
    if (body.isEmpty() || isMediaMsgtype(msgtype)) return {};
    return msgtype == QLatin1String("m.text") ? MarkdownParser::toHtml(body)
                                              : MarkdownParser::plainToHtml(body);
}

} // namespace

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
    case SenderIsBotRole: return msg.senderIsBot;
    case DeliveryStateRole: return static_cast<int>(msg.delivery);
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
    case MediaMxcRole: return msg.mediaMxc;
    case MediaFileNameRole: return msg.mediaFileName;
    case MediaFileSizeRole: return msg.mediaFileSize;
    case MediaWidthRole: return msg.mediaWidth;
    case MediaHeightRole: return msg.mediaHeight;
    case EditedRole: return msg.edited;
    case ReplyToEventIdRole: return msg.replyToEventId;
    case ReplyToSenderRole: return msg.replyToSender;
    case ReplyPreviewRole: return msg.replyPreview;
    case ReactionsRole: return buildReactionsList(msg);
    case ThreadRootIdRole: return msg.threadRootId;
    case ThreadReplyCountRole: return m_threadReplyCounts.value(msg.eventId, 0);
    case MentionsMeRole: return msg.mentionsMe;
    case MentionsRoomRole: return msg.mentionsRoom;
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
        {MediaMxcRole, "mediaMxc"},
        {MediaFileNameRole, "mediaFileName"},
        {MediaFileSizeRole, "mediaFileSize"},
        {MediaWidthRole, "mediaWidth"},
        {MediaHeightRole, "mediaHeight"},
        {EditedRole, "edited"},
        {ReplyToEventIdRole, "replyToEventId"},
        {ReplyToSenderRole, "replyToSender"},
        {ReplyPreviewRole, "replyPreview"},
        {ReactionsRole, "reactions"},
        {ThreadRootIdRole, "threadRootId"},
        {ThreadReplyCountRole, "threadReplyCount"},
        {MentionsMeRole, "mentionsMe"},
        {MentionsRoomRole, "mentionsRoom"},
        {SenderIsBotRole, "senderIsBot"},
        {DeliveryStateRole, "deliveryState"}
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

int MessageModel::rowForEventId(const QString& eventId) const
{
    if (eventId.isEmpty()) return -1;
    const int row = m_indexByEventId.value(eventId, -1);
    // Defensive: an index that has drifted out of range must not be handed
    // to index()/m_messages. Treating it as "not found" degrades to the
    // pre-index behaviour for that one lookup instead of crashing.
    if (row < 0 || row >= m_messages.size()) return -1;
    return row;
}

QStringList MessageModel::mentionSetFor(const QString& eventId) const
{
    // O(1) via m_indexByEventId — never a linear scan.
    const int row = rowForEventId(eventId);
    if (row < 0) return {};
    QStringList out = m_messages[row].mentionedUserIds;
    // Re-flatten the room-wide flag back to the sentinel the send paths speak.
    // Spelled literally rather than via MatrixClient::kRoomMentionSentinel:
    // this translation unit is also linked into test_models, which has no Qt
    // Network dependency, and MatrixClient.h would drag one in.
    const QString kRoomSentinel = QStringLiteral("@room");
    if (m_messages[row].mentionsRoom && !out.contains(kRoomSentinel)) {
        out.append(kRoomSentinel);
    }
    return out;
}

void MessageModel::rebuildIndices()
{
    m_indexByEventId.clear();
    m_threadReplyCounts.clear();
    m_indexByEventId.reserve(m_messages.size());
    for (int i = 0; i < m_messages.size(); ++i) {
        // An unreconciled local echo has no event id yet. Indexing it under
        // the empty string would make rowForEventId("") resolve to a real
        // row, and — worse — two pending echoes would collide on that key.
        if (!m_messages[i].eventId.isEmpty())
            m_indexByEventId.insert(m_messages[i].eventId, i);
        if (!m_messages[i].threadRootId.isEmpty())
            ++m_threadReplyCounts[m_messages[i].threadRootId];
    }
}

QString MessageModel::ownReactionEventId(const QString& targetEventId, const QString& emoji,
                                          const QString& userId) const
{
    const int row = rowForEventId(targetEventId);
    if (row < 0) return {};
    const auto& msg = m_messages[row];
    auto it = msg.reactionsByEmoji.find(emoji);
    if (it == msg.reactionsByEmoji.end()) return {};
    for (const auto& p : it.value()) {
        if (p.first == userId) return p.second;
    }
    return {};
}

int MessageModel::applyReactionToTarget(const QString& targetEventId, const QString& emoji,
                                         const QString& userId, const QString& reactionEventId)
{
    const int row = rowForEventId(targetEventId);
    if (row < 0) return -1;
    auto& bucket = m_messages[row].reactionsByEmoji[emoji];
    // Dedupe by reaction event id — sync may replay.
    for (const auto& p : bucket) {
        if (p.second == reactionEventId) return row;
    }
    bucket.append(qMakePair(userId, reactionEventId));
    m_reactionIndex.insert(reactionEventId,
                           ReactionRef{targetEventId, emoji, userId});
    return row;
}

int MessageModel::indexForEventId(const QString& eventId) const
{
    return rowForEventId(eventId);
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
        // The thread panel renders the same prose the timeline does, so it
        // needs the mention-rendered markup and the highlight flags too —
        // otherwise a reply that pings you looks inert in the drawer and
        // highlighted in the channel behind it.
        row[QStringLiteral("formattedBody")] = m.formattedBody;
        row[QStringLiteral("mentionsMe")] = m.mentionsMe;
        row[QStringLiteral("mentionsRoom")] = m.mentionsRoom;
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
    return m_threadReplyCounts.value(rootEventId, 0);
}

QVariantList MessageModel::editHistory(const QString& eventId) const
{
    QVariantList out;
    int idx = indexForEventId(eventId);
    if (idx < 0) return out;
    const auto& m = m_messages[idx];
    if (!m.edited || m.history.isEmpty()) return out;
    for (const auto& h : m.history) {
        QVariantMap row;
        row[QStringLiteral("body")] = h.first;
        row[QStringLiteral("timestamp")] = h.second;
        out.append(row);
    }
    // Current body as the last entry.
    QVariantMap cur;
    cur[QStringLiteral("body")] = m.body;
    cur[QStringLiteral("timestamp")] = m.editedAt;
    cur[QStringLiteral("isCurrent")] = true;
    out.append(cur);
    return out;
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

QString MessageModel::firstUnreadEventIdAfterTs(qint64 tsMs) const
{
    if (tsMs <= 0) return {};
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].timestamp <= tsMs) continue;
        // Skip, don't stop: a message from somebody else after your own is
        // still unread, and it is the one the divider belongs above.
        if (m_messages[i].isOwnMessage) continue;
        return m_messages[i].eventId;
    }
    return {};
}

qint64 MessageModel::newestServerTimestampMs() const
{
    // `serverTimestamp`, never `timestamp`: for a row this device authored
    // the latter is the local send time and is deliberately never rewritten
    // (it is the sort key and the time in the bubble). A row that has not
    // been named by the server yet carries 0 and is skipped — the newest row
    // the SERVER has timestamped is the newest thing it makes sense to claim
    // to have read. See the header.
    for (auto it = m_messages.crbegin(); it != m_messages.crend(); ++it) {
        if (it->serverTimestamp > 0) return it->serverTimestamp;
    }
    return 0;
}

int MessageModel::restoreIndexForDivider(const QString& dividerEventId) const
{
    // rowForEventId already answers -1 for an empty id and for an id this
    // model has never seen — an anchor carried over from another server's
    // timeline is exactly the latter.
    const int idx = rowForEventId(dividerEventId);
    const auto target = bsfchat::client::chooseRestoreTarget(
        static_cast<int>(m_messages.size()), idx);
    return target.kind == bsfchat::client::RestoreTarget::Kind::Row
        ? target.index : -1;
}

bool MessageModel::isPinnedToEnd(qreal contentHeight, qreal contentY,
                                 qreal viewportHeight, qreal tolerance) const
{
    return bsfchat::client::isPinnedToEnd(contentHeight, contentY,
                                          viewportHeight, tolerance);
}

int MessageModel::scrollPolicy(bool contextChanged, bool paginating,
                               bool pinnedToEnd, bool followLatch) const
{
    return static_cast<int>(
        bsfchat::client::positionPolicyForModelChange(contextChanged, paginating,
                                                      pinnedToEnd, followLatch));
}

bool MessageModel::followEndAfterContentYSample(bool followEnd, bool userDriven,
                                                bool liveAtEnd) const
{
    return bsfchat::client::followEndAfterContentYSample(followEnd, userDriven,
                                                         liveAtEnd);
}

int MessageModel::geometryChangePolicy(bool followEnd) const
{
    return static_cast<int>(
        bsfchat::client::positionPolicyForGeometryChange(followEnd));
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

void MessageModel::setMediaTicketCache(bsfchat::client::MediaTicketCache* cache)
{
    if (m_mediaTickets == cache) return;
    if (m_mediaTickets) m_mediaTickets->disconnect(this);
    m_mediaTickets = cache;
    if (!m_mediaTickets) return;
    connect(m_mediaTickets, &bsfchat::client::MediaTicketCache::ticketReady,
            this, &MessageModel::onMediaTicketReady);
}

QString MessageModel::resolveMediaUrl(const QString& mxcUri) const
{
    // "" is a normal answer, not a failure: it means "no ticket yet". The row
    // is repainted by onMediaTicketReady when one arrives, and MessageBubble
    // already handles mediaUrl changing after the bubble exists (it had to —
    // this returned "" before login too).
    if (!m_mediaTickets) return {};
    return m_mediaTickets->urlFor(mxcUri);
}

void MessageModel::onMediaTicketReady(const QString& mxcUri, const QString& url)
{
    for (int row = 0; row < m_messages.size(); ++row) {
        if (m_messages[row].mediaMxc != mxcUri) continue;
        if (m_messages[row].mediaUrl == url) continue;
        m_messages[row].mediaUrl = url;
        const auto idx = index(row, 0);
        emit dataChanged(idx, idx, {MediaUrlRole});
    }
}

int MessageModel::indexOfLocalEcho(const QString& localId) const
{
    if (localId.isEmpty()) return -1;
    for (int i = m_messages.size() - 1; i >= 0; --i) {
        if (m_messages[i].localId == localId) return i;
    }
    return -1;
}

int MessageModel::indexOfEchoMatching(const bsfchat::RoomEvent& event) const
{
    // The overwhelmingly common case: nothing of ours is in flight, so this
    // costs one integer test per inbound event rather than a walk.
    if (m_unreconciledEchoes <= 0) return -1;
    const QString sender = QString::fromStdString(event.sender);
    const QString body = QString::fromStdString(event.content.data.value("body", ""));
    // Newest first: if the user sent the same text twice in a row, the
    // server's copies arrive in send order, so the OLDEST unmatched echo is
    // the right target... except that scanning oldest-first would match an
    // echo we have already confirmed. Confirmed echoes carry an eventId and
    // are skipped, so oldest-first among the UNCONFIRMED ones is correct.
    for (int i = 0; i < m_messages.size(); ++i) {
        const auto& m = m_messages[i];
        if (m.localId.isEmpty()) continue;        // not an echo
        if (!m.eventId.isEmpty()) continue;       // already reconciled
        if (m.delivery == DeliveryFailed) continue; // a failed send is not this
        if (m.sender != sender) continue;
        if (m.body != body) continue;
        return i;
    }
    return -1;
}

void MessageModel::adoptEventId(int row, const QString& eventId, qint64 serverTsMs)
{
    if (row < 0 || row >= m_messages.size()) return;
    if (!m_messages[row].localId.isEmpty() && m_messages[row].eventId.isEmpty()
        && m_unreconciledEchoes > 0) {
        --m_unreconciledEchoes;
    }
    m_messages[row].eventId = eventId;
    m_messages[row].delivery = DeliveryConfirmed;
    // `timestamp` is deliberately NOT touched: it is the sort key and the
    // time in the bubble, and re-sorting a row under the user's cursor at
    // the moment it is confirmed would be worse than a few ms of skew. The
    // server's own clock lands beside it instead, for the read marker.
    if (serverTsMs > 0) m_messages[row].serverTimestamp = serverTsMs;
    if (!eventId.isEmpty()) m_indexByEventId.insert(eventId, row);

    // An edit or a reaction can reach us for an event whose row existed the
    // whole time under no id at all. The normal append path drains both the
    // moment it inserts; an adopted echo has to get the same treatment, or
    // an edit-then-confirm ordering leaves the pre-edit text on screen.
    drainPendingEdit(m_messages[row]);
    auto pIt = m_pendingReactions.find(eventId);
    if (pIt != m_pendingReactions.end()) {
        for (const auto& pr : pIt.value()) {
            if (m_reactionIndex.contains(pr.reactionEventId)) continue;
            auto& bucket = m_messages[row].reactionsByEmoji[pr.emoji];
            bucket.append(qMakePair(pr.userId, pr.reactionEventId));
            m_reactionIndex.insert(pr.reactionEventId,
                                   ReactionRef{eventId, pr.emoji, pr.userId});
        }
        m_pendingReactions.erase(pIt);
    }

    auto idx = index(row);
    emit dataChanged(idx, idx, {EventIdRole, DeliveryStateRole, BodyRole,
                                FormattedBodyRole, EditedRole, ReactionsRole});
}

void MessageModel::appendLocalEcho(const QString& localId, const QString& body,
                                   const QString& formattedBody, const QString& ownUserId,
                                   const QString& replyToEventId,
                                   const QString& threadRootId)
{
    if (localId.isEmpty()) return;
    if (indexOfLocalEcho(localId) >= 0) return; // already echoed

    MessageEntry entry;
    entry.localId = localId;
    entry.delivery = DeliverySending;
    // No eventId: the server has not named it yet. Everything that keys off
    // event ids (the dedupe index, reactions, edits, redaction) therefore
    // skips this row until confirmLocalEcho hands it one.
    entry.sender = ownUserId;
    entry.senderDisplayName = resolveDisplayName(ownUserId);
    entry.senderIsBot = resolveIsBot(ownUserId);
    entry.isOwnMessage = true;
    // Our own clock. It is only used for ordering against rows that are all
    // older, and for the timestamp in the bubble; the server's
    // origin_server_ts replaces nothing, because the row is not rebuilt on
    // confirmation — re-sorting a message under the user's cursor at the
    // moment it is confirmed would be worse than a few ms of skew.
    entry.timestamp = QDateTime::currentMSecsSinceEpoch();
    entry.msgtype = QStringLiteral("m.text");
    entry.body = body;
    entry.formattedBody = formattedBody;
    entry.replyToEventId = replyToEventId;
    entry.threadRootId = threadRootId;

    beginInsertRows(QModelIndex(), m_messages.size(), m_messages.size());
    m_messages.append(std::move(entry));
    ++m_unreconciledEchoes;
    if (!threadRootId.isEmpty()) ++m_threadReplyCounts[threadRootId];
    endInsertRows();
    emit countChanged();
}

void MessageModel::confirmLocalEcho(const QString& localId, const QString& eventId)
{
    const int row = indexOfLocalEcho(localId);
    if (row < 0) return;
    // The event already came down /sync and was matched by body, which
    // adopted the id there. Nothing left to do, and re-adopting would
    // re-register the same index entry.
    if (!m_messages[row].eventId.isEmpty()) return;
    adoptEventId(row, eventId);
}

void MessageModel::failLocalEcho(const QString& localId)
{
    const int row = indexOfLocalEcho(localId);
    if (row < 0) return;
    if (!m_messages[row].eventId.isEmpty()) return; // it landed after all
    m_messages[row].delivery = DeliveryFailed;
    auto idx = index(row);
    emit dataChanged(idx, idx, {DeliveryStateRole});
}

void MessageModel::discardLocalEcho(const QString& localId)
{
    const int row = indexOfLocalEcho(localId);
    if (row < 0) return;
    const QString threadRoot = m_messages[row].threadRootId;
    if (m_messages[row].eventId.isEmpty() && m_unreconciledEchoes > 0) {
        --m_unreconciledEchoes;
    }
    beginRemoveRows(QModelIndex(), row, row);
    m_messages.remove(row);
    rebuildIndices();
    endRemoveRows();
    emit countChanged();
    Q_UNUSED(threadRoot) // rebuildIndices recomputes m_threadReplyCounts
}

QString MessageModel::localEchoBody(const QString& localId) const
{
    const int row = indexOfLocalEcho(localId);
    return row < 0 ? QString() : m_messages[row].body;
}

MessageModel::MessageEntry MessageModel::eventToEntry(const bsfchat::RoomEvent& event, const QString& ownUserId) const
{
    MessageEntry entry;
    entry.eventId = QString::fromStdString(event.event_id);
    entry.sender = QString::fromStdString(event.sender);
    entry.senderDisplayName = resolveDisplayName(entry.sender);
    entry.senderIsBot = resolveIsBot(entry.sender);
    entry.timestamp = event.origin_server_ts;
    entry.serverTimestamp = static_cast<qint64>(event.origin_server_ts);
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
        // Resolve the target from the already-ingested timeline.
        const int target = rowForEventId(entry.replyToEventId);
        if (target >= 0) {
            entry.replyToSender = m_messages[target].senderDisplayName;
            QString preview = m_messages[target].body;
            if (preview.size() > 80) preview = preview.left(80) + "…";
            entry.replyPreview = preview;
        }
    }

    // Extract media fields for image/file messages
    if (entry.msgtype == "m.image" || entry.msgtype == "m.file" ||
        entry.msgtype == "m.audio" || entry.msgtype == "m.video") {
        QString mxcUrl = QString::fromStdString(event.content.data.value("url", ""));
        // Kept alongside the resolved URL: a ticket is short-lived, so the row
        // has to know which object it is showing in order to be re-resolved when
        // one arrives (or is refreshed). Also what the external-open path uses,
        // so that path never touches a URL with a credential in it.
        entry.mediaMxc = mxcUrl;
        entry.mediaUrl = resolveMediaUrl(mxcUrl);
        entry.mediaFileName = entry.body; // body is the filename in media messages

        if (event.content.data.contains("info") && event.content.data["info"].is_object()) {
            const auto& info = event.content.data["info"];
            entry.mediaFileSize = info.value("size", 0);
            // Pull intrinsic dimensions from the upload metadata so
            // MessageBubble can reserve the right box before the
            // image / video data finishes downloading. Without this
            // the row's height reflows once Image::sourceSize
            // resolves, which forces the scroll-pin logic to
            // chase a moving bottom while async media loads in.
            entry.mediaWidth  = info.value("w", 0);
            entry.mediaHeight = info.value("h", 0);
        }
    }

    // --- m.mentions (MSC3952) ------------------------------------------
    // The authoritative mention set: who the author says they pinged, and
    // whether it was an @room broadcast. We trust it for *routing* only —
    // the notification decision and the highlight — never for markup.
    //
    // Sourced through notifiedMentions() rather than straight off the content,
    // so an edited message highlights the people its ORIGINAL send notified and
    // not the ones an edit added afterwards — see that function's header.
    {
        const auto& mentions = notifiedMentions(event);
        if (mentions.contains("user_ids") && mentions["user_ids"].is_array()) {
            for (const auto& u : mentions["user_ids"]) {
                if (!u.is_string()) continue;
                const QString uid = QString::fromStdString(u.get<std::string>());
                if (uid.isEmpty() || entry.mentionedUserIds.contains(uid)) continue;
                entry.mentionedUserIds.append(uid);
                if (!ownUserId.isEmpty() && uid == ownUserId) entry.mentionsMe = true;
            }
        }
        if (mentions.contains("room") && mentions["room"].is_boolean())
            entry.mentionsRoom = mentions["room"].get<bool>();

        // Role mentions (bsfchat.role_ids, inside the same block — see
        // protocol Constants.h for why it lives there and not beside it).
        // Read through the SAME notifiedMentions() object as everything above,
        // so the "an edit's mentions notified nobody" rule covers roles for
        // free rather than needing a second copy of it.
        const std::string kRoleKey(bsfchat::mention::kRoleIdsKey);
        if (m_roleResolver && mentions.contains(kRoleKey)
            && mentions[kRoleKey].is_array()) {
            QStringList roleIds;
            for (const auto& r : mentions[kRoleKey]) {
                if (!r.is_string()) continue;
                const QString id = QString::fromStdString(r.get<std::string>());
                if (!id.isEmpty() && !roleIds.contains(id)) roleIds.append(id);
            }
            if (!roleIds.isEmpty()) {
                entry.roleMentions = m_roleResolver(
                    roleIds, entry.sender, QString::fromStdString(event.room_id));
                for (const auto& role : entry.roleMentions) {
                    if (role.includesMe) { entry.mentionsMe = true; break; }
                }
            }
        }
    }

    // --- server-reconciled edits ---------------------------------------
    // The server folds edits into the original event before handing it to us:
    // `content` is already the latest text, `unsigned.m.relations.m.replace`
    // identifies the edit that produced it, and
    // `unsigned.bsfchat.original_content` carries the pre-edit content.
    //
    // Reading the bundle is what makes "Show edit history" correct on a fresh
    // load. Before this, the original arrived looking un-edited, and the
    // replacement — which is *also* an ordinary timeline event — then pushed
    // the current body into `history` and re-set it as the current body, so
    // the history popover showed the same text twice and never the original.
    if (event.unsigned_data.has_value()) {
        const auto& unsignedData = event.unsigned_data->data;
        QString replaceEventId;
        qint64 replaceTs = 0;
        if (unsignedData.contains("m.relations")
            && unsignedData["m.relations"].is_object()) {
            const auto& relations = unsignedData["m.relations"];
            if (relations.contains("m.replace")
                && relations["m.replace"].is_object()) {
                const auto& replace = relations["m.replace"];
                replaceEventId = QString::fromStdString(
                    replace.value("event_id", ""));
                if (replace.contains("origin_server_ts")
                    && replace["origin_server_ts"].is_number()) {
                    replaceTs = replace["origin_server_ts"].get<qint64>();
                }
            }
        }
        if (!replaceEventId.isEmpty()) {
            entry.edited = true;
            // Fall back to the original's own ts rather than leaving 0, which
            // the history popover would render as the epoch.
            entry.editedAt = replaceTs > 0 ? replaceTs : entry.timestamp;
            // Marking the edit as already-applied is what suppresses the
            // duplicate when its sibling event reaches appendEvent().
            entry.appliedEdits.insert(replaceEventId);
            if (unsignedData.contains("bsfchat.original_content")
                && unsignedData["bsfchat.original_content"].is_object()) {
                const auto& original = unsignedData["bsfchat.original_content"];
                const QString originalBody = QString::fromStdString(
                    original.value("body", ""));
                if (!originalBody.isEmpty())
                    entry.history.append({originalBody, entry.timestamp});
            }
        }
    }

    // If no formatted_body from server, render the body ourselves — markdown
    // for m.text, escape-only for the rest. Either way the result is the
    // display markup the bubble shows verbatim.
    if (entry.formattedBody.isEmpty()) {
        entry.formattedBody = renderBodyHtml(entry.body, entry.msgtype);
    }
    applyMentionMarkup(entry);

    return entry;
}

const nlohmann::json& MessageModel::notifiedMentions(const bsfchat::RoomEvent& event)
{
    static const nlohmann::json kNone = nlohmann::json::object();

    auto mentionsOf = [](const nlohmann::json& content) -> const nlohmann::json& {
        if (content.is_object() && content.contains("m.mentions")
            && content["m.mentions"].is_object()) {
            return content["m.mentions"];
        }
        return kNone;
    };

    // Only a SERVER-RECONCILED edit redirects the lookup. Both halves of the
    // bundle are required: m.relations.m.replace is what says "this content is
    // post-edit", and original_content is the pre-edit content it displaced.
    // Without the relation check a client (or a future server) writing
    // original_content for some other reason would silently reroute mentions;
    // without original_content there is nothing to reroute TO, and falling back
    // to the event's own content is right — an unedited message's mentions are
    // its own.
    if (event.unsigned_data.has_value()) {
        const auto& u = event.unsigned_data->data;
        const bool edited = u.is_object() && u.contains("m.relations")
            && u["m.relations"].is_object()
            && u["m.relations"].contains("m.replace")
            && u["m.relations"]["m.replace"].is_object();
        if (edited && u.contains("bsfchat.original_content")
            && u["bsfchat.original_content"].is_object()) {
            // Note this returns the ORIGINAL's block even when it is absent
            // entirely — an original that mentioned nobody notified nobody, so
            // an edit that adds "@alice" must render no pill for her.
            return mentionsOf(u["bsfchat.original_content"]);
        }
    }
    return mentionsOf(event.content.data);
}

bool MessageModel::isReplacementEvent(const bsfchat::RoomEvent& event)
{
    const auto& data = event.content.data;
    if (!data.contains("m.relates_to") || !data["m.relates_to"].is_object())
        return false;
    return data["m.relates_to"].value("rel_type", "") == "m.replace";
}

bool MessageModel::rendersAsRow(const bsfchat::RoomEvent& event)
{
    return event.type == std::string(bsfchat::event_type::kRoomMessage)
        && !isReplacementEvent(event);
}

MessageModel::PendingEdit MessageModel::editPayload(const bsfchat::RoomEvent& event)
{
    PendingEdit edit;
    edit.editEventId = QString::fromStdString(event.event_id);
    edit.timestamp = event.origin_server_ts;
    // Prefer m.new_content; fall back to stripping the "* " prefix.
    const auto& data = event.content.data;
    if (data.contains("m.new_content") && data["m.new_content"].is_object()) {
        const auto& nc = data["m.new_content"];
        edit.body = QString::fromStdString(nc.value("body", ""));
        edit.formattedBody = QString::fromStdString(nc.value("formatted_body", ""));
    } else {
        QString raw = QString::fromStdString(data.value("body", ""));
        if (raw.startsWith("* ")) raw = raw.mid(2);
        edit.body = raw;
    }
    return edit;
}

bool MessageModel::applyEditToEntry(MessageEntry& m, const PendingEdit& edit) const
{
    // Already reflected in the body? Then this edit carries no news. Two ways
    // that happens:
    //   * The server reconciled the edit before sending us the original,
    //     and recorded this event id in unsigned.m.relations.m.replace.
    //     Applying it again would seed `history` with the CURRENT text and
    //     "Show edit history" would list the same body twice.
    //   * Sync replayed the same replacement event.
    // A *later* edit has a different event id and still lands below, which
    // is what keeps live editing working while the user watches the room.
    if (!edit.editEventId.isEmpty() && m.appliedEdits.contains(edit.editEventId))
        return false;

    // AN OLDER EDIT NEVER OVERWRITES A NEWER ONE.
    //
    // The server hands the original over already carrying the WINNING edit's
    // content, and names only that one edit in the bundle. Every earlier edit
    // is still an ordinary timeline event, and when a window holding the
    // original and its edits was replayed through appendEvent — oldest first
    // — edit 1 of N was not in appliedEdits and overwrote the reconciled
    // body, edits 2..N-1 followed, and edit N was skipped as "already
    // applied". A message edited N times rendered edit N-1. Rare for a human;
    // for the #notifications board, edited every ~2 minutes, it is every
    // single load of a window that holds the original.
    //
    // Compared by server timestamp: origin_server_ts is the server's clock
    // for both, and the bundle carries the winner's. Ties apply, as they
    // always did — a same-millisecond pair has no better tiebreak available
    // here than arrival order.
    //
    // The superseded text is not thrown away: it slots into `history` at its
    // own time, so "Show edit history" lists the versions in between rather
    // than jumping from the original straight to the current body.
    if (m.edited && edit.timestamp < m.editedAt) {
        if (!edit.body.isEmpty()) {
            int pos = 0;
            while (pos < m.history.size() && m.history[pos].second <= edit.timestamp)
                ++pos;
            m.history.insert(pos, {edit.body, edit.timestamp});
        }
        if (!edit.editEventId.isEmpty()) m.appliedEdits.insert(edit.editEventId);
        return false;
    }

    // Same promotion as a freshly-arrived event: an edit must not be the
    // thing that changes how a body is rendered. It was — the badge
    // promoted the row to rich text while the body stayed plain, and the
    // edit ate the message's line breaks.
    const QString newFormatted = edit.formattedBody.isEmpty()
        ? renderBodyHtml(edit.body, m.msgtype) : edit.formattedBody;
    // Stash the previous body into history so "Show edit history" can
    // recover it. We push EITHER the pristine original (before any edit) or
    // the last edit — so the user sees every distinct version.
    const qint64 prevTs = m.edited ? m.editedAt : m.timestamp;
    m.history.append({m.body, prevTs});
    m.body = edit.body;
    m.formattedBody = newFormatted;
    m.edited = true;
    m.editedAt = edit.timestamp;
    if (!edit.editEventId.isEmpty()) m.appliedEdits.insert(edit.editEventId);
    // The edit re-wrote the body, which threw away the mention anchors the
    // previous rendering had baked in. Re-apply from the entry's recorded
    // mention set so an edited message keeps its highlights.
    //
    // From the ENTRY's set — the original's — never from the replacement
    // event's own m.mentions, which is why editPayload does not read it. The
    // server records no mention row and fires no push for an m.replace, so
    // a mention an edit introduces notified nobody and must not render as
    // though it had. notifiedMentions() is the same rule applied to the
    // reconciled event a later reload sees; the two paths have to agree or
    // a highlight would appear on relaunch that was not there live.
    applyMentionMarkup(m);
    return true;
}

void MessageModel::stashPendingEdit(const QString& targetId, const PendingEdit& edit)
{
    auto it = m_pendingEdits.find(targetId);
    if (it != m_pendingEdits.end()) {
        // Newest wins; an equal timestamp is a later arrival, same as a tie
        // in applyEditToEntry.
        if (edit.timestamp >= it->timestamp) *it = edit;
        return;
    }
    if (m_pendingEdits.size() >= kMaxPendingEditTargets) {
        // Evict the stalest. A linear scan, but only on overflow, and over a
        // table that small by construction.
        auto oldest = m_pendingEdits.begin();
        for (auto p = m_pendingEdits.begin(); p != m_pendingEdits.end(); ++p)
            if (p->timestamp < oldest->timestamp) oldest = p;
        m_pendingEdits.erase(oldest);
    }
    m_pendingEdits.insert(targetId, edit);
}

void MessageModel::drainPendingEdit(MessageEntry& entry)
{
    auto it = m_pendingEdits.find(entry.eventId);
    if (it == m_pendingEdits.end()) return;
    // applyEditToEntry decides: a reconciled original that already carries
    // this edit (or a newer one) is left alone.
    applyEditToEntry(entry, it.value());
    m_pendingEdits.erase(it);
}

void MessageModel::applyMentionMarkup(MessageEntry& entry) const
{
    if (entry.mentionedUserIds.isEmpty() && entry.roleMentions.isEmpty()
        && !entry.mentionsRoom) return;
    // Media rows render a filename, not prose; there is nothing to highlight
    // and formattedBody is not shown for them.
    if (isMediaMsgtype(entry.msgtype)) return;

    QVector<bsfchat::client::MentionTarget> targets;
    targets.reserve(entry.mentionedUserIds.size());
    for (const QString& uid : entry.mentionedUserIds) {
        targets.append({uid, resolveDisplayName(uid),
                        !m_ownUserId.isEmpty() && uid == m_ownUserId});
    }

    // renderMentions requires already-escaped markup. When the sender supplied
    // no formatted_body and markdown rendering didn't kick in (e.g. m.notice),
    // promote the plain body ourselves rather than handing it raw text — the
    // renderer would otherwise match tokens against unescaped input and the
    // result would be interpreted as RichText. plainToHtml, not
    // toHtmlEscaped: this IS that promotion to rich text, so the body's line
    // breaks have to survive it.
    QString base = entry.formattedBody.isEmpty()
        ? MarkdownParser::plainToHtml(entry.body) : entry.formattedBody;
    entry.formattedBody = bsfchat::client::renderMentions(
        base, targets, entry.mentionsRoom, entry.roleMentions);
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
    // A redaction targets either a reaction (fold it out of the target
    // message's aggregate) or a message (take the row out of the timeline).
    // The message half used to say "handled elsewhere" and elsewhere did not
    // exist, so deleted messages never left the view until the room was
    // re-entered (U-H5).
    if (event.type == std::string(bsfchat::event_type::kRoomRedaction)) {
        const auto& data = event.content.data;
        QString target = QString::fromStdString(data.value("redacts", ""));
        if (target.isEmpty()) return;
        auto it = m_reactionIndex.find(target);
        if (it == m_reactionIndex.end()) {
            // Not a reaction we know about — try it as a message. Unknown
            // ids (never loaded, or already removed optimistically by
            // redactEvent) fall through harmlessly.
            removeMessage(target);
            return;
        }
        ReactionRef ref = it.value();
        m_reactionIndex.erase(it);
        const int row = rowForEventId(ref.targetEventId);
        if (row < 0) return;
        auto bIt = m_messages[row].reactionsByEmoji.find(ref.emoji);
        if (bIt == m_messages[row].reactionsByEmoji.end()) return;
        auto& bucket = bIt.value();
        for (int j = 0; j < bucket.size(); ++j) {
            if (bucket[j].second == target) {
                bucket.removeAt(j);
                break;
            }
        }
        if (bucket.isEmpty()) m_messages[row].reactionsByEmoji.erase(bIt);
        auto idx = index(row);
        emit dataChanged(idx, idx, {ReactionsRole});
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

    if (isEdit) {
        // An m.replace with no target is malformed. It is still an edit
        // sibling, and prependEvents has always dropped it (isReplacement-
        // Event), so it must not become a "* text" row here either — the two
        // paths have to agree on what a row is, because rendersAsRow() counts
        // for both of them.
        if (targetId.isEmpty()) return;

        const PendingEdit edit = editPayload(event);
        const int i = rowForEventId(targetId);
        if (i < 0) {
            // Target not loaded. It used to be dropped here on the promise
            // that "a future sync/backfill will bring the original" with the
            // edit already in it — which only holds if nothing is edited
            // between the server answering and us absorbing. Keep it; the
            // row picks it up when it arrives (drainPendingEdit).
            stashPendingEdit(targetId, edit);
            return;
        }
        if (applyEditToEntry(m_messages[i], edit)) {
            auto idx = index(i);
            emit dataChanged(idx, idx, {BodyRole, FormattedBodyRole, EditedRole});
        }
        return;
    }

    // Regular new message — dedupe + append. The index is authoritative for
    // the dedupe: sync replays the same event id often enough that this was
    // a full scan per inbound message.
    QString eventId = QString::fromStdString(event.event_id);
    if (const auto known = m_indexByEventId.constFind(eventId);
        known != m_indexByEventId.constEnd()) {
        // Already on screen — but if it got here as a local echo that the PUT
        // reply named (confirmLocalEcho has no timestamp to give), this replay
        // is the ONLY time the server's origin_server_ts for the user's own
        // message passes through the client. Take it before dropping the copy,
        // or the read marker can never advance past a message you sent
        // yourself: `timestamp` on that row is the moment we hit send, which
        // is always earlier than the ts the server stamped on receipt, so the
        // channel's own dot stays lit on your own words.
        const int row = known.value();
        if (row >= 0 && row < m_messages.size()
            && m_messages[row].serverTimestamp <= 0) {
            m_messages[row].serverTimestamp =
                static_cast<qint64>(event.origin_server_ts);
        }
        return;
    }

    // Our own local echo, coming back from the server. Normally the PUT
    // reply has already adopted this id (confirmLocalEcho) and the dedupe
    // above catches it — but /sync can beat the PUT reply, and then the
    // echo is still sitting here with no event id at all. Adopt it into the
    // existing row rather than appending a second copy of the user's own
    // message next to the one they are already looking at.
    if (const int echo = indexOfEchoMatching(event); echo >= 0) {
        adoptEventId(echo, eventId, static_cast<qint64>(event.origin_server_ts));
        return;
    }

    MessageEntry entry = eventToEntry(event, ownUserId);
    drainPendingEdit(entry);
    beginInsertRows(QModelIndex(), m_messages.size(), m_messages.size());
    m_messages.append(std::move(entry));
    m_indexByEventId.insert(eventId, m_messages.size() - 1);
    const QString threadRoot = m_messages.last().threadRootId;
    if (!threadRoot.isEmpty()) ++m_threadReplyCounts[threadRoot];
    endInsertRows();
    emit countChanged();

    // A thread reply changes its root's reply badge. Previously that only
    // refreshed when some unrelated change happened to repaint the root row.
    if (!threadRoot.isEmpty()) {
        const int rootRow = rowForEventId(threadRoot);
        if (rootRow >= 0) {
            auto rootIdx = index(rootRow);
            emit dataChanged(rootIdx, rootIdx, {ThreadReplyCountRole});
        }
    }

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
    QSet<QString> queued;
    for (const auto& event : events) {
        // rendersAsRow drops edit siblings: they are not messages. /messages
        // returns them alongside the originals, and this path never applied
        // them — so every edit in the fetched page showed up as its own junk
        // row rendering the "* new text" fallback body, directly above the
        // message it had already been folded into. There is nothing to apply
        // either: the original arrives from the same page (or an older one)
        // already reconciled, carrying the bundle eventToEntry reads.
        if (!rendersAsRow(event)) continue;
        QString eventId = QString::fromStdString(event.event_id);
        // Against both the loaded rows and the batch itself: overlapping
        // /messages pages can repeat an id inside a single call, which the
        // old m_messages-only scan let through.
        if (m_indexByEventId.contains(eventId) || queued.contains(eventId)) continue;
        queued.insert(eventId);
        MessageEntry entry = eventToEntry(event, ownUserId);
        // An edit that arrived over /sync while this page was in flight — the
        // server reconciled the original as of ITS answer, not ours.
        drainPendingEdit(entry);
        newEntries.append(std::move(entry));
    }

    if (newEntries.isEmpty()) return;

    beginInsertRows(QModelIndex(), 0, newEntries.size() - 1);
    for (int i = newEntries.size() - 1; i >= 0; --i) {
        m_messages.prepend(newEntries[i]);
    }
    // Every existing row index shifted by newEntries.size(), so patching the
    // index incrementally would cost the same as rebuilding it.
    rebuildIndices();
    endInsertRows();
    emit countChanged();

    // The message that used to be row 0 is no longer row 0 (U-M7). Both
    // ShowSenderRole and ShowDateSeparator special-case row 0 as "always
    // true" and otherwise compare against the previous row, so without this
    // the first message of the old page keeps a sender header and a date
    // separator it no longer earns — a duplicate header at every page
    // boundary. The view has no reason to re-query a row that only shifted.
    const int formerFirst = newEntries.size();
    if (formerFirst < m_messages.size()) {
        auto idx = index(formerFirst);
        emit dataChanged(idx, idx, {ShowSenderRole, ShowDateSeparator});
    }
}

bool MessageModel::removeMessage(const QString& eventId)
{
    const int row = rowForEventId(eventId);
    if (row < 0) return false;

    const QString threadRoot = m_messages[row].threadRootId;

    beginRemoveRows(QModelIndex(), row, row);
    // Reactions on the doomed message: their index entries would otherwise
    // survive as pointers to an event id that no longer has a row, and the
    // next reaction redaction against one of them would resolve rowForEventId
    // to -1 at best, or a re-used id at worst.
    for (auto bIt = m_messages[row].reactionsByEmoji.cbegin();
         bIt != m_messages[row].reactionsByEmoji.cend(); ++bIt) {
        for (const auto& pair : bIt.value()) m_reactionIndex.remove(pair.second);
    }
    m_messages.remove(row);
    m_pendingReactions.remove(eventId);
    // Every row after `row` shifted down by one, and m_threadReplyCounts has
    // to lose this message's contribution — rebuildIndices does both.
    rebuildIndices();
    endRemoveRows();
    emit countChanged();

    // The root's reply badge shrank.
    if (!threadRoot.isEmpty()) {
        const int rootRow = rowForEventId(threadRoot);
        if (rootRow >= 0) {
            auto rootIdx = index(rootRow);
            emit dataChanged(rootIdx, rootIdx, {ThreadReplyCountRole});
        }
    }

    // The row that slid into this slot has a new predecessor, so its sender
    // header and date separator may have flipped — same reason as the
    // prepend case above.
    if (row < m_messages.size()) {
        auto idx = index(row);
        emit dataChanged(idx, idx, {ShowSenderRole, ShowDateSeparator});
    }
    return true;
}

QAbstractItemModel* MessageModel::threadModel(const QString& rootEventId)
{
    if (rootEventId.isEmpty()) return nullptr;
    if (!m_threadProxy) {
        m_threadProxy = new ThreadFilterModel(this);
        m_threadProxy->setSourceModel(this);
    }
    m_threadProxy->setRootEventId(rootEventId);
    return m_threadProxy;
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

std::optional<MessageModel::HistoryRequest>
MessageModel::beginHistoryFill(bsfchat::client::HistoryFillKind kind, int firstPageLimit)
{
    using bsfchat::client::HistoryFillKind;
    if (!m_historyFill.canStart(kind)) return std::nullopt;
    // Anything but an open continues from the oldest loaded point, and at the
    // start of the room there is no such point: this refusal is what stops a
    // short room's non-scrollable view from asking again after every settle.
    if (kind != HistoryFillKind::Open && m_prevBatchToken.isEmpty()) return std::nullopt;
    m_historyFill.start(kind, static_cast<int>(m_messages.size()), firstPageLimit);
    m_historyFillPages.clear();
    m_historyFillRowIds.clear();
    m_historyFillFrom = kind == HistoryFillKind::Open ? QString() : m_prevBatchToken;
    setLoadingHistory(true);
    return HistoryRequest{m_historyFillFrom, m_historyFill.nextPageLimit()};
}

MessageModel::HistoryPageResult
MessageModel::absorbHistoryPage(const QString& requestedFrom,
                                const QVector<bsfchat::RoomEvent>& chronological,
                                const QString& endToken,
                                const QString& ownUserId)
{
    HistoryPageResult result;
    // Only the page the running fill is waiting for. A room re-opened before
    // its first answer landed issues a second from="" request; whichever
    // answer arrives second no longer matches and is dropped instead of being
    // counted as a further page.
    if (!m_historyFill.active() || requestedFrom != m_historyFillFrom) return result;

    // Count what this page would ADD to the timeline: rows, not events —
    // which is the entire fix. Dedupe against the loaded rows and against
    // earlier pages of this fill, exactly as prependEvents will.
    int newRows = 0;
    for (const auto& event : chronological) {
        if (!rendersAsRow(event)) continue;
        const QString id = QString::fromStdString(event.event_id);
        if (m_indexByEventId.contains(id) || m_historyFillRowIds.contains(id)) continue;
        m_historyFillRowIds.insert(id);
        ++newRows;
    }
    // An OPEN fill puts each page in as it lands; every other kind buffers
    // until the fill ends.
    //
    // An open is on the critical path of a tap and is allowed several serial
    // requests (util/HistoryFill.h), and buffering meant the timeline showed
    // NOTHING for all of them — the spinner is bound 1:1 to loadingHistory,
    // so a three-page open was three round trips of blank screen followed by
    // everything at once. Flushing makes time-to-first-row one round trip
    // whatever the room's density.
    //
    // Gesture and Viewport fills keep buffering, and that is not an oversight:
    // MessageView arms its scroll anchor ONCE per request it makes, so those
    // paths depend on seeing one insertion per request. An open arms no
    // anchor — the view is in its initial-load window, pinned to the end —
    // so the extra insertions have nothing to disturb.
    if (m_historyFill.kind() == bsfchat::client::HistoryFillKind::Open)
        ingestHistoryEvents(chronological, ownUserId);
    else
        m_historyFillPages.append(chronological);

    // The server's token is authoritative on every page, the first included:
    // an initial load's answer replaces whatever the model held.
    setPrevBatchToken(endToken);

    const bool wasSpent = m_historyFill.autoBudgetSpent();
    if (m_historyFill.onPage(newRows, endToken.isEmpty())) {
        m_historyFillFrom = endToken;
        if (wasSpent != m_historyFill.autoBudgetSpent()) emit historyAutoFillSpentChanged();
        result.outcome = HistoryPageOutcome::FetchMore;
        result.next = HistoryRequest{endToken, m_historyFill.nextPageLimit()};
        return result;
    }
    finishHistoryFill(ownUserId, wasSpent);
    result.outcome = HistoryPageOutcome::Done;
    return result;
}

bool MessageModel::failHistoryFill(const QString& requestedFrom, const QString& ownUserId)
{
    if (!m_historyFill.active() || requestedFrom != m_historyFillFrom) return false;
    const bool wasSpent = m_historyFill.autoBudgetSpent();
    m_historyFill.fail();
    finishHistoryFill(ownUserId, wasSpent);
    return true;
}

void MessageModel::ingestCachedWindow(const QVector<bsfchat::RoomEvent>& chronological,
                                      const QString& ownUserId)
{
    if (chronological.isEmpty()) return;
    m_ownUserId = ownUserId;

    // ONE insertion for the whole window, not one per event.
    //
    // appendEvent emits beginInsertRows/endInsertRows/countChanged per event,
    // and MessageView answers every countChanged with _recomputeUnreadDivider(),
    // which scans the timeline — so replaying a 400-event window through it
    // would be quadratic, and would cost 400 round trips into QML at the exact
    // moment the user is waiting to see the channel. The point of the cache is
    // that a switch is instant; a batched insert is what makes the local work
    // small enough for that to be true on a phone.
    QVector<MessageEntry> rows;
    QSet<QString> queued;
    rows.reserve(chronological.size());
    for (const auto& event : chronological) {
        if (!rendersAsRow(event)) continue;
        const QString eventId = QString::fromStdString(event.event_id);
        if (m_indexByEventId.contains(eventId) || queued.contains(eventId)) continue;
        queued.insert(eventId);
        MessageEntry entry = eventToEntry(event, ownUserId);
        drainPendingEdit(entry);
        rows.append(std::move(entry));
    }

    if (!rows.isEmpty()) {
        const int first = static_cast<int>(m_messages.size());
        beginInsertRows(QModelIndex(), first, first + static_cast<int>(rows.size()) - 1);
        for (auto& row : rows) m_messages.append(std::move(row));
        rebuildIndices();
        endInsertRows();
        emit countChanged();
    }

    // The window's relations, for ingestHistoryEvents' reason: a reaction, a
    // redaction or an edit sitting in the cached events is not a row and has
    // to be folded into one.
    for (const auto& event : chronological) {
        if (rendersAsRow(event)) continue;
        appendEvent(event, ownUserId);
    }
}

void MessageModel::ingestHistoryEvents(const QVector<bsfchat::RoomEvent>& chronological,
                                       const QString& ownUserId)
{
    if (chronological.isEmpty()) return;

    if (m_messages.isEmpty()) {
        // Nothing loaded: replay the lot in order, so reactions, redactions
        // and in-window edits all take the live path.
        for (const auto& event : chronological) appendEvent(event, ownUserId);
        return;
    }

    // Rows already present — older history, or the cold-start race: /sync's
    // long-poll can populate the model with the newest events before a
    // room-open's /messages answer lands. Appending that answer would drop
    // what /sync delivered as duplicates and put everything /sync DIDN'T
    // deliver — older history — at the END of the timeline (the pre-v0.0.37
    // bug: week-old messages at the bottom, April/May above them). It is a
    // prepend either way.
    prependEvents(chronological, ownUserId);

    // AND THEN THE PAGE'S RELATIONS, which prependEvents does not apply.
    //
    // prependEvents keeps rendersAsRow events and drops everything else, so a
    // reaction, a redaction or an edit that lives in this page was silently
    // thrown away. That was invisible while the only multi-page path put
    // every page through appendEvent (the branch above, taken because an open
    // started from an empty model) — but it was already losing them on every
    // scroll-to-top, and flushing an open's pages one at a time would have
    // made it lose them from page two onwards as well.
    //
    // appendEvent IS the relation handler: its reaction, redaction and
    // m.replace branches each return before the append, and its final guard
    // is `type != m.room.message`, which is exactly !rendersAsRow. So feeding
    // it the non-row events cannot produce a duplicate row — the one path
    // that appends requires the event to be a row, and those went through
    // prependEvents above.
    for (const auto& event : chronological) {
        if (rendersAsRow(event)) continue;
        appendEvent(event, ownUserId);
    }
}

void MessageModel::finishHistoryFill(const QString& ownUserId, bool wasSpent)
{
    // Pages arrived newest first; the model wants oldest first.
    QVector<bsfchat::RoomEvent> events;
    for (auto p = m_historyFillPages.crbegin(); p != m_historyFillPages.crend(); ++p)
        events += *p;
    m_historyFillPages.clear();
    m_historyFillRowIds.clear();
    m_historyFillFrom.clear();

    // Empty for an OPEN fill: absorbHistoryPage already put its pages in.
    ingestHistoryEvents(events, ownUserId);
    setLoadingHistory(false);
    if (wasSpent != m_historyFill.autoBudgetSpent()) emit historyAutoFillSpentChanged();
}

void MessageModel::clear()
{
    beginResetModel();
    m_messages.clear();
    m_indexByEventId.clear();
    m_threadReplyCounts.clear();
    m_pendingReactions.clear();
    m_pendingEdits.clear();
    m_reactionIndex.clear();
    // The rows are gone, so no echo is awaiting reconciliation any more.
    // Left set, it would make indexOfEchoMatching walk the new room's
    // timeline on every inbound event for nothing.
    m_unreconciledEchoes = 0;
    endResetModel();
    // A room switch invalidates the pagination state too — otherwise a
    // stale token from the previous room would drive the next scroll-up.
    const bool hadMore = hasMoreHistory();
    m_prevBatchToken.clear();
    if (hadMore) emit hasMoreHistoryChanged();
    if (m_loadingHistory) { m_loadingHistory = false; emit loadingHistoryChanged(); }
    // A fill in flight belongs to the room being left: its pages are dropped
    // (ServerConnection filters them by room id too) and the new visit gets a
    // fresh automatic budget.
    const bool wasSpent = m_historyFill.autoBudgetSpent();
    m_historyFill.reset();
    m_historyFillFrom.clear();
    m_historyFillPages.clear();
    m_historyFillRowIds.clear();
    if (wasSpent) emit historyAutoFillSpentChanged();
    emit countChanged();
}

bool MessageModel::resolveIsBot(const QString& userId) const
{
    return m_botUsers && m_botUsers->contains(userId);
}

void MessageModel::refreshBotFlags()
{
    // Same shape as refreshDisplayNames below, and for the same reason: this
    // is driven by member events, which arrive constantly, and repainting the
    // whole model for a flag that moved on one sender would rebuild every
    // delegate in a busy channel. In practice the flag moves once per bot per
    // session — the first member event that names it — so this loop almost
    // always finds nothing and emits nothing.
    QVector<QPair<int, int>> ranges;
    for (int i = 0; i < m_messages.size(); ++i) {
        const bool resolved = resolveIsBot(m_messages[i].sender);
        if (resolved == m_messages[i].senderIsBot) continue;
        m_messages[i].senderIsBot = resolved;
        if (!ranges.isEmpty() && ranges.last().second == i - 1) {
            ranges.last().second = i;
        } else {
            ranges.append({i, i});
        }
    }
    for (const auto& r : ranges) {
        emit dataChanged(index(r.first), index(r.second), {SenderIsBotRole});
    }
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
    // Called from the sync path for every m.room.member event that carries a
    // changed displayname, so it runs in bursts. It used to repaint the
    // entire model — dataChanged(row 0 .. row n-1) — for a single user's
    // rename, which in a busy channel means every delegate rebuilding while
    // messages are still arriving. Emit only the rows that actually changed,
    // coalesced into contiguous ranges.
    QVector<QPair<int, int>> ranges;
    for (int i = 0; i < m_messages.size(); ++i) {
        QString resolved = resolveDisplayName(m_messages[i].sender);
        if (resolved == m_messages[i].senderDisplayName) continue;
        m_messages[i].senderDisplayName = resolved;
        if (!ranges.isEmpty() && ranges.last().second == i - 1) {
            ranges.last().second = i;
        } else {
            ranges.append({i, i});
        }
    }
    if (ranges.isEmpty()) return;

    // One rename usually touches a handful of runs (a person's messages are
    // clustered). A pathological case — an initial member-state batch that
    // renames everybody — would otherwise emit hundreds of signals, each
    // with its own QML round trip, so collapse to a single span past a
    // threshold. Still bounded by the changed rows rather than the model.
    constexpr int kMaxRanges = 24;
    if (ranges.size() > kMaxRanges) {
        const int first = ranges.first().first;
        const int last = ranges.last().second;
        emit dataChanged(index(first), index(last), {SenderDisplayNameRole});
        return;
    }
    for (const auto& r : ranges) {
        emit dataChanged(index(r.first), index(r.second), {SenderDisplayNameRole});
    }
}
