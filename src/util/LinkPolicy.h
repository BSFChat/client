#pragma once

// Which links a click may hand to the operating system — the pure half of
// util/LinkOpener, kept free of QObject so it can be tested directly.
//
// A link in a message is chosen by its sender. Handing every href to
// Qt.openUrlExternally meant `file://`, `smb://` and UNC paths (code execution
// or an NTLM hash leak on Windows), `javascript:` and any custom protocol
// handler the viewer happens to have installed were all one click away, and
// an anchor reading "https://bank.example" could point anywhere at all.

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace bsfchat::client::links {

enum class Action {
    Open,     // http(s) or mailto: hand it to the browser
    Confirm,  // http(s) whose visible text names a different host: ask first
    Refuse,   // any other scheme, or not a URL at all
    Internal, // bsfchat:// — the call site routes these itself
};

struct Decision {
    Action action = Action::Refuse;
    QUrl url;
    QString host;   // the real destination host, for Confirm
    QString reason; // for Refuse
};

// Undo the handful of escapes HtmlSanitizer / toHtmlEscaped produce, plus
// numeric references, so a string read out of our own markup compares equal
// to what Qt reports on a click.
inline QString unescapeHtml(const QString& s)
{
    if (!s.contains(QLatin1Char('&'))) return s;
    static const QRegularExpression ref(QStringLiteral("&(#[0-9]{1,7}|#[xX][0-9a-fA-F]{1,6}|[a-zA-Z]+);"));
    QString out;
    qsizetype last = 0;
    auto it = ref.globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next();
        out += QStringView(s).mid(last, m.capturedStart() - last);
        const QString name = m.captured(1);
        QString rep;
        if (name.startsWith(QLatin1Char('#'))) {
            bool ok = false;
            const uint cp = name.startsWith(QLatin1String("#x"), Qt::CaseInsensitive)
                ? name.mid(2).toUInt(&ok, 16) : name.mid(1).toUInt(&ok, 10);
            if (ok && cp > 0 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF)) {
                const char32_t c = cp;
                rep = QString::fromUcs4(&c, 1);
            }
        } else if (name == QLatin1String("amp")) rep = QStringLiteral("&");
        else if (name == QLatin1String("lt")) rep = QStringLiteral("<");
        else if (name == QLatin1String("gt")) rep = QStringLiteral(">");
        else if (name == QLatin1String("quot")) rep = QStringLiteral("\"");
        else if (name == QLatin1String("apos")) rep = QStringLiteral("'");
        else if (name == QLatin1String("nbsp")) rep = QString(QChar(0xA0));
        out += rep.isNull() ? m.captured(0) : rep;
        last = m.capturedEnd();
    }
    out += QStringView(s).mid(last);
    return out;
}

// The visible text of every <a> in `html` whose href is `href`.
inline QStringList anchorTextsFor(const QString& html, const QString& href)
{
    QStringList out;
    if (html.isEmpty() || href.isEmpty()) return out;
    static const QRegularExpression anchor(
        QStringLiteral("<a\\b[^>]*?\\bhref\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)')[^>]*>(.*?)</a\\s*>"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
    auto it = anchor.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString raw = m.capturedStart(1) >= 0 ? m.captured(1) : m.captured(2);
        if (unescapeHtml(raw) != href) continue;
        QString text = m.captured(3);
        text.remove(tags);
        out << unescapeHtml(text).trimmed();
    }
    return out;
}

// The host a piece of link TEXT presents itself as, or "" when the text does
// not look like a URL ("click here", "the docs") — in which case there is
// nothing it could be lying about.
inline QString hostShownBy(const QString& text)
{
    static const QRegularExpression urlish(
        QStringLiteral("^(?:[a-z][a-z0-9+.-]*://)?(?:[^\\s/@]+@)?"
                       "[^\\s/?#@:.]+(?:\\.[^\\s/?#@:.]+)+(?::[0-9]{1,5})?(?:[/?#]\\S*)?$"),
        QRegularExpression::CaseInsensitiveOption);
    const QString t = text.trimmed();
    if (t.isEmpty() || !urlish.match(t).hasMatch()) return {};
    // "www" or a bare dotted word with no TLD-ish tail is still a host to Qt;
    // what matters is that the reader would take it for one.
    return QUrl::fromUserInput(t).host().toLower();
}

inline QString normalisedHost(QString host)
{
    host = host.toLower();
    if (host.endsWith(QLatin1Char('.'))) host.chop(1);
    if (host.startsWith(QLatin1String("www."))) host = host.mid(4);
    return host;
}

inline Decision decide(const QString& href, const QStringList& anchorTexts = {})
{
    Decision d;
    const QString h = href.trimmed();
    for (const QChar c : h) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7F) {
            d.reason = QStringLiteral("the link contains control characters");
            return d;
        }
    }
    const QUrl url(h, QUrl::StrictMode);
    d.url = url;
    if (h.isEmpty() || !url.isValid() || url.scheme().isEmpty()) {
        d.reason = QStringLiteral("it is not a web link");
        return d;
    }
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("bsfchat")) {
        d.action = Action::Internal;
        return d;
    }
    if (scheme == QLatin1String("mailto")) {
        d.action = Action::Open;
        return d;
    }
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) {
        d.reason = QStringLiteral("BSFChat only opens web (http/https) and mailto links, not %1:")
                       .arg(scheme);
        return d;
    }
    if (url.host().isEmpty()) {
        d.reason = QStringLiteral("the link has no host");
        return d;
    }
    d.host = url.host();
    const QString real = normalisedHost(url.host());
    for (const QString& text : anchorTexts) {
        const QString shown = hostShownBy(text);
        if (!shown.isEmpty() && normalisedHost(shown) != real) {
            d.action = Action::Confirm;
            return d;
        }
    }
    d.action = Action::Open;
    return d;
}

} // namespace bsfchat::client::links
