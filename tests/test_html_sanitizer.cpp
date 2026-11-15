// The formatted_body sanitiser (Q-S2).
//
// A sender's formatted_body is rendered as rich text, so whatever survives
// sanitizeHtml() is markup every viewer's client interprets: a remote <img>
// is an IP beacon and a decode bomb, a file:// or javascript: href is one
// click from a local exploit. These cases are the bypasses a hand-written
// filter usually misses — casing, unquoted and duplicated attributes,
// comments, entity-encoded schemes, unterminated tags — plus the round trip
// that keeps this client's own markdown looking the way it did.

#include "util/HtmlSanitizer.h"

#include <QRegularExpression>
#include <QTest>

using bsfchat::client::isAllowedLinkHref;
using bsfchat::client::sanitizeHtml;

namespace {

// Every tag name in `html`, lowercased.
QStringList tagNames(const QString& html)
{
    static const QRegularExpression tag(QStringLiteral("</?([a-zA-Z0-9]+)"));
    QStringList out;
    for (auto it = tag.globalMatch(html); it.hasNext();)
        out << it.next().captured(1).toLower();
    return out;
}

// Every attribute in `html`, as name=value.
QStringList attributes(const QString& html)
{
    static const QRegularExpression attr(QStringLiteral("<[a-z0-9]+((?:\\s+[a-z-]+=\"[^\"]*\")*)\\s*>"));
    static const QRegularExpression one(QStringLiteral("([a-z-]+)=\"([^\"]*)\""));
    QStringList out;
    for (auto it = attr.globalMatch(html); it.hasNext();) {
        const QString attrs = it.next().captured(1);
        for (auto jt = one.globalMatch(attrs); jt.hasNext();) {
            const auto m = jt.next();
            out << m.captured(1) + QLatin1Char('=') + m.captured(2);
        }
    }
    return out;
}

void verifyNoActiveContent(const QString& out)
{
    const QString lower = out.toLower();
    for (const char* bad : {"<img", "<script", "<style", "<iframe", "<object", "<embed",
                            "<svg", "<math", "<link", "<meta", "<base", "<form", "<input",
                            "<video", "<audio", "<font", "<div", "onerror", "onload",
                            "onclick", "src=", "javascript:", "vbscript:", "data:",
                            "file:", "width=", "height=", "<!--"}) {
        QVERIFY2(!lower.contains(QLatin1String(bad)),
                 qPrintable(QStringLiteral("'%1' survived: %2").arg(QLatin1String(bad), out)));
    }
    static const QStringList kAllowed = {
        "b", "strong", "i", "em", "u", "s", "code", "pre", "blockquote", "br", "p", "ul",
        "ol", "li", "a", "span", "h1", "h2", "h3", "h4", "h5", "h6", "sub", "sup", "hr",
        "table", "thead", "tbody", "tr", "td", "th",
    };
    for (const QString& t : tagNames(out))
        QVERIFY2(kAllowed.contains(t), qPrintable(QStringLiteral("tag <%1> survived: %2").arg(t, out)));
    for (const QString& a : attributes(out)) {
        QVERIFY2(a.startsWith(QLatin1String("href=")) || a.startsWith(QLatin1String("style=")),
                 qPrintable(QStringLiteral("attribute %1 survived: %2").arg(a, out)));
    }
}

} // namespace

class TestHtmlSanitizer : public QObject {
    Q_OBJECT

private slots:
    void plainFormattingSurvives()
    {
        QCOMPARE(sanitizeHtml("<b>bold</b> <i>it</i> <u>u</u> <s>s</s>"),
                 QStringLiteral("<b>bold</b> <i>it</i> <u>u</u> <s>s</s>"));
        QCOMPARE(sanitizeHtml("<strong>a</strong><em>b</em>"),
                 QStringLiteral("<strong>a</strong><em>b</em>"));
        QCOMPARE(sanitizeHtml("<del>x</del><strike>y</strike>"), QStringLiteral("<s>x</s><s>y</s>"));
        QCOMPARE(sanitizeHtml("a<br>b<br/>c<BR />d"), QStringLiteral("a<br>b<br>c<br>d"));
        QCOMPARE(sanitizeHtml("<ul><li>one</li><li>two</li></ul>"),
                 QStringLiteral("<ul><li>one</li><li>two</li></ul>"));
        QCOMPARE(sanitizeHtml("<h1>T</h1><sub>1</sub><sup>2</sup><hr>"),
                 QStringLiteral("<h1>T</h1><sub>1</sub><sup>2</sup><hr>"));
        QCOMPARE(sanitizeHtml(""), QString());
        QCOMPARE(sanitizeHtml("just text"), QStringLiteral("just text"));
    }

