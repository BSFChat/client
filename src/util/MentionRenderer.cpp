#include "util/MentionRenderer.h"

#include <QRegularExpression>
#include <QStringView>
#include <QUrl>

#include <algorithm>

namespace bsfchat::client {
namespace {

// Inline styles, hard-coded to match the rest of the rendering pipeline
// (MarkdownParser does the same for links, code and quotes — the HTML goes to
// Qt's RichText engine, which can't see QML's Theme singleton).
//
// Three visually distinct treatments, because the reader has to triage at a
// glance: someone else was pinged / *you* were pinged / everyone was pinged.
constexpr auto kOtherMentionStyle =
    "color:#7aa2ff; background-color:#232a3d; "
    "text-decoration:none; font-weight:600;";
constexpr auto kSelfMentionStyle =
    "color:#ffd166; background-color:#40361d; "
    "text-decoration:none; font-weight:700;";
constexpr auto kRoomMentionStyle =
    "color:#ff9f6e; background-color:#40291d; font-weight:700;";

// Token characters for the boundary test. A mention must not be recognised
// inside a longer word, or "@Bob" lights up in the middle of "@Bobby" and the
// wrong user appears to have been pinged.
bool isTokenChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

// The one place a user id reaches markup. Percent-encoded, so neither a quote
// nor a bracket in a hostile id can terminate the attribute.
QString hrefFor(const QString& userId)
{
    return QStringLiteral("bsfchat://user/")
           + QString::fromUtf8(QUrl::toPercentEncoding(userId));
}

// `label` MUST already be HTML-escaped: it is emitted as element text.
QString anchorFor(const MentionTarget& target, const QString& label)
{
    return QStringLiteral("<a href=\"") + hrefFor(target.userId)
           + QStringLiteral("\" style=\"")
           + QLatin1String(target.isSelf ? kSelfMentionStyle
                                         : kOtherMentionStyle)
           + QStringLiteral("\">") + label + QStringLiteral("</a>");
}

QString roomSpan()
{
    return QStringLiteral("<span style=\"") + QLatin1String(kRoomMentionStyle)
           + QStringLiteral("\">@room</span>");
}

// Canonical rendered label for a target: '@' + the locally-resolved display
// name, escaped. Deliberately NOT the label the sender chose — a sender can
// write anything between their anchor tags, including a different person's
// name, and this is the impersonation vector.
QString labelFor(const MentionTarget& target)
{
    const QString name = target.displayName.isEmpty() ? target.userId
                                                      : target.displayName;
    return mentionToken(name).toHtmlEscaped();
}

// The element name of a tag string like "</a>" or "<a href=...>", lowercased.
QString tagName(QStringView tag)
{
    int i = 1; // past '<'
    if (i < tag.size() && tag[i] == QLatin1Char('/')) ++i;
    const int start = i;
    while (i < tag.size() && (tag[i].isLetterOrNumber())) ++i;
    return tag.sliced(start, i - start).toString().toLower();
}

struct Needle {
    QString text; // escaped, '@'-prefixed
    int target;   // index into `targets`, or -1 for @room
};

QVector<Needle> buildNeedles(const QVector<MentionTarget>& targets,
                             bool roomMention)
{
    QVector<Needle> needles;
    auto add = [&needles](const QString& raw, int target) {
        if (raw.isEmpty()) return;
        // A user id already carries its own '@'; a display name does not.
        QString token = raw.startsWith(QLatin1Char('@')) ? raw
                                                         : QLatin1Char('@') + raw;
        const QString escaped = token.toHtmlEscaped();
        for (const auto& n : needles) {
            if (n.text.compare(escaped, Qt::CaseInsensitive) == 0) return;
        }
        needles.append({escaped, target});
    };

    // @room first so it wins a length tie against a user literally named
    // "room" (stable_sort below preserves this).
    if (roomMention) needles.append({QStringLiteral("@room"), -1});

    for (int i = 0; i < targets.size(); ++i) {
        const auto& t = targets[i];
        // The composer's whitespace-stripped form is the one actually written
        // into bodies; the others are fallbacks for mentions typed or built by
        // other clients.
        add(mentionToken(t.displayName).mid(1), i);
        add(t.displayName, i);
        add(t.userId, i);
    }

    // Longest first: with both "Bob" and "Bob Smith" in the room, matching the
    // short one first would leave " Smith" dangling as plain text.
    std::stable_sort(needles.begin(), needles.end(),
                     [](const Needle& a, const Needle& b) {
                         return a.text.size() > b.text.size();
                     });
    return needles;
}

// Rewrite one text node. `segment` is escaped HTML with no tags in it.
QString rewriteSegment(const QString& segment, const QVector<Needle>& needles,
                       const QVector<MentionTarget>& targets)
{
    if (!segment.contains(QLatin1Char('@'))) return segment;

    QString out;
    out.reserve(segment.size() + 64);
    int i = 0;
    while (i < segment.size()) {
        if (segment[i] != QLatin1Char('@')
            || (i > 0 && isTokenChar(segment[i - 1]))) {
            // Not a candidate: either not an '@', or it sits inside a word
            // (an email address, "foo@bar").
            out += segment[i];
            ++i;
            continue;
        }
        const QStringView rest = QStringView(segment).sliced(i);
        bool matched = false;
        for (const auto& n : needles) {
            if (!rest.startsWith(n.text, Qt::CaseInsensitive)) continue;
            const int end = i + n.text.size();
            if (end < segment.size() && isTokenChar(segment[end])) continue;
            out += (n.target < 0) ? roomSpan()
                                  : anchorFor(targets[n.target],
                                              labelFor(targets[n.target]));
            i = end;
            matched = true;
            break;
        }
        if (!matched) {
            out += segment[i];
            ++i;
        }
    }
    return out;
}

// Replace every <a href="bsfchat://user/...">…</a> with our own markup, so the
// sender controls neither the styling nor the label.
QString canonicaliseUserAnchors(const QString& html,
                                const QVector<MentionTarget>& targets)
{
    static const QRegularExpression re(
        QStringLiteral("<a\\b[^>]*href=\"bsfchat://user/([^\"]*)\"[^>]*>(.*?)</a>"),
        QRegularExpression::CaseInsensitiveOption);
    if (!html.contains(QStringLiteral("bsfchat://user/"), Qt::CaseInsensitive))
        return html;

    QString out;
    out.reserve(html.size());
    int last = 0;
    auto it = re.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        out += html.mid(last, m.capturedStart() - last);
        const QString userId = QUrl::fromPercentEncoding(
            m.captured(1).toUtf8());
        // Prefer the locally-known target so the label reflects the room's
        // current name for that user rather than the sender's snapshot.
        const MentionTarget* known = nullptr;
        for (const auto& t : targets) {
            if (t.userId == userId) { known = &t; break; }
        }
        if (known) {
            out += anchorFor(*known, labelFor(*known));
        } else {
            // Anchored at someone the event's m.mentions never listed. Keep
            // the link (it is still a valid profile target) but render it in
            // the neutral style with the id as its own label — an unvouched
            // anchor must not be able to claim to be someone else.
            MentionTarget synthetic{userId, userId, false};
            out += anchorFor(synthetic, userId.toHtmlEscaped());
        }
        last = m.capturedEnd();
    }
    out += html.mid(last);
    return out;
}

} // namespace

