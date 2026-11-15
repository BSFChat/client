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
//   * open tags are closed at the end, and nesting is capped.
//
// The output is always well-formed and contains only markup this function
// wrote itself; nothing in the input is copied through as markup.
QString sanitizeHtml(const QString& html);

// True when `href` (already entity-decoded) is a link target the timeline may
// render: http, https, mailto or bsfchat, with no control characters or
// whitespace that a browser would strip before reading the scheme.
bool isAllowedLinkHref(const QString& href);

} // namespace bsfchat::client
