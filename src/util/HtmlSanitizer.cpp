#include "util/HtmlSanitizer.h"

#include <QStringList>
#include <QVector>

#include <algorithm>

namespace bsfchat::client {
namespace {

// The inline styles MarkdownParser gives the same elements. Re-emitted here so
// a message our own composer sent looks the same after the round trip through
// the server, without letting a sender's own style attribute through.
constexpr char kPreStyle[] =
    "background-color:#1e1f22; padding:8px 12px; border-radius:4px; "
    "font-family:monospace; margin:4px 0;";
constexpr char kCodeStyle[] =
    "background-color:#1e1f22; padding:2px 6px; border-radius:3px; "
    "font-family:monospace;";
constexpr char kQuoteStyle[] =
    "border-left:3px solid #4e5058; padding-left:8px; margin:4px 0; color:#b5bac1;";
constexpr char kLinkStyle[] = "color:#5865f2; text-decoration:none;";
constexpr char kChannelLinkStyle[] =
    "color:#5865f2; text-decoration:none; font-weight:bold;";

constexpr int kMaxDepth = 32;
constexpr int kMaxTags = 4000;

bool isAsciiAlpha(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z');
}

bool isAsciiAlnum(QChar c)
{
    const char16_t u = c.unicode();
    return isAsciiAlpha(c) || (u >= '0' && u <= '9');
}

bool isHtmlSpace(QChar c)
{
    const char16_t u = c.unicode();
    return u == ' ' || u == '\t' || u == '\n' || u == '\r' || u == '\f';
}

QString asciiLower(const QString& s)
{
    QString out = s;
    for (QChar& c : out)
        if (c.unicode() >= 'A' && c.unicode() <= 'Z') c = QChar(c.unicode() + 32);
    return out;
}

// Elements dropped together with everything up to their end tag.
bool dropsContent(const QString& tag)
{
    static const QStringList k = {
        QStringLiteral("script"),   QStringLiteral("style"),    QStringLiteral("iframe"),
        QStringLiteral("object"),   QStringLiteral("embed"),    QStringLiteral("applet"),
        QStringLiteral("noscript"), QStringLiteral("template"), QStringLiteral("textarea"),
        QStringLiteral("title"),    QStringLiteral("head"),     QStringLiteral("svg"),
        QStringLiteral("math"),     QStringLiteral("select"),   QStringLiteral("frameset"),
        QStringLiteral("frame"),    QStringLiteral("noframes"), QStringLiteral("noembed"),
        QStringLiteral("xmp"),      QStringLiteral("plaintext"), QStringLiteral("video"),
        QStringLiteral("audio"),    QStringLiteral("canvas"),   QStringLiteral("mx-reply"),
    };
    return k.contains(tag);
}

// Allowed elements, by canonical name. Synonyms are folded first.
QString canonicalTag(const QString& tag)
{
    if (tag == QLatin1String("del") || tag == QLatin1String("strike"))
        return QStringLiteral("s");
    static const QStringList k = {
        QStringLiteral("b"),   QStringLiteral("strong"), QStringLiteral("i"),
        QStringLiteral("em"),  QStringLiteral("u"),      QStringLiteral("s"),
        QStringLiteral("code"), QStringLiteral("pre"),   QStringLiteral("blockquote"),
        QStringLiteral("br"),  QStringLiteral("p"),      QStringLiteral("ul"),
        QStringLiteral("ol"),  QStringLiteral("li"),     QStringLiteral("a"),
        QStringLiteral("span"), QStringLiteral("h1"),    QStringLiteral("h2"),
        QStringLiteral("h3"),  QStringLiteral("h4"),     QStringLiteral("h5"),
        QStringLiteral("h6"),  QStringLiteral("sub"),    QStringLiteral("sup"),
        QStringLiteral("hr"),  QStringLiteral("table"),  QStringLiteral("thead"),
        QStringLiteral("tbody"), QStringLiteral("tr"),   QStringLiteral("td"),
        QStringLiteral("th"),
    };
    return k.contains(tag) ? tag : QString();
}

bool isVoid(const QString& tag)
{
    return tag == QLatin1String("br") || tag == QLatin1String("hr");
}

// Length of a well-formed character reference starting at text[i] == '&',
// or 0. Only these are passed through; any other '&' is escaped.
int entityLength(const QString& text, int i)
{
    const int n = text.size();
    int j = i + 1;
    if (j < n && text[j] == QLatin1Char('#')) {
        ++j;
        bool hex = false;
        if (j < n && (text[j] == QLatin1Char('x') || text[j] == QLatin1Char('X'))) {
            hex = true;
            ++j;
        }
        const int start = j;
        while (j < n && j - start < 7) {
            const char16_t u = text[j].unicode();
            const bool digit = (u >= '0' && u <= '9');
            const bool hexDigit = digit || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F');
            if (hex ? !hexDigit : !digit) break;
            ++j;
        }
        if (j == start || j >= n || text[j] != QLatin1Char(';')) return 0;
        return j - i + 1;
    }
    const int start = j;
    while (j < n && j - start < 32 && isAsciiAlnum(text[j])) ++j;
    if (j == start || !isAsciiAlpha(text[start]) || j >= n || text[j] != QLatin1Char(';'))
        return 0;
    return j - i + 1;
}

// Decode character references in an attribute value, the way a browser would
// before looking at a URL scheme: numeric ones with or without the ';', and
// the named ones that matter for that decision.
QString decodeEntities(const QString& s)
{
    QString out;
    out.reserve(s.size());
    const int n = s.size();
    for (int i = 0; i < n; ++i) {
        if (s[i] != QLatin1Char('&')) { out += s[i]; continue; }
        int j = i + 1;
        if (j < n && s[j] == QLatin1Char('#')) {
            ++j;
            int base = 10;
            if (j < n && (s[j] == QLatin1Char('x') || s[j] == QLatin1Char('X'))) {
                base = 16;
                ++j;
            }
            const int start = j;
            while (j < n && j - start < 8) {
                const char16_t u = s[j].unicode();
                const bool digit = u >= '0' && u <= '9';
                if (!(digit || (base == 16 && ((u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F')))))
                    break;
                ++j;
            }
            bool ok = false;
            const uint cp = s.mid(start, j - start).toUInt(&ok, base);
            if (!ok || j == start) { out += s[i]; continue; }
            if (j < n && s[j] == QLatin1Char(';')) ++j;
            if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                out += QChar(0xFFFD);
            } else {
                const char32_t c32 = cp;
                out += QString::fromUcs4(&c32, 1);
            }
            i = j - 1;
            continue;
        }
        static const struct { const char* name; const char* value; } kNamed[] = {
            {"amp;", "&"}, {"lt;", "<"}, {"gt;", ">"}, {"quot;", "\""},
            {"apos;", "'"}, {"colon;", ":"}, {"Tab;", "\t"}, {"NewLine;", "\n"},
            {"nbsp;", "\xC2\xA0"}, {"sol;", "/"}, {"period;", "."},
        };
        bool matched = false;
        for (const auto& e : kNamed) {
            const QLatin1String name(e.name);
            if (QStringView(s).mid(j).startsWith(name)) {
                out += QString::fromUtf8(e.value);
                i = j + name.size() - 1;
                matched = true;
                break;
            }
        }
        if (!matched) out += s[i];
    }
    return out;
}

// Text content: escape markup characters, keep well-formed references.
void appendText(QString& out, QStringView text)
{
    const QString t = text.toString();
    for (int i = 0; i < t.size(); ++i) {
        const QChar c = t[i];
        switch (c.unicode()) {
        case '<': out += QLatin1String("&lt;"); break;
        case '>': out += QLatin1String("&gt;"); break;
        case 0: break;
        case '&': {
            const int len = entityLength(t, i);
            if (len > 0) {
                out += QStringView(t).mid(i, len);
                i += len - 1;
            } else {
                out += QLatin1String("&amp;");
            }
            break;
        }
        default: out += c;
        }
    }
}

struct Attr {
    QString name;  // ASCII-lowercased
    QString value; // raw, entities not yet decoded
};

constexpr int kMaxAttrs = 64;

struct Tag {
    QString name; // ASCII-lowercased
    QVector<Attr> attrs;
    bool closing = false;
    int end = -1; // index one past the closing '>'
};

// Parse a tag whose '<' is at html[i]. end == -1 means it never terminates.
Tag parseTag(const QString& html, int i)
{
    Tag tag;
    const int n = html.size();
    int j = i + 1;
    if (j < n && html[j] == QLatin1Char('/')) { tag.closing = true; ++j; }
    const int nameStart = j;
    // Letters/digits, and '-' after the first (custom elements: mx-reply).
    while (j < n && (isAsciiAlnum(html[j]) || (j > nameStart && html[j] == QLatin1Char('-')))) ++j;
    tag.name = asciiLower(html.mid(nameStart, j - nameStart));

    while (j < n) {
        const QChar c = html[j];
        if (c == QLatin1Char('>')) { tag.end = j + 1; return tag; }
        if (isHtmlSpace(c) || c == QLatin1Char('/')) { ++j; continue; }
        // Attribute name: up to whitespace, '/', '>' or '='. A leading '='
        // (or any other junk) is taken as one character of name so the loop
        // always advances.
        const int an = j;
        ++j;
        while (j < n && !isHtmlSpace(html[j]) && html[j] != QLatin1Char('/')
               && html[j] != QLatin1Char('>') && html[j] != QLatin1Char('='))
            ++j;
        // Past the cap the attribute is still scanned (to find the tag's end)
        // but nothing is copied: a tag of 30 000 attributes costs one pass.
        const bool keep = tag.attrs.size() < kMaxAttrs;
        Attr attr;
        if (keep) attr.name = asciiLower(html.mid(an, j - an));
        int k = j;
        while (k < n && isHtmlSpace(html[k])) ++k;
        if (k < n && html[k] == QLatin1Char('=')) {
            ++k;
            while (k < n && isHtmlSpace(html[k])) ++k;
            if (k < n && (html[k] == QLatin1Char('"') || html[k] == QLatin1Char('\''))) {
                const QChar q = html[k];
                const int close = html.indexOf(q, k + 1);
                if (close < 0) return tag; // unterminated quote: no tag
                if (keep) attr.value = html.mid(k + 1, close - k - 1);
                j = close + 1;
            } else {
                const int vs = k;
                while (k < n && !isHtmlSpace(html[k]) && html[k] != QLatin1Char('>')) ++k;
                if (keep) attr.value = html.mid(vs, k - vs);
                j = k;
            }
        }
        if (keep) tag.attrs.append(std::move(attr));
    }
    return tag;
}

QString attrValue(const Tag& tag, const char* name)
{
    for (const auto& a : tag.attrs)
        if (a.name == QLatin1String(name)) return a.value;
    return QString();
}

// Index just past the end tag `</name ...>` at or after `from`, or -1.
int skipPastEndTag(const QString& html, int from, const QString& name)
{
    const QString needle = QStringLiteral("</") + name;
    int pos = from;
    while (true) {
        pos = html.indexOf(needle, pos, Qt::CaseInsensitive);
        if (pos < 0) return -1;
        const int after = pos + needle.size();
        if (after >= html.size()
            || !(isAsciiAlnum(html[after]) || html[after] == QLatin1Char('-'))) {
            const int gt = html.indexOf(QLatin1Char('>'), after);
            return gt < 0 ? -1 : gt + 1;
        }
        pos = after;
    }
}

} // namespace

bool isAllowedLinkHref(const QString& href)
{
    if (href.isEmpty()) return false;
    for (const QChar c : href) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7F || c.isSpace()) return false;
    }
    const int colon = href.indexOf(QLatin1Char(':'));
    if (colon <= 0) return false; // relative, or no scheme at all
    const QString scheme = asciiLower(href.left(colon));
    for (const QChar c : scheme)
        if (!isAsciiAlnum(c) && c != QLatin1Char('+') && c != QLatin1Char('-')
            && c != QLatin1Char('.'))
            return false;
    return scheme == QLatin1String("http") || scheme == QLatin1String("https")
        || scheme == QLatin1String("mailto") || scheme == QLatin1String("bsfchat");
}

QString sanitizeHtml(const QString& html)
{
    if (html.isEmpty()) return {};

    QString out;
    out.reserve(html.size() + 64);
    QVector<QString> open; // canonical names of the elements we emitted
    int emittedTags = 0;
    const int n = html.size();
    int textStart = 0;
    int i = 0;

    auto flushText = [&](int upTo) {
        if (upTo > textStart) appendText(out, QStringView(html).mid(textStart, upTo - textStart));
    };
    auto inside = [&](const char* name) { return open.contains(QLatin1String(name)); };

    while (i < n) {
        if (html[i] != QLatin1Char('<')) { ++i; continue; }
        flushText(i);
        const QChar next = i + 1 < n ? html[i + 1] : QChar();

        // Comments, doctypes, CDATA, processing instructions: removed.
        if (next == QLatin1Char('!') || next == QLatin1Char('?')) {
            int end;
            if (QStringView(html).mid(i).startsWith(QLatin1String("<!--"))) {
                const int close = html.indexOf(QLatin1String("-->"), i + 4);
                end = close < 0 ? n : close + 3;
            } else {
                const int gt = html.indexOf(QLatin1Char('>'), i + 2);
                end = gt < 0 ? n : gt + 1;
            }
            i = textStart = end;
            continue;
        }

        const bool looksLikeTag = isAsciiAlpha(next)
            || (next == QLatin1Char('/') && i + 2 < n && isAsciiAlpha(html[i + 2]));
        if (!looksLikeTag) {
            // Not markup: the '<' is text.
            out += QLatin1String("&lt;");
            i = textStart = i + 1;
            continue;
        }
        const Tag tag = parseTag(html, i);
        if (tag.end < 0) {
            // Never terminated: parseTag scanned to the end of the input, so
            // the whole remainder is text, escaped in one pass. Re-parsing
            // from every later '<' instead was quadratic: 64 KiB of "<a "
            // froze each viewer for seconds.
            textStart = i;
            break;
        }
        i = textStart = tag.end;

        if (tag.closing) {
            const QString name = canonicalTag(tag.name);
            if (name.isEmpty() || isVoid(name)) continue;
            const int at = open.lastIndexOf(name);
            if (at < 0) continue; // never opened by us: ignore
            while (open.size() > at) out += QLatin1String("</") + open.takeLast() + QLatin1Char('>');
            continue;
        }

        if (dropsContent(tag.name)) {
            const int past = skipPastEndTag(html, tag.end, tag.name);
            i = textStart = past < 0 ? n : past;
            continue;
        }

        if (tag.name == QLatin1String("img")) {
            // Never fetched. The alt text is the only thing kept.
            out += decodeEntities(attrValue(tag, "alt")).toHtmlEscaped();
            continue;
        }

        const QString name = canonicalTag(tag.name);
        if (name.isEmpty()) continue; // unknown element: drop the tag, keep the text
        if (++emittedTags > kMaxTags) continue;

        if (isVoid(name)) {
            out += QLatin1Char('<') + name + QLatin1Char('>');
            continue;
        }
        if (open.size() >= kMaxDepth) continue;

        if (name == QLatin1String("a")) {
            if (inside("a")) continue; // no nested anchors
            const QString href = decodeEntities(attrValue(tag, "href")).trimmed();
            if (!isAllowedLinkHref(href)) continue; // keep the text, lose the link
            const bool channel =
                href.startsWith(QLatin1String("bsfchat://channel/"), Qt::CaseInsensitive);
            out += QLatin1String("<a href=\"") + href.toHtmlEscaped()
                 + QLatin1String("\" style=\"")
                 + QLatin1String(channel ? kChannelLinkStyle : kLinkStyle)
                 + QLatin1String("\">");
        } else if (name == QLatin1String("pre")) {
            out += QLatin1String("<pre style=\"") + QLatin1String(kPreStyle) + QLatin1String("\">");
        } else if (name == QLatin1String("code")) {
            // Inside a <pre> the block already carries the background.
            if (inside("pre")) out += QLatin1String("<code>");
            else out += QLatin1String("<code style=\"") + QLatin1String(kCodeStyle) + QLatin1String("\">");
        } else if (name == QLatin1String("span")
                   && std::any_of(tag.attrs.cbegin(), tag.attrs.cend(), [](const Attr& a) {
                          return a.name == QLatin1String("data-mx-spoiler");
                      })) {
            // The spoiler marker is the one attribute kept, in a fixed form
            // and with no value (the reason is not shown).
            out += QLatin1String(kSpoilerOpen);
        } else if (name == QLatin1String("blockquote")) {
            out += QLatin1String("<blockquote style=\"") + QLatin1String(kQuoteStyle)
                 + QLatin1String("\">");
        } else {
            out += QLatin1Char('<') + name + QLatin1Char('>');
        }
        open.append(name);
    }
    flushText(n);

    while (!open.isEmpty()) out += QLatin1String("</") + open.takeLast() + QLatin1Char('>');
    return out;
}

namespace {

// Index just past the </span> closing the span opened at `open` (whose '<' is
// at `open`), in sanitised markup — balanced, canonical tags only — or -1.
int matchingSpanEnd(const QString& html, int open)
{
    int depth = 0;
    int i = open;
    const int n = html.size();
    while (i < n) {
        const int lt = html.indexOf(QLatin1Char('<'), i);
        if (lt < 0) return -1;
        const int gt = html.indexOf(QLatin1Char('>'), lt);
        if (gt < 0) return -1;
        const QStringView tag = QStringView(html).mid(lt, gt - lt + 1);
        if (tag.startsWith(QLatin1String("<span"))) ++depth;
        else if (tag == QLatin1String("</span>") && --depth == 0) return gt + 1;
        i = gt + 1;
    }
    return -1;
}

// Visible characters in a run of markup: tags skipped, an entity counts once.
int visibleLength(QStringView html)
{
    int count = 0;
    for (int i = 0; i < html.size(); ++i) {
        const QChar c = html[i];
        if (c == QLatin1Char('<')) {
            const int gt = html.indexOf(QLatin1Char('>'), i);
            if (gt < 0) break;
            i = gt;
            continue;
        }
        if (c == QLatin1Char('&')) {
            const int semi = html.indexOf(QLatin1Char(';'), i);
            if (semi > i) i = semi;
        }
        if (!c.isLowSurrogate()) ++count;
    }
    return count;
}

// Whether position `at` in sanitised markup is inside an <a>.
bool insideAnchor(const QString& html, int at)
{
    const int open = html.lastIndexOf(QLatin1String("<a "), at);
    if (open < 0) return false;
    const int close = html.lastIndexOf(QLatin1String("</a>"), at);
    return close < open;
}

} // namespace

QString hideSpoilers(const QString& sanitized)
{
    const QLatin1String opener(kSpoilerOpen);
    if (!sanitized.contains(opener)) return sanitized;
    QString out;
    out.reserve(sanitized.size());
    int pos = 0;
    while (true) {
        const int at = sanitized.indexOf(opener, pos);
        if (at < 0) break;
        const int end = matchingSpanEnd(sanitized, at);
        if (end < 0) break; // not well-formed: leave the rest to revealSpoilers' treatment
        out += QStringView(sanitized).mid(pos, at - pos);
        const QStringView inner =
            QStringView(sanitized).mid(at + opener.size(), end - at - opener.size() - 7);
        const int blocks = std::clamp(visibleLength(inner), 3, 24);
        const QString bar(blocks, QChar(0x2588));
        const QString style = QStringLiteral("color:#4e5058; text-decoration:none;");
        if (insideAnchor(sanitized, at)) {
            out += QStringLiteral("<span style=\"%1\">%2</span>").arg(style, bar);
        } else {
            out += QStringLiteral("<a href=\"%1\" style=\"%2\">%3</a>")
                       .arg(QLatin1String(kSpoilerHref), style, bar);
        }
        pos = end;
    }
    out += QStringView(sanitized).mid(pos);
    // Anything left that could not be matched is still not shown openly.
    out.replace(opener, QStringLiteral("<span style=\"color:#4e5058; background-color:#4e5058;\">"));
    return out;
}

QString revealSpoilers(const QString& sanitized)
{
    const QLatin1String opener(kSpoilerOpen);
    if (!sanitized.contains(opener)) return sanitized;
    QString out = sanitized;
    out.replace(opener, QStringLiteral("<span style=\"background-color:#3f4147;\">"));
    return out;
}

} // namespace bsfchat::client