QString mentionToken(const QString& displayName)
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    QString stripped = displayName;
    stripped.remove(ws);
    return QLatin1Char('@') + stripped;
}

QString renderMentions(const QString& html,
                       const QVector<MentionTarget>& targets,
                       bool roomMention)
{
    if (html.isEmpty()) return html;
    if (targets.isEmpty() && !roomMention) return html;

    const QString normalised = canonicaliseUserAnchors(html, targets);
    const QVector<Needle> needles = buildNeedles(targets, roomMention);
    if (needles.isEmpty()) return normalised;

    QString out;
    out.reserve(normalised.size() + 64);
    int i = 0;
    // Depth of elements we must not rewrite inside. <a> because nesting
    // anchors produces broken markup (and our own composer already anchors
    // its mentions); <code>/<pre> because a mention inside a code span is
    // being quoted, not addressed.
    int inertDepth = 0;
    while (i < normalised.size()) {
        if (normalised[i] == QLatin1Char('<')) {
            const int close = normalised.indexOf(QLatin1Char('>'), i);
            if (close < 0) {
                // Unterminated '<'. Emit the remainder verbatim rather than
                // guessing — it is already-escaped text by contract, so the
                // only way to get here is a malformed formatted_body.
                out += normalised.mid(i);
                break;
            }
            const QStringView tag = QStringView(normalised).sliced(i, close - i + 1);
            out += tag;
            const QString name = tagName(tag);
            if (name == QLatin1String("a") || name == QLatin1String("code")
                || name == QLatin1String("pre")) {
                if (tag.startsWith(QLatin1String("</"))) {
                    if (inertDepth > 0) --inertDepth;
                } else if (!tag.endsWith(QLatin1String("/>"))) {
                    ++inertDepth;
                }
            }
            i = close + 1;
            continue;
        }
        int next = normalised.indexOf(QLatin1Char('<'), i);
        if (next < 0) next = normalised.size();
        const QString segment = normalised.mid(i, next - i);
        out += (inertDepth > 0) ? segment
                                : rewriteSegment(segment, needles, targets);
        i = next;
    }
    return out;
}

} // namespace bsfchat::client
