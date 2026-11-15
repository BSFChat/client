#pragma once

// sed-style corrections typed into the composer: `s/teh/the/` edits your most
// recent message instead of being posted.
//
// Pure logic, no model and no network, so the parser and the substitution are
// testable on their own (tests/test_sed_edit.cpp). ServerConnection finds the
// message and sends the result through the ordinary m.replace edit path.
//
// The rules, all deliberately strict so ordinary chat never trips them:
//
//   * The whole message is `s/PATTERN/REPLACEMENT/FLAGS` on ONE line, starting
//     at the very first character. A leading space or a leading backslash
//     (`\s/...`) means "send this literally" — the composer handles those
//     before it gets here.
//   * The closing `/` is required; FLAGS is any of `g` (every occurrence) and
//     `i` (case-insensitive), each at most once. Anything else after the
//     third delimiter makes it not a sed command at all.
//   * `\/` is a literal slash in either half. Every other backslash sequence
//     is passed through to the regex (pattern) or the expander (replacement).
//   * PATTERN is a QRegularExpression (PCRE2), not a substring: that is what a
//     sed user expects `.` and `\d` to mean. It is capped at 200 characters,
//     and the body it runs on at kMaxBodyChars, so a catastrophic pattern is
//     bounded by PCRE2's own match limit on a bounded input. A pattern that
//     hits that limit simply reports no match.
//   * REPLACEMENT: `&` is the whole match, `\0`..`\9` a capture group, `\&`
//     and `\\` literal, any other `\c` is `c`.

#include <QRegularExpression>
#include <QString>

#include <optional>

namespace bsfchat::client::sed {

inline constexpr int kMaxPatternChars = 200;
inline constexpr int kMaxBodyChars = 64 * 1024;

struct Command {
    QString pattern;      // regex source, `\/` already turned into `/`
    QString replacement;  // raw replacement, `\/` already turned into `/`
    bool global = false;
    bool caseInsensitive = false;
};

enum class ParseStatus {
    NotSed,   // ordinary text — send it as a message
    Invalid,  // looks like a sed command but cannot run; `error` says why
    Ok,
};

struct ParseResult {
    ParseStatus status = ParseStatus::NotSed;
    Command command;
    QString error;
};

// Split one `/`-delimited field starting at `pos`. Returns false if no
// unescaped closing `/` is found. On success `pos` is just past the delimiter.
inline bool takeField(const QString& s, qsizetype& pos, QString& out)
{
    out.clear();
    while (pos < s.size()) {
        const QChar c = s.at(pos);
        if (c == u'\\' && pos + 1 < s.size()) {
            const QChar n = s.at(pos + 1);
            if (n == u'/') {
                out += u'/';
            } else {
                out += c;
                out += n;
            }
            pos += 2;
            continue;
        }
        if (c == u'/') {
            ++pos;
            return true;
        }
        out += c;
        ++pos;
    }
    return false;
}

inline ParseResult parse(const QString& text)
{
    ParseResult r;
    if (!text.startsWith(QLatin1String("s/"))) return r;
    // Other lines mean this is a message that happens to start with s/.
    if (text.contains(u'\n') || text.contains(u'\r')) return r;

    qsizetype pos = 2;
    QString pattern, replacement;
    if (!takeField(text, pos, pattern)) return r;
    if (!takeField(text, pos, replacement)) return r;

    Command cmd;
    for (qsizetype i = pos; i < text.size(); ++i) {
        const QChar f = text.at(i);
        if (f == u'g' && !cmd.global) {
            cmd.global = true;
        } else if (f == u'i' && !cmd.caseInsensitive) {
            cmd.caseInsensitive = true;
        } else {
            return r;  // trailing junk: not ours
        }
    }

    r.status = ParseStatus::Invalid;
    if (pattern.isEmpty()) {
        r.error = QStringLiteral("Nothing to search for — the pattern is empty.");
        return r;
    }
    if (pattern.size() > kMaxPatternChars) {
        r.error = QStringLiteral("That pattern is too long (over %1 characters).")
                      .arg(kMaxPatternChars);
        return r;
    }
    QRegularExpression re(pattern);
    if (!re.isValid()) {
        r.error = QStringLiteral("That isn't a valid pattern: %1").arg(re.errorString());
        return r;
    }

    cmd.pattern = pattern;
    cmd.replacement = replacement;
    r.status = ParseStatus::Ok;
    r.command = cmd;
    r.error.clear();
    return r;
}

// Expand `&`, `\N`, `\&`, `\\` against one match.
inline QString expandReplacement(const QString& repl, const QRegularExpressionMatch& m)
{
    QString out;
    out.reserve(repl.size());
    for (qsizetype i = 0; i < repl.size(); ++i) {
        const QChar c = repl.at(i);
        if (c == u'&') {
            out += m.captured(0);
        } else if (c == u'\\' && i + 1 < repl.size()) {
            const QChar n = repl.at(++i);
            if (n.isDigit() && n.unicode() <= u'9') {
                const int group = n.unicode() - u'0';
                if (group <= m.lastCapturedIndex()) out += m.captured(group);
            } else {
                out += n;  // \& \\ and anything else: the character itself
            }
        } else {
            out += c;
        }
    }
    return out;
}

// Apply `cmd` to `body`. nullopt when nothing matched (or the body is too
// large to run a user regex over).
inline std::optional<QString> apply(const Command& cmd, const QString& body)
{
    if (body.size() > kMaxBodyChars) return std::nullopt;
    QRegularExpression::PatternOptions opts = QRegularExpression::NoPatternOption;
    if (cmd.caseInsensitive) opts |= QRegularExpression::CaseInsensitiveOption;
    const QRegularExpression re(cmd.pattern, opts);
    if (!re.isValid()) return std::nullopt;

    QString out;
    qsizetype last = 0;
    bool matched = false;
    auto it = re.globalMatch(body);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        matched = true;
        out += body.mid(last, m.capturedStart(0) - last);
        out += expandReplacement(cmd.replacement, m);
        last = m.capturedEnd(0);
        if (!cmd.global) break;
    }
    if (!matched) return std::nullopt;
    out += body.mid(last);
    return out;
}

} // namespace bsfchat::client::sed
