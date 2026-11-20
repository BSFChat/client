// "%n server(s)" reads "1 server" / "3 servers" (audit 3). Header-only.

#include "core/EnglishPlurals.h"

#include <QCoreApplication>
#include <QTest>

using bsfchat::client::englishPlural;

class TestEnglishPlurals : public QObject
{
    Q_OBJECT
private slots:
    void regularAndIrregular()
    {
        QCOMPARE(englishPlural(QStringLiteral("%n member(s)"), 1), QStringLiteral("%n member"));
        QCOMPARE(englishPlural(QStringLiteral("%n member(s)"), 0), QStringLiteral("%n members"));
        QCOMPARE(englishPlural(QStringLiteral("%n reply(s) in thread"), 2),
                 QStringLiteral("%n replies in thread"));
        QCOMPARE(englishPlural(QStringLiteral("%n reply(s) in thread"), 1),
                 QStringLiteral("%n reply in thread"));
    }
    void notMineWithoutACountOrMarker()
    {
        QVERIFY(englishPlural(QStringLiteral("%n members"), 3).isEmpty());
        QVERIFY(englishPlural(QStringLiteral("thing(s)"), -1).isEmpty());
    }
    void installedItAppliesToTr()
    {
        bsfchat::client::EnglishPluralTranslator t;
        QCoreApplication::installTranslator(&t);
        QCOMPARE(QCoreApplication::translate("X", "Push is on for %n server(s).", nullptr, 1),
                 QStringLiteral("Push is on for 1 server."));
        QCOMPARE(QCoreApplication::translate("X", "Push is on for %n server(s).", nullptr, 4),
                 QStringLiteral("Push is on for 4 servers."));
        QCOMPARE(QCoreApplication::translate("X", "Plain text"), QStringLiteral("Plain text"));
        QCoreApplication::removeTranslator(&t);
    }
};

QTEST_GUILESS_MAIN(TestEnglishPlurals)
#include "test_english_plurals.moc"
