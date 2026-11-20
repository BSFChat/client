#pragma once

#include <QString>
#include <QTranslator>

// English plural forms for tr()/qsTr() strings written with "(s)".
//
// The app ships no translations, so nothing resolved numerus forms and the
// UI said "3 server(s)", "1 member(s)" literally (audit 3). Rather than a
// .ts/.qm round trip for English alone, this translator answers any string
// that has a count (n >= 0) and a "(s)" marker: the marker becomes "" for 1
// and "s" otherwise, with the few irregular words spelled out. Qt then
// substitutes %n as usual. Everything else returns empty, i.e. "not mine",
// so a real translator installed later takes precedence for its strings.
namespace bsfchat::client {

inline QString englishPlural(const QString& source, int n)
{
    if (n < 0 || !source.contains(QLatin1String("(s)"))) return {};
    QString out = source;
    const bool one = (n == 1);
    // Words whose plural is not "+s".
    static const struct { const char* stem; const char* plural; } kIrregular[] = {
        {"reply(s)", "replies"},
        {"entry(s)", "entries"},
        {"category(s)", "categories"},
        {"identity(s)", "identities"},
    };
    for (const auto& w : kIrregular) {
        const QString stem = QLatin1String(w.stem);
        out.replace(stem, one ? stem.left(stem.size() - 3) : QLatin1String(w.plural));
    }
    out.replace(QLatin1String("(s)"), one ? QString() : QStringLiteral("s"));
    return out;
}

class EnglishPluralTranslator : public QTranslator
{
public:
    using QTranslator::QTranslator;
    QString translate(const char*, const char* sourceText, const char* = nullptr,
                      int n = -1) const override
    {
        if (!sourceText) return {};
        return englishPlural(QString::fromUtf8(sourceText), n);
    }
    bool isEmpty() const override { return false; }
};

} // namespace bsfchat::client
