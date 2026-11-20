#pragma once

#include <QChar>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <atomic>

#include <nlohmann/json.hpp>

// The m.room.message contents this client sends, built in one place.
//
// Every send path used to assemble its own JSON inline in MatrixClient, which
// is how they drifted: the thread path dropped formatted_body and m.mentions
// (so a mention inside a thread never notified anyone), and the edit path
// always wrote msgtype m.text with no url/info (so editing an /me turned it
// into plain text, and editing an image replaced it with its own file name).
// Header-only and free of QNetwork so tests can check the payloads directly.
namespace bsfchat::client::msgcontent {

using json = nlohmann::json;

// Sentinel for an @room broadcast in a mention list; see
// MatrixClient::kRoomMentionSentinel.
inline constexpr QLatin1StringView kRoomSentinel{"@room"};

// One transaction id generator for every send path. Each path used to keep
// its own static counter, and sendMessage and sendRichMessage shared the "m"
// prefix — so two sends in the same millisecond from those two paths minted
// the same id, and the server deduplicated the second one away (and the local
// echo for it never resolved).
inline QString nextTxnId(QStringView prefix)
{
    static std::atomic<quint64> counter{0};
    return QStringLiteral("%1%2.%3")
        .arg(prefix)
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(++counter);
}

// The msgtypes a user can edit: prose. A media event's body is its file name,
// and m.new_content replaces the whole content server-side, so an edit there
// would have to carry url/info forward — and nothing renders a caption.
inline bool isEditableMsgtype(const QString& msgtype)
{
    return msgtype == QLatin1String("m.text") || msgtype == QLatin1String("m.notice")
        || msgtype == QLatin1String("m.emote");
}

// Attach the MSC3952 `m.mentions` block to `content`, in place. "@room" is
// lifted into the `room` boolean. Nothing is written for an empty list.
inline void applyMentions(json& content, const QStringList& mentionedUserIds)
{
    if (mentionedUserIds.isEmpty()) return;
    json users = json::array();
    bool roomMention = false;
    for (const auto& u : mentionedUserIds) {
        if (u == kRoomSentinel) { roomMention = true; continue; }
        users.push_back(u.toStdString());
    }
    json mentions = json::object();
    if (!users.empty()) mentions["user_ids"] = users;
    if (roomMention) mentions["room"] = true;
    if (!mentions.empty()) content["m.mentions"] = mentions;
}

// A prose message: msgtype, body, and the HTML pair when there is HTML.
inline json prose(const QString& msgtype, const QString& body,
                  const QString& formattedBody = {},
                  const QStringList& mentionedUserIds = {})
{
    json content;
    content["msgtype"] = msgtype.toStdString();
    content["body"] = body.toStdString();
    if (!formattedBody.isEmpty()) {
        content["format"] = "org.matrix.custom.html";
        content["formatted_body"] = formattedBody.toStdString();
    }
    applyMentions(content, mentionedUserIds);
    return content;
}

inline json reply(const QString& targetEventId, const QString& body,
                  const QString& formattedBody, const QStringList& mentionedUserIds)
{
    json content = prose(QStringLiteral("m.text"), body, formattedBody, mentionedUserIds);
    content["m.relates_to"] = {
        {"m.in_reply_to", {{"event_id", targetEventId.toStdString()}}},
    };
    return content;
}

// A thread reply is an ordinary new send as far as mentions go, so it carries
// formatted_body and m.mentions exactly as a main-timeline message does.
inline json threadReply(const QString& threadRootId, const QString& body,
                        const QString& formattedBody, const QStringList& mentionedUserIds)
{
    json content = prose(QStringLiteral("m.text"), body, formattedBody, mentionedUserIds);
    content["m.relates_to"] = {
        {"rel_type", "m.thread"},
        {"event_id", threadRootId.toStdString()},
    };
    return content;
}

// An m.replace. `msgtype` is the ORIGINAL's (an /me stays m.emote). Returns a
// null json for a msgtype that cannot be edited — the caller must not send.
//
// m.mentions goes in both copies: m.new_content because the server folds it in
// as the event's content, and the top level because that is the copy the
// server validates (MENTION_EVERYONE for `room`). See MatrixClient::editMessage.
inline json edit(const QString& targetEventId, const QString& msgtype,
                 const QString& newBody, const QString& formattedBody,
                 const QStringList& mentionedUserIds)
{
    const QString type = msgtype.isEmpty() ? QStringLiteral("m.text") : msgtype;
    if (!isEditableMsgtype(type)) return json();
    json content = prose(type, QStringLiteral("* ") + newBody);
    content["m.new_content"] = prose(type, newBody, formattedBody, mentionedUserIds);
    content["m.relates_to"] = {
        {"rel_type", "m.replace"},
        {"event_id", targetEventId.toStdString()},
    };
    applyMentions(content, mentionedUserIds);
    return content;
}

// A forwarded message: a plain m.text with an attribution line and the
// original quoted under it, plus formatted_body so the attribution renders
// the same everywhere. `sourceLabel` is display-ready ("#general", or a DM's
// name); `senderName` is the original author's display name. Both are
// user-controlled and are HTML-escaped here — they used to go into a markdown
// string raw, so a channel called "*x*" or a name with "](" in it reshaped
// the header. `sourceFormattedBody` is the source row's already-sanitised
// display markup; when empty the plain body is escaped instead. With all
// three source ids the label links back to the original
// (bsfchat://message/<server>/<room>/<event>).
inline json forward(const QString& body, const QString& sourceFormattedBody,
                    const QString& sourceLabel, const QString& senderName,
                    const QString& sourceServerUrl, const QString& sourceRoomId,
                    const QString& sourceEventId)
{
    const QString label = sourceLabel.isEmpty() ? QStringLiteral("unknown") : sourceLabel;
    const QString sender = senderName.isEmpty() ? QStringLiteral("unknown") : senderName;

    QString labelHtml = label.toHtmlEscaped();
    if (!sourceServerUrl.isEmpty() && !sourceRoomId.isEmpty() && !sourceEventId.isEmpty()) {
        const QString href = QStringLiteral("bsfchat://message/%1/%2/%3")
            .arg(QString::fromUtf8(QUrl::toPercentEncoding(sourceServerUrl, "", "/")),
                 QString::fromUtf8(QUrl::toPercentEncoding(sourceRoomId)),
                 QString::fromUtf8(QUrl::toPercentEncoding(sourceEventId)));
        labelHtml = QStringLiteral("<a href=\"%1\">%2</a>").arg(href.toHtmlEscaped(), labelHtml);
    }
    const QString header = QStringLiteral("Forwarded from %1 — originally by @%2");

    QString quoted;
    const QStringList lines = body.split(QLatin1Char('\n'));
    for (const auto& line : lines) quoted += QStringLiteral("\n> ") + line;
    const QString plain = header.arg(label, sender) + quoted;

    QString inner = sourceFormattedBody;
    if (inner.isEmpty())
        inner = body.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    const QString html = QStringLiteral("<blockquote><b>%1</b><br>%2</blockquote>")
        .arg(header.arg(labelHtml, sender.toHtmlEscaped()), inner);

    return prose(QStringLiteral("m.text"), plain, html);
}

} // namespace bsfchat::client::msgcontent