    void ourOwnMarkdownLooksTheSameAfterTheRoundTrip()
    {
        // MarkdownParser's output, as the server hands it back.
        const QString code = "<code style=\"background-color:#1e1f22; padding:2px 6px; "
                             "border-radius:3px; font-family:monospace;\">x</code>";
        QCOMPARE(sanitizeHtml(code), code);
        const QString link = "<a href=\"https://example.com/a?b=1&amp;c=2\" "
                             "style=\"color:#5865f2; text-decoration:none;\">ex</a>";
        QCOMPARE(sanitizeHtml(link), link);
        const QString channel = "<a href=\"bsfchat://channel/general\" style=\"color:#5865f2; "
                                "text-decoration:none; font-weight:bold;\">#general</a>";
        QCOMPARE(sanitizeHtml(channel), channel);
        const QString pre = "<pre style=\"background-color:#1e1f22; padding:8px 12px; "
                            "border-radius:4px; font-family:monospace; margin:4px 0;\">"
                            "<code>a &lt; b</code></pre>";
        QCOMPARE(sanitizeHtml(pre), pre);
        // A mention anchor keeps its target for MentionRenderer to restyle.
        QVERIFY(sanitizeHtml("<a href=\"bsfchat://user/%40carol%3As\">@Carol</a>")
                    .startsWith("<a href=\"bsfchat://user/%40carol%3As\""));
    }

    void aSendersStyleIsReplacedNotKept()
    {
        const QString out = sanitizeHtml(
            "<code style=\"font-size:900px\">x</code><span style=\"color:red\">y</span>");
        QVERIFY(!out.contains("900px"));
        QVERIFY(!out.contains("color:red"));
        QVERIFY(out.contains("<span>y</span>"));
    }

    void imagesNeverSurviveAndBecomeTheirAltText()
    {
        QCOMPARE(sanitizeHtml("a<img src=\"https://tracker.example/p.png\" width=4000>b"),
                 QStringLiteral("ab"));
        QCOMPARE(sanitizeHtml("<img alt=\"a cat\" src=x>"), QStringLiteral("a cat"));
        QCOMPARE(sanitizeHtml("<IMG SRC=x ALT='<b>bold</b>'>"),
                 QStringLiteral("&lt;b&gt;bold&lt;/b&gt;"));
        QCOMPARE(sanitizeHtml("<img/src=x/onerror=alert(1)>"), QString());
        QCOMPARE(sanitizeHtml("<img src=mxc://s/abc>"), QString());
        verifyNoActiveContent(sanitizeHtml("<p><img src=\"//evil/x\"></p>"));
    }

    void dangerousElementsLoseTheirContentToo()
    {
        QCOMPARE(sanitizeHtml("a<script>alert(1)</script>b"), QStringLiteral("ab"));
        QCOMPARE(sanitizeHtml("a<SCRIPT type=x>alert(1)</ScRiPt >b"), QStringLiteral("ab"));
        QCOMPARE(sanitizeHtml("a<style>*{display:none}</style>b"), QStringLiteral("ab"));
        QCOMPARE(sanitizeHtml("a<iframe src=//x></iframe>b"), QStringLiteral("ab"));
        QCOMPARE(sanitizeHtml("a<svg><image href=//x /></svg>b"), QStringLiteral("ab"));
        QCOMPARE(sanitizeHtml("a<object data=x>fallback</object>b"), QStringLiteral("ab"));
        // A reply fallback is quoted text the client renders itself.
        QCOMPARE(sanitizeHtml("<mx-reply><blockquote>old</blockquote></mx-reply>new"),
                 QStringLiteral("new"));
        // Never closed: everything after it goes, rather than leaking out.
        QCOMPARE(sanitizeHtml("a<script>alert(1)"), QStringLiteral("a"));
        // `</scriptx>` is not the end tag.
        QCOMPARE(sanitizeHtml("<script>x</scriptx>y</script>z"), QStringLiteral("z"));
    }

