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
QString renderMentions(const QString& html,
                       const QVector<MentionTarget>& targets,
                       bool roomMention);

} // namespace bsfchat::client
