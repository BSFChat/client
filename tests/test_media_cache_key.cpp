// What identifies a cached media object.
//
// Media URLs carry the session access token in the query string —
// MediaUrl.h explains why it cannot be an Authorization header: these URLs
// go straight into QML Image.source. The cache hashed the whole URL, so the
// credential was part of the key. Every re-login minted a new token, so
// every previously downloaded image and video missed, the entire cache was
// re-fetched over the network, and the orphaned copies kept consuming the
// 200 MB budget until eviction happened to reach them.
//
// That used to be rare. Access tokens now expire after 90 days and a
// re-login is an ordinary event, which turns it into a scheduled full
// re-download of every image the user has ever seen.

#include "core/MediaDownloader.h"

#include <QTest>

class TestMediaCacheKey : public QObject
{
    Q_OBJECT

    static QString url(const QString& token)
    {
        return QStringLiteral("https://bsfchat.com/_matrix/media/v3/download/"
                              "bsfchat.com/abc123?access_token=")
               + token;
    }

private slots:
    void sameObjectUnderDifferentTokensSharesAKey()
    {
        QCOMPARE(MediaDownloader::cacheKeyForUrl(url(QStringLiteral("syt_old_token"))),
                 MediaDownloader::cacheKeyForUrl(url(QStringLiteral("syt_new_token"))));
    }

    void differentObjectsDoNotShareAKey()
    {
        const QString a = QStringLiteral(
            "https://bsfchat.com/_matrix/media/v3/download/bsfchat.com/aaa?access_token=t");
        const QString b = QStringLiteral(
            "https://bsfchat.com/_matrix/media/v3/download/bsfchat.com/bbb?access_token=t");
        QVERIFY(MediaDownloader::cacheKeyForUrl(a) != MediaDownloader::cacheKeyForUrl(b));
    }

    void differentHomeserversDoNotShareAKey()
    {
        // Two servers can hand out the same media id; the host is part of
        // the object's identity.
        const QString a = QStringLiteral("https://one.example/_matrix/media/v3/download/s/x?access_token=t");
        const QString b = QStringLiteral("https://two.example/_matrix/media/v3/download/s/x?access_token=t");
        QVERIFY(MediaDownloader::cacheKeyForUrl(a) != MediaDownloader::cacheKeyForUrl(b));
    }

    void keyDoesNotContainTheToken()
    {
        const QString key = MediaDownloader::cacheKeyForUrl(url(QStringLiteral("secret-token")));
        QVERIFY(!key.contains(QStringLiteral("secret-token")));
        QVERIFY(!key.contains(QStringLiteral("access_token")));
    }

    void unauthenticatedUrlsAreUnaffected()
    {
        // require_media_auth off, or an unset token: no query string at all.
        const QString plain = QStringLiteral(
            "https://bsfchat.com/_matrix/media/v3/download/bsfchat.com/abc123");
        QCOMPARE(MediaDownloader::cacheKeyForUrl(plain), plain);
        QCOMPARE(MediaDownloader::cacheKeyForUrl(plain),
                 MediaDownloader::cacheKeyForUrl(plain + QStringLiteral("?access_token=t")));
    }

    void edgeCases()
    {
        QCOMPARE(MediaDownloader::cacheKeyForUrl(QString()), QString());
        QCOMPARE(MediaDownloader::cacheKeyForUrl(QStringLiteral("?onlyquery")), QString());
        // Only the FIRST '?' splits; anything after it is query, whatever it holds.
        QCOMPARE(MediaDownloader::cacheKeyForUrl(QStringLiteral("https://h/p?a=1?b=2")),
                 QStringLiteral("https://h/p"));
    }

    void extensionSurvivesForSniffing()
    {
        // The extension is preserved "so platform decoders can sniff". With
        // the query string in play, "the last dot" was a dot inside the
        // base64 token, so every authenticated download landed under a
        // meaningless extension and the sniffing never happened.
        const QString key = MediaDownloader::cacheKeyForUrl(QStringLiteral(
            "https://bsfchat.com/_matrix/media/v3/download/s/clip.mp4?access_token=aa.bbb"));
        QVERIFY(key.endsWith(QStringLiteral(".mp4")));
    }
};

QTEST_MAIN(TestMediaCacheKey)
#include "test_media_cache_key.moc"