    void unknownElementsKeepTheirTextButNotTheirTag()
    {
        QCOMPARE(sanitizeHtml("<font color=red size=7>big</font>"), QStringLiteral("big"));
        QCOMPARE(sanitizeHtml("<div align=center>x</div>"), QStringLiteral("x"));
        QCOMPARE(sanitizeHtml("<marquee>x</marquee>"), QStringLiteral("x"));
        verifyNoActiveContent(sanitizeHtml("<form action=x><input value=y></form>"));
    }

    void linksAreLimitedToSafeSchemes()
    {
        const QString style = " style=\"color:#5865f2; text-decoration:none;\"";
        QCOMPARE(sanitizeHtml("<a href=\"https://ok.example/\">x</a>"),
                 "<a href=\"https://ok.example/\"" + style + ">x</a>");
        QCOMPARE(sanitizeHtml("<a href=mailto:a@b.c>m</a>"),
                 "<a href=\"mailto:a@b.c\"" + style + ">m</a>");
        // Everything else keeps its text and loses the anchor.
        for (const char* href : {"javascript:alert(1)", "JaVaScRiPt:alert(1)",
                                 " javascript:alert(1)", "java\tscript:alert(1)",
                                 "java&#x09;script:alert(1)", "&#106;avascript:alert(1)",
                                 "&#x6A;avascript:alert(1)", "&#0000106avascript:alert(1)",
                                 "javascript&colon;alert(1)", "vbscript:x", "data:text/html,x",
                                 "file:///C:/Windows/System32/calc.exe", "FILE://server/share",
                                 "\\\\server\\share\\x.exe", "smb://server/share",
                                 "ms-msdt:/id x", "search-ms:query=x", "//evil.example/",
                                 "/relative", "x", "", "steam://run/1"}) {
            const QString in = QStringLiteral("<a href=\"%1\">t</a>").arg(QString::fromUtf8(href));
            QCOMPARE(sanitizeHtml(in), QStringLiteral("t"));
        }
        // Unquoted and single-quoted forms are parsed, not passed through.
        QCOMPARE(sanitizeHtml("<a href=javascript:alert(1)>t</a>"), QStringLiteral("t"));
        QCOMPARE(sanitizeHtml("<a href='file:///etc/passwd'>t</a>"), QStringLiteral("t"));
        // The first href wins; a second cannot sneak in behind a good one.
        QCOMPARE(sanitizeHtml("<a href=\"https://ok/\" href=\"javascript:x\">t</a>"),
                 "<a href=\"https://ok/\"" + style + ">t</a>");
        // Event handlers and other attributes never survive.
        verifyNoActiveContent(sanitizeHtml(
            "<a href=\"https://ok/\" onclick=\"x()\" onmouseover=y target=_blank>t</a>"));
        // A quote in the href cannot break out of the attribute we write.
        const QString out = sanitizeHtml("<a href='https://ok/\"onclick=\"x'>t</a>");
        QVERIFY(out.contains("href=\"https://ok/&quot;onclick=&quot;x\""));
        QCOMPARE(attributes(out).size(), 2); // href + our style, nothing else
    }

    void nestedAnchorsAreFlattened()
    {
        const QString out = sanitizeHtml("<a href=https://a/>x<a href=https://b/>y</a>z</a>");
        QCOMPARE(out.count("<a "), 1);
    }

    void commentsDoctypesAndProcessingInstructionsAreRemoved()
    {
        QCOMPARE(sanitizeHtml("a<!-- <img src=x> -->b"), QStringLiteral("ab"));
        QCOMPARE(sanitizeHtml("a<!--><img src=x>-->b"), QStringLiteral("ab"));
        QCOMPARE(sanitizeHtml("a<!DOCTYPE html>b"), QStringLiteral("ab"));
        QCOMPARE(sanitizeHtml("a<?xml version=1?>b"), QStringLiteral("ab"));
        QCOMPARE(sanitizeHtml("a<![CDATA[<img src=x>]]>b"), QStringLiteral("a]]&gt;b"));
        QCOMPARE(sanitizeHtml("a<!-- never closed <img src=x>"), QStringLiteral("a"));
    }

