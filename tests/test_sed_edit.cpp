// sed-style corrections in the composer: `s/teh/the/` edits your last message.
//
// What is pinned: which texts are treated as a command at all (ordinary chat
// must never be swallowed), `\/` escaping, the g and i flags, regex semantics
// with `&` and group references, and the guards (pattern length, invalid
// regex, no match). Pure logic — see util/SedEdit.h.

#include "util/SedEdit.h"

#include <QTest>

namespace sed = bsfchat::client::sed;

class TestSedEdit : public QObject {
    Q_OBJECT

    static QString run(const QString& cmd, const QString& body)
    {
        const auto p = sed::parse(cmd);
        if (p.status != sed::ParseStatus::Ok) return QStringLiteral("<parse:%1>").arg(int(p.status));
        const auto r = sed::apply(p.command, body);
        return r ? *r : QStringLiteral("<nomatch>");
    }

private slots:
    void notSed_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("plain") << "hello";
        QTest::newRow("one slash") << "s/he said";
        QTest::newRow("no closing slash") << "s/foo/bar";
        QTest::newRow("leading space") << " s/foo/bar/";
        QTest::newRow("escaped") << "\\s/foo/bar/";
        QTest::newRow("unknown flag") << "s/foo/bar/x";
        QTest::newRow("repeated flag") << "s/foo/bar/gg";
        QTest::newRow("trailing text") << "s/foo/bar/ please";
        QTest::newRow("multi-line") << "s/foo/bar/\nmore";
        QTest::newRow("code fence") << "```s/foo/bar/```";
        QTest::newRow("capital S") << "S/foo/bar/";
    }
    void notSed()
    {
        QFETCH(QString, text);
        QCOMPARE(sed::parse(text).status, sed::ParseStatus::NotSed);
    }

    void parsesFlagsAndEscapes()
    {
        auto p = sed::parse(QStringLiteral("s/a\\/b/c\\/d/gi"));
        QCOMPARE(p.status, sed::ParseStatus::Ok);
        QCOMPARE(p.command.pattern, QStringLiteral("a/b"));
        QCOMPARE(p.command.replacement, QStringLiteral("c/d"));
        QVERIFY(p.command.global);
        QVERIFY(p.command.caseInsensitive);

        p = sed::parse(QStringLiteral("s/x/y/ig"));
        QCOMPARE(p.status, sed::ParseStatus::Ok);
        QVERIFY(p.command.global && p.command.caseInsensitive);

        // Empty replacement deletes.
        p = sed::parse(QStringLiteral("s/x//"));
        QCOMPARE(p.status, sed::ParseStatus::Ok);
        QVERIFY(p.command.replacement.isEmpty());

        // Escaped backslash before the delimiter: `\\` stays, `/` closes.
        p = sed::parse(QStringLiteral("s/a\\\\/b/"));
        QCOMPARE(p.status, sed::ParseStatus::Ok);
        QCOMPARE(p.command.pattern, QStringLiteral("a\\\\"));
        QCOMPARE(p.command.replacement, QStringLiteral("b"));
    }

    void invalidCommands()
    {
        QCOMPARE(sed::parse(QStringLiteral("s//x/")).status, sed::ParseStatus::Invalid);
        QCOMPARE(sed::parse(QStringLiteral("s/(/x/")).status, sed::ParseStatus::Invalid);
        const QString longPat(sed::kMaxPatternChars + 1, u'a');
        const auto p = sed::parse(QStringLiteral("s/%1/x/").arg(longPat));
        QCOMPARE(p.status, sed::ParseStatus::Invalid);
        QVERIFY(!p.error.isEmpty());
        const QString okPat(sed::kMaxPatternChars, u'a');
        QCOMPARE(sed::parse(QStringLiteral("s/%1/x/").arg(okPat)).status, sed::ParseStatus::Ok);
    }

    void applies()
    {
        QCOMPARE(run("s/teh/the/", "teh cat and teh dog"), QStringLiteral("the cat and teh dog"));
        QCOMPARE(run("s/teh/the/g", "teh cat and teh dog"), QStringLiteral("the cat and the dog"));
        QCOMPARE(run("s/TEH/the/i", "Teh cat"), QStringLiteral("the cat"));
        QCOMPARE(run("s/teh/the/", "TEH cat"), QStringLiteral("<nomatch>"));
        // Regex, not substring.
        QCOMPARE(run("s/c.t/dog/", "a cat"), QStringLiteral("a dog"));
        QCOMPARE(run("s/\\d+/N/g", "1 and 22"), QStringLiteral("N and N"));
        // & and groups.
        QCOMPARE(run("s/cat/[&]/", "a cat"), QStringLiteral("a [cat]"));
        QCOMPARE(run("s/(\\w+) (\\w+)/\\2 \\1/", "hello world"), QStringLiteral("world hello"));
        QCOMPARE(run("s/cat/\\&/", "a cat"), QStringLiteral("a &"));
        QCOMPARE(run("s/cat/\\\\/", "a cat"), QStringLiteral("a \\"));
        // Literal slash on both sides.
        QCOMPARE(run("s/a\\/b/c\\/d/", "x a/b y"), QStringLiteral("x c/d y"));
        // Empty matches under g make progress and terminate.
        QCOMPARE(run("s/x*/-/g", "ab"), QStringLiteral("-a-b-"));
    }

    void bodyTooLargeIsRefused()
    {
        const auto p = sed::parse(QStringLiteral("s/a/b/"));
        QVERIFY(!sed::apply(p.command, QString(sed::kMaxBodyChars + 1, u'a')).has_value());
    }

    void catastrophicPatternTerminates()
    {
        // Classic exponential backtracking; PCRE2's match limit must stop it
        // and the result is simply "no match" (or a match), never a hang.
        const auto p = sed::parse(QStringLiteral("s/(a+)+$/x/"));
        QCOMPARE(p.status, sed::ParseStatus::Ok);
        const QString body = QString(40, u'a') + u'!';
        (void)sed::apply(p.command, body);
        QVERIFY(true);
    }
};

QTEST_GUILESS_MAIN(TestSedEdit)
#include "test_sed_edit.moc"
