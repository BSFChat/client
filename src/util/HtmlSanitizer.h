#pragma once

#include <QString>

namespace bsfchat::client {

// Reduce a sender-supplied `formatted_body` to markup that is safe to hand to
// a QML Text/TextEdit in RichText mode.
//
// The timeline renders formatted_body as rich text, and it used to do so
// verbatim: `<img src="https://tracker/…" width=4000>` loaded a remote image
// on every viewer's machine (IP disclosure, and a decode bomb away from a
// freeze), and `<a href="file://…">` or a link whose text names another site
// went straight to the click handler.
//
// The rules, in short:
//   * a fixed tag allowlist (inline formatting, code, quotes, lists, headings,
//     simple tables); every other tag is dropped and its text kept;
//   * script, style, iframe, object, svg, … are dropped WITH their content;
//   * <img> becomes its alt text, escaped; nothing is ever fetched;
//   * no attributes survive except <a href>, and only for http, https, mailto
//     and bsfchat URLs (checked after entity decoding); a link with any other
//     target keeps its text and loses the anchor;
//   * <a>, <code>, <pre> and <blockquote> are re-emitted with this client's
//     own inline style, so our composer's markdown still renders as before;
//   * comments, doctypes and processing instructions are removed; stray '<',
//     '>' and '&' are escaped; well-formed character references are kept;
//   * open tags are closed at the end, and nesting is capped;
//   * the one exception to "no attributes": a <span data-mx-spoiler> (any
//     value — the reason is dropped) is re-emitted as exactly
//     kSpoilerOpen, so hideSpoilers/revealSpoilers below can find it.
//
// The output is always well-formed and contains only markup this function
// wrote itself; nothing in the input is copied through as markup.
QString sanitizeHtml(const QString& html);

// True when `href` (already entity-decoded) is a link target the timeline may
// render: http, https, mailto or bsfchat, with no control characters or
// whitespace that a browser would strip before reading the scheme.
bool isAllowedLinkHref(const QString& href);

// The canonical spoiler opener sanitizeHtml emits. Qt's rich text ignores the
// attribute, so markup holding one must go through hideSpoilers (or
// revealSpoilers) before it is displayed — otherwise the text shows openly.
inline constexpr char kSpoilerOpen[] = "<span data-mx-spoiler>";

// The link a hidden spoiler becomes; the view reveals the message's spoilers
// when it is activated.
inline constexpr char kSpoilerHref[] = "bsfchat://spoiler";

// Replace every spoiler in SANITISED markup with a click-to-reveal block: a
// bsfchat://spoiler link showing one "█" per visible character (capped), with
// the spoiler's own text and markup gone. Inside an existing link the block is
// not a link (no nested anchors). Markup without a spoiler comes back as is.
QString hideSpoilers(const QString& sanitized);
// The revealed form: each spoiler span gets a tinted background instead.
QString revealSpoilers(const QString& sanitized);

} // namespace bsfchat::client
