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
};
