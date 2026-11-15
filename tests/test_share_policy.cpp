// What the Android share target accepts (audit M-4): another app's content://
// URI only — never file://, never one of our own providers, never a path
// inside our own storage. See core/SharePolicy.h.

#include "core/SharePolicy.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

namespace share = bsfchat::client::share;

class TestSharePolicy : public QObject {
    Q_OBJECT
private slots:
    void classifiesUris_data()
    {
        QTest::addColumn<QString>("uri");
        QTest::addColumn<int>("verdict");
        QTest::newRow("media store")
            << "content://media/external/images/media/42" << int(share::UriVerdict::Accept);
        QTest::newRow("other provider")
            << "content://com.example.files/share/a.pdf" << int(share::UriVerdict::Accept);
        QTest::newRow("upper-case scheme")
            << "CONTENT://media/external/1" << int(share::UriVerdict::Accept);
        QTest::newRow("file")
            << "file:///data/user/0/com.bsfchat.app/files/BSFChat.conf"
            << int(share::UriVerdict::NotContent);
        QTest::newRow("bare path")
            << "/data/user/0/com.bsfchat.app/files/BSFChat.conf" << int(share::UriVerdict::NotContent);
        QTest::newRow("no authority") << "content:///x" << int(share::UriVerdict::NotContent);
        QTest::newRow("http") << "https://example.com/a" << int(share::UriVerdict::NotContent);
        QTest::newRow("own provider")
            << "content://com.bsfchat.app/files/x" << int(share::UriVerdict::OwnProvider);
        QTest::newRow("own sub-provider")
            << "content://com.bsfchat.app.qtprovider/files/x" << int(share::UriVerdict::OwnProvider);
        QTest::newRow("look-alike authority")
            << "content://com.bsfchat.apps/x" << int(share::UriVerdict::Accept);
    }
    void classifiesUris()
    {
        QFETCH(QString, uri);
        QFETCH(int, verdict);
        QCOMPARE(int(share::classifyUri(uri, QStringLiteral("com.bsfchat.app"))), verdict);
    }

    void containment()
    {
        const QStringList dirs{QStringLiteral("/data/user/0/com.bsfchat.app")};
        QVERIFY(share::isInsideAny("/data/user/0/com.bsfchat.app/files/BSFChat.conf", dirs));
        QVERIFY(share::isInsideAny("/data/user/0/com.bsfchat.app", dirs));
        QVERIFY(share::isInsideAny("/data/user/0/other/../com.bsfchat.app/x", dirs));
        QVERIFY(!share::isInsideAny("/data/user/0/com.bsfchat.apps/x", dirs));
        QVERIFY(!share::isInsideAny("/sdcard/Download/a.pdf", dirs));
        QVERIFY(!share::isInsideAny("", dirs));
        // A root entry never makes everything "ours".
        QVERIFY(!share::isInsideAny("/sdcard/a", {QStringLiteral("/")}));
    }

    void containmentFollowsSymlinks()
    {
#ifdef Q_OS_UNIX
        QTemporaryDir own, elsewhere;
        QVERIFY(own.isValid() && elsewhere.isValid());
        QFile secret(own.filePath("secret.conf"));
        QVERIFY(secret.open(QIODevice::WriteOnly));
        secret.write("token");
        secret.close();
        const QString link = elsewhere.filePath("innocent.txt");
        QVERIFY(QFile::link(secret.fileName(), link));
        QVERIFY(share::isInsideAny(link, {own.path()}));
#endif
    }
};

QTEST_GUILESS_MAIN(TestSharePolicy)
#include "test_share_policy.moc"
