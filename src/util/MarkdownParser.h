#pragma once

#include <QString>

class MarkdownParser {
public:
    // Convert simple markdown to HTML suitable for Qt Rich Text
    static QString toHtml(const QString& markdown);

private:
    static QString processCodeBlocks(const QString& text);
    static QString processInlineCode(const QString& text);
    static QString processBold(const QString& text);
    static QString processItalic(const QString& text);
    static QString processStrikethrough(const QString& text);
    static QString processLinks(const QString& text);
    static QString processBlockQuotes(const QString& text);
    // Turn `#channel-name` tokens into clickable bsfchat://channel links so
    // MessageBubble can intercept and switch channel. Runs AFTER explicit
    // Markdown links so `[#foo](url)` isn't double-linked.
    static QString processChannelMentions(const QString& text);
};