    void strayMarkupCharactersAreEscaped()
    {
        QCOMPARE(sanitizeHtml("1 < 2 > 0"), QStringLiteral("1 &lt; 2 &gt; 0"));
        QCOMPARE(sanitizeHtml("a & b"), QStringLiteral("a &amp; b"));
        QCOMPARE(sanitizeHtml("<3"), QStringLiteral("&lt;3"));
        QCOMPARE(sanitizeHtml("< img src=x>"), QStringLiteral("&lt; img src=x&gt;"));
        // An unterminated tag is text, not a tag that eats the rest.
        QCOMPARE(sanitizeHtml("<b class=\"x>y"), QStringLiteral("&lt;b class=\"x&gt;y"));
        QCOMPARE(sanitizeHtml("<img src=x"), QStringLiteral("&lt;img src=x"));
        // Well-formed references stay; they decode to TEXT in Qt.
        QCOMPARE(sanitizeHtml("&lt;img&gt; &amp; &#60; &#x3C; &eacute;"),
                 QStringLiteral("&lt;img&gt; &amp; &#60; &#x3C; &eacute;"));
        QCOMPARE(sanitizeHtml("&#60"), QStringLiteral("&amp;#60"));
        QCOMPARE(sanitizeHtml("&nonsense"), QStringLiteral("&amp;nonsense"));
        // A NUL byte is dropped rather than truncating anything downstream.
        QCOMPARE(sanitizeHtml(QString::fromUtf8("a\0b", 3)), QStringLiteral("ab"));
    }

    void oddTagShapesCannotSmuggleATagThrough()
    {
        for (const char* in : {
                 "<scr<script>ipt>alert(1)</script>",
                 "<<img src=x>img src=x>",
                 "<img\nsrc=x\nonerror=alert(1)>",
                 "<img src=`x`onerror=alert(1)>",
                 "<IMG\tSRC=x>",
                 "<b/onmouseover=alert(1)>x</b>",
                 "<b \"onmouseover=alert(1)\">x</b>",
                 "<a href=\"https://ok/\"/onclick=alert(1)>x</a>",
                 "<svg/onload=alert(1)>",
                 "<math><mi xlink:href=\"javascript:alert(1)\">x</mi></math>",
                 "<table background=\"javascript:alert(1)\"><tr><td>x</td></tr></table>",
                 "<p style=\"background:url(javascript:alert(1))\">x</p>",
                 "<span data-mx-color=red style=\"font-size:9999px\">x</span>",
                 "<ſcript>x</ſcript>",
                 "<img src=x>>>>",
                 "</img src=x><img src=x>",
                 "<a href=\"https://ok/\">x</a ><img src=x>",
             }) {
            verifyNoActiveContent(sanitizeHtml(QString::fromUtf8(in)));
        }
    }

    void closingTagsOnlyCloseWhatWasOpened()
    {
        QCOMPARE(sanitizeHtml("</b>x</i>"), QStringLiteral("x"));
        QCOMPARE(sanitizeHtml("<b><i>x</b>y"), QStringLiteral("<b><i>x</i></b>y"));
        QCOMPARE(sanitizeHtml("<b>never closed"), QStringLiteral("<b>never closed</b>"));
    }

    void nestingIsCapped()
    {
        QString deep;
        for (int i = 0; i < 10000; ++i) deep += "<b>";
        deep += "x";
        const QString out = sanitizeHtml(deep);
        QVERIFY(out.count("<b>") <= 32);
        QCOMPARE(out.count("<b>"), out.count("</b>"));
        QVERIFY(out.contains('x'));
    }

    void outputIsStableUnderASecondPass()
    {
        for (const char* in : {"<b>x</b>", "<a href=https://ok/>t</a>", "a < b & c",
                               "<pre><code>x</code></pre>", "<img alt=\"<b>\">"}) {
            const QString once = sanitizeHtml(QString::fromUtf8(in));
            QCOMPARE(sanitizeHtml(once), once);
        }
    }

    void theHrefRuleOnItsOwn()
    {
        QVERIFY(isAllowedLinkHref("https://x/"));
        QVERIFY(isAllowedLinkHref("HTTP://x/"));
        QVERIFY(isAllowedLinkHref("mailto:a@b"));
        QVERIFY(isAllowedLinkHref("bsfchat://channel/x"));
        QVERIFY(!isAllowedLinkHref("https ://x/"));
        QVERIFY(!isAllowedLinkHref("ht\ntps://x/"));
        QVERIFY(!isAllowedLinkHref("javascript:x"));
        QVERIFY(!isAllowedLinkHref("file:///x"));
        QVERIFY(!isAllowedLinkHref(":x"));
        QVERIFY(!isAllowedLinkHref("x"));
    }
};

QTEST_GUILESS_MAIN(TestHtmlSanitizer)
#include "test_html_sanitizer.moc"
