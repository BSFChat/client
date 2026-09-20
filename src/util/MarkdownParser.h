#pragma once

#include <QString>

class MarkdownParser {
public:
    // Convert simple markdown to HTML suitable for Qt Rich Text
    static QString toHtml(const QString& markdown);

    // Promote a body to HTML WITHOUT interpreting markdown: escape it, then
    // keep its hard line breaks. For the bodies markdown must not touch (an
    // m.notice or m.emote, where a bot's literal asterisks are literal), and
    // for anywhere else plain text is handed to a rich-text renderer.
    //
    // The line breaks are the point. Escaping alone was what the mention
    // renderer and the "(edited)" badge used to do, and HTML folds a newline
    // into a space — so a three-line message collapsed to one the moment
    // anything promoted it. toHtml() has always ended with the same
    // conversion; this is the other half of that pair.
    static QString plainToHtml(const QString& text);

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
