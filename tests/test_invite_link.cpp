// bsfchat://invite parsing (src/core/InviteLink.h): what a link may prefill
// into Add server, and what it may not.
#include "core/InviteLink.h"

#include <QTest>

using bsfchat::invite::isInviteLink;
using bsfchat::invite::serverFromLink;

class TestInviteLink : public QObject {
    Q_OBJECT
private slots:
    void recognises()
    {
        QVERIFY(isInviteLink(QStringLiteral("bsfchat://invite/chat.example.com")));
        QVERIFY(isInviteLink(QStringLiteral("bsfchat://invite?server=chat.example.com")));
        QVERIFY(!isInviteLink(QStringLiteral("bsfchat://message/x/y/z")));
        QVERIFY(!isInviteLink(QStringLiteral("bsfchat://invitex/chat.example.com")));
        QVERIFY(!isInviteLink(QStringLiteral("https://invite/chat.example.com")));
    }

    void accepts_data()
    {
        QTest::addColumn<QString>("link");
        QTest::addColumn<QString>("server");
        QTest::newRow("host") << "bsfchat://invite/chat.example.com" << "chat.example.com";
        QTest::newRow("host:port") << "bsfchat://invite/chat.example.com:8448" << "chat.example.com:8448";
        QTest::newRow("trailing slash") << "bsfchat://invite/chat.example.com/" << "chat.example.com";
        QTest::newRow("https encoded") << "bsfchat://invite/https%3A%2F%2Fchat.example.com" << "chat.example.com";
        QTest::newRow("https raw") << "bsfchat://invite/https://chat.example.com:444" << "chat.example.com:444";
        QTest::newRow("https path") << "bsfchat://invite/https%3A%2F%2Fexample.com%2Fchat" << "https://example.com/chat";
        QTest::newRow("query form") << "bsfchat://invite?server=chat.example.com" << "chat.example.com";
        QTest::newRow("query form encoded") << "bsfchat://invite?server=https%3A%2F%2Fchat.example.com" << "chat.example.com";
        QTest::newRow("path form drops query") << "bsfchat://invite/chat.example.com?ref=x" << "chat.example.com";
        QTest::newRow("ipv4") << "bsfchat://invite/192.0.2.7:8008" << "192.0.2.7:8008";
        QTest::newRow("ipv6") << "bsfchat://invite/[2001:db8::1]:8448" << "[2001:db8::1]:8448";
    }
    void accepts()
    {
        QFETCH(QString, link);
        QFETCH(QString, server);
        QCOMPARE(serverFromLink(link), server);
    }

    void refuses_data()
    {
        QTest::addColumn<QString>("link");
        QTest::newRow("empty") << "bsfchat://invite/";
        QTest::newRow("empty query") << "bsfchat://invite?server=";
        QTest::newRow("http") << "bsfchat://invite/http%3A%2F%2Fchat.example.com";
        QTest::newRow("javascript") << "bsfchat://invite/javascript:alert(1)";
        QTest::newRow("file") << "bsfchat://invite/file%3A%2F%2F%2Fetc%2Fpasswd";
        QTest::newRow("userinfo") << "bsfchat://invite/https%3A%2F%2Fuser%3Apw%40chat.example.com";
        QTest::newRow("space") << "bsfchat://invite/chat%20example.com";
        QTest::newRow("newline") << "bsfchat://invite/chat.example.com%0Aevil";
        QTest::newRow("odd host chars") << "bsfchat://invite/chat_example!.com";
        QTest::newRow("not an invite") << "bsfchat://message/chat.example.com/r/e";
    }
    void refuses()
    {
        QFETCH(QString, link);
        QCOMPARE(serverFromLink(link), QString());
    }
};

QTEST_GUILESS_MAIN(TestInviteLink)
#include "test_invite_link.moc"
