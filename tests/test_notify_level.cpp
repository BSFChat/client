// The client's notify-level default matches the server's (audit 3): all in a
// DM, mentions in a channel; an explicit choice always wins. Header-only.

#include "core/NotifyLevel.h"

#include <QTest>

using namespace bsfchat::client;

class TestNotifyLevel : public QObject
{
    Q_OBJECT
private slots:
    void defaultsMatchPushService()
    {
        QCOMPARE(effectiveNotifyLevel(QString(), false), QStringLiteral("mentions"));
        QCOMPARE(effectiveNotifyLevel(QString(), true), QStringLiteral("all"));
    }
    void anExplicitChoiceWins()
    {
        for (const char* l : {"all", "mentions", "none"}) {
            QCOMPARE(effectiveNotifyLevel(QLatin1String(l), false), QLatin1String(l));
            QCOMPARE(effectiveNotifyLevel(QLatin1String(l), true), QLatin1String(l));
        }
    }
    void garbageIsNoChoice()
    {
        QVERIFY(!isNotifyLevel(QStringLiteral("loud")));
        QCOMPARE(effectiveNotifyLevel(QStringLiteral("loud"), false), QStringLiteral("mentions"));
    }
};

QTEST_MAIN(TestNotifyLevel)
#include "test_notify_level.moc"
