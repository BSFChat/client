#include "model/MessageModel.h"

#include <QDateTime>
#include <QSet>
#include <bsfchat/Constants.h>
#include "util/MarkdownParser.h"
#include "util/MediaUrl.h"
#include "util/MentionRenderer.h"

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
        {MentionsRoomRole, "mentionsRoom"}
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
    return bsfchat::client::buildMediaDownloadUrl(
        m_homeserver, m_accessToken ? *m_accessToken : QString(), mxcUri);
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

    // If no formatted_body from server, apply local markdown rendering
    if (entry.formattedBody.isEmpty() && !entry.body.isEmpty() && entry.msgtype == "m.text") {
        entry.formattedBody = MarkdownParser::toHtml(entry.body);
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

void MessageModel::applyMentionMarkup(MessageEntry& entry) const
{
    if (entry.mentionedUserIds.isEmpty() && !entry.mentionsRoom) return;
    // Media rows render a filename, not prose; there is nothing to highlight
    // and formattedBody is not shown for them.
    if (entry.msgtype == "m.image" || entry.msgtype == "m.file"
        || entry.msgtype == "m.audio" || entry.msgtype == "m.video") return;

    QVector<bsfchat::client::MentionTarget> targets;
    targets.reserve(entry.mentionedUserIds.size());
    for (const QString& uid : entry.mentionedUserIds) {
        targets.append({uid, resolveDisplayName(uid),
                        !m_ownUserId.isEmpty() && uid == m_ownUserId});
    }

    // renderMentions requires already-escaped markup. When the sender supplied
    // no formatted_body and markdown rendering didn't kick in (e.g. m.notice),
    // escape the plain body ourselves rather than handing it raw text — the
    // renderer would otherwise match tokens against unescaped input and the
    // result would be interpreted as RichText.
    QString base = entry.formattedBody.isEmpty() ? entry.body.toHtmlEscaped()
                                                 : entry.formattedBody;
    entry.formattedBody = bsfchat::client::renderMentions(
        base, targets, entry.mentionsRoom);
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

    if (isEdit && !targetId.isEmpty()) {
        // Look up the target. If we don't have it yet (edit arrived before
        // the original via out-of-order sync), silently drop — a future
        // sync/backfill will bring the original, and we'll see this edit
        // again or via its own m_new_content chain. Keeping edits in the
        // timeline would double-render the message.
        const int i = rowForEventId(targetId);
        // Target not found — ignore silently.
        if (i < 0) return;

        // Already reflected in the target's body? Then this sibling carries no
        // news. Two ways that happens:
        //   * The server reconciled the edit before sending us the original,
        //     and recorded this event id in unsigned.m.relations.m.replace.
        //     Applying it again would seed `history` with the CURRENT text and
        //     "Show edit history" would list the same body twice.
        //   * Sync replayed the same replacement event.
        // A *later* edit has a different event id and still lands below, which
        // is what keeps live editing working while the user watches the room.
        const QString editEventId = QString::fromStdString(event.event_id);
        if (!editEventId.isEmpty()
            && m_messages[i].appliedEdits.contains(editEventId)) return;

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
        // Stash the previous body into history so "Show edit
        // history" can recover it. We push EITHER the pristine
        // original (before any edit) or the last edit — so the
        // user sees every distinct version.
        qint64 prevTs = m_messages[i].edited
            ? m_messages[i].editedAt : m_messages[i].timestamp;
        m_messages[i].history.append({m_messages[i].body, prevTs});
        m_messages[i].body = newBody;
        m_messages[i].formattedBody = newFormatted;
        m_messages[i].edited = true;
        m_messages[i].editedAt = event.origin_server_ts;
        if (!editEventId.isEmpty()) m_messages[i].appliedEdits.insert(editEventId);
        // The edit re-wrote the body, which threw away the mention anchors the
        // previous rendering had baked in. Re-apply from the entry's recorded
        // mention set so an edited message keeps its highlights.
        //
        // From the ENTRY's set — the original's — never from this replacement
        // event's own m.mentions, which is why nothing above reads it. The
        // server records no mention row and fires no push for an m.replace, so
        // a mention an edit introduces notified nobody and must not render as
        // though it had. notifiedMentions() is the same rule applied to the
        // reconciled event a later reload sees; the two paths have to agree or
        // a highlight would appear on relaunch that was not there live.
        applyMentionMarkup(m_messages[i]);
        auto idx = index(i);
        emit dataChanged(idx, idx, {BodyRole, FormattedBodyRole, EditedRole});
        return;
    }

    // Regular new message — dedupe + append. The index is authoritative for
    // the dedupe: sync replays the same event id often enough that this was
    // a full scan per inbound message.
    QString eventId = QString::fromStdString(event.event_id);
    if (m_indexByEventId.contains(eventId)) return;

    beginInsertRows(QModelIndex(), m_messages.size(), m_messages.size());
    m_messages.append(eventToEntry(event, ownUserId));
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
        if (event.type != std::string(bsfchat::event_type::kRoomMessage))
            continue;
        // Edit siblings are not messages. /messages returns them alongside the
        // originals, and this path never applied them — so every edit in the
        // fetched page showed up as its own junk row rendering the "* new text"
        // fallback body, directly above the message it had already been folded
        // into. There is nothing to apply either: the original arrives from the
        // same page already reconciled, carrying the bundle eventToEntry reads.
        if (isReplacementEvent(event)) continue;
        QString eventId = QString::fromStdString(event.event_id);
        // Against both the loaded rows and the batch itself: overlapping
        // /messages pages can repeat an id inside a single call, which the
        // old m_messages-only scan let through.
        if (m_indexByEventId.contains(eventId) || queued.contains(eventId)) continue;
        queued.insert(eventId);
        newEntries.append(eventToEntry(event, ownUserId));
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
    m_indexByEventId.clear();
    m_threadReplyCounts.clear();
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
