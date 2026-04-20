#include "util/MarkdownParser.h"

#include <QRegularExpression>

QString MarkdownParser::toHtml(const QString& markdown)
{
    QString result = markdown.toHtmlEscaped();
    result = processCodeBlocks(result);
    result = processInlineCode(result);
    result = processBold(result);
    result = processItalic(result);
    result = processStrikethrough(result);
    result = processLinks(result);
    result = processChannelMentions(result);
    result = processBlockQuotes(result);
    // Convert newlines to <br>
    result.replace('\n', "<br>");
    return result;
}

QString MarkdownParser::processCodeBlocks(const QString& text)
{
    static QRegularExpression re("```(?:[a-zA-Z]*\\n)?([\\s\\S]*?)```",
                                  QRegularExpression::MultilineOption);
    QString result = text;
    result.replace(re,
        "<pre style=\"background-color:#1e1f22; padding:8px 12px; border-radius:4px; "
        "font-family:monospace; margin:4px 0;\"><code>\\1</code></pre>");
    return result;
}

QString MarkdownParser::processInlineCode(const QString& text)
{
    static QRegularExpression re("`([^`]+)`");
    QString result = text;
    result.replace(re,
        "<code style=\"background-color:#1e1f22; padding:2px 6px; border-radius:3px; "
        "font-family:monospace;\">\\1</code>");
    return result;
}

QString MarkdownParser::processBold(const QString& text)
{
    static QRegularExpression re("\\*\\*(.+?)\\*\\*");
    QString result = text;
    result.replace(re, "<b>\\1</b>");
    return result;
}

QString MarkdownParser::processItalic(const QString& text)
{
    // Match *text* but not **text**
    static QRegularExpression re("(?<!\\*)\\*(?!\\*)(.+?)(?<!\\*)\\*(?!\\*)");
    QString result = text;
    result.replace(re, "<i>\\1</i>");
    return result;
}

QString MarkdownParser::processStrikethrough(const QString& text)
{
    static QRegularExpression re("~~(.+?)~~");
    QString result = text;
    result.replace(re, "<s>\\1</s>");
    return result;
}

QString MarkdownParser::processLinks(const QString& text)
{
    static QRegularExpression re("\\[([^\\]]+)\\]\\(([^)]+)\\)");
    QString result = text;
    result.replace(re,
        "<a href=\"\\2\" style=\"color:#5865f2; text-decoration:none;\">\\1</a>");
    return result;
}

QString MarkdownParser::processChannelMentions(const QString& text)
{
    // Match `#word` preceded by start-of-string, whitespace, or one of a few
    // common delimiters. The negative lookahead on `(` avoids eating Markdown
    // link anchors that already got rewritten (e.g. `<a href="bsfchat://...">`).
    // Channel names allow letters, digits, hyphen, and underscore — matches
    // the server-side room-name charset.
    static QRegularExpression re(R"((^|[\s>("',.!?])#([a-zA-Z0-9][a-zA-Z0-9_-]{0,63}))");
    QString result = text;
    result.replace(re,
        "\\1<a href=\"bsfchat://channel/\\2\" "
        "style=\"color:#5865f2; text-decoration:none; font-weight:bold;\">#\\2</a>");
    return result;
}

QString MarkdownParser::processBlockQuotes(const QString& text)
{
    static QRegularExpression re("(?:^|\\n)&gt; (.+)");
    QString result = text;
    result.replace(re,
        "<blockquote style=\"border-left:3px solid #4e5058; padding-left:8px; "
        "margin:4px 0; color:#b5bac1;\">\\1</blockquote>");
    return result;
}
