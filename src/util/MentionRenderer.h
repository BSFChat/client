#pragma once

#include <QString>
#include <QVector>

namespace bsfchat::client {

// One resolved @-mention target. `userId` is the authoritative identity (it
// came out of m.mentions.user_ids, which the sender controls but the server
// echoes verbatim); `displayName` is whatever the room currently calls that
// user.
//
// BOTH fields are attacker-controlled text. A display name is set by its
// owner and can be `<img src=x onerror=alert(1)>` or `" onmouseover="`, and
// the rendered body is fed to Qt's RichText engine. The renderer therefore
// never interpolates either field into markup raw: display names are emitted
// only as HTML-escaped element *text*, user ids only percent-encoded inside
// an href.
struct MentionTarget {
    QString userId;
    QString displayName;
    bool isSelf = false;
};

// One resolved ROLE mention.
//
// Only roles that ACTUALLY NOTIFIED are ever put in this list. The caller
// (MessageModel, via ServerConnection's resolver) applies the same rule the
// server applies — the role exists, and either it is `mentionable` or the
// sender holds MENTION_EVERYONE — and drops the rest before calling here.
//
// That is the answer to "what happens to the rendering when a non-mentionable
// role is named": nothing at all. The token is not in this list, so no needle
// matches it, so it survives as the ordinary escaped text it already was. It
// reads as "@Moderator", in the body colour, exactly as typed. It is NOT
// stripped and NOT quietly turned into an empty span, because the sender did
// write those characters and a reader comparing the message to what they meant
// to send should see them. What they should not see is a pill implying somebody
// was pinged when nobody was.
//
// `name` and `color` are attacker-controlled in the same way a display name is
// — a role name is set by whoever has MANAGE_ROLES — and get the same
// treatment: the name is emitted only as HTML-escaped element text, and the
// colour is used only after being validated as a #RRGGBB literal.
struct RoleMentionTarget {
    QString roleId;
    QString name;
    QString color;      // "#RRGGBB", or empty for the default mention colour
    bool includesMe = false;
};

// The composer writes a mention as '@' + the display name with all whitespace
// removed (see MessageInput.qml's _stripToToken), so that the token survives
// copy/paste and re-editing as a single word. Both sides have to agree on the
// transform or received mentions never match their token.
QString mentionToken(const QString& displayName);

// Rewrite the @-mention tokens in `html` into styled anchors.
//
// `html` must ALREADY be escaped/rendered markup — MarkdownParser::toHtml
// output, or a sender-supplied formatted_body. Consequences of that contract:
//
//   * Matching happens against the *escaped* form of each token, so a display
//     name of `A&B` is found inside `@A&amp;B` and a name containing `<` can
//     never match a real tag.
//   * Only text outside tags is rewritten, and never inside an existing <a>,
//     <code> or <pre>. Without that, running over our own composer's output
//     would nest an anchor inside an anchor, and mentions inside code spans
//     would become links.
//
// Anchors that already point at bsfchat://user/<id> are re-emitted in the
// canonical style so a sender's colour choices (or a hand-rolled attribute)
// can't survive into our rendering, and so self-mentions get the self style
// regardless of which client sent them.
//
// `roomMention` reflects m.mentions.room: when true, a literal `@room` token
// is highlighted too.
//
// `roleTargets` are the role mentions that took effect. A role token is matched
// against '@' + the role's name with whitespace stripped, the same transform
// user mentions use (mentionToken), so "@Server Staff" in the composer and
// "@ServerStaff" in the body are the same token. A role the reader holds is
// styled like a self-mention, because it is one: they were notified.
QString renderMentions(const QString& html,
                       const QVector<MentionTarget>& targets,
                       bool roomMention,
                       const QVector<RoleMentionTarget>& roleTargets = {});

} // namespace bsfchat::client
