// Server-side link previews: the policy layer in front of preview_url
// (src/net/LinkPreviewCache.h, audit Q-S4).
//
// The cache is what keeps "move previews to the server" from turning into
// "hammer the server": one request per URL however many cards show it, a
// concurrency cap, negative results remembered, and a server that has no
// preview endpoint asked exactly once. It is also where the guarantee lives
// that a server without previews means no previews — there is no path from
// here back to fetching the page ourselves.

#include "net/LinkPreviewCache.h"

#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

using bsfchat::client::LinkPreviewCache;
using bsfchat::net::MatrixFailure;
using bsfchat::net::parseMatrixFailure;
using State = LinkPreviewCache::State;

namespace {

MatrixFailure failure(int status, const char* body = "")
{
    return parseMatrixFailure(status, QByteArray(body));
}

QJsonObject page(const QString& title)
{
    QJsonObject o;
    o[QStringLiteral("og:title")] = title;
    return o;
}

const QString kA = QStringLiteral("https://example.org/a");
const QString kB = QStringLiteral("https://example.org/b");

} // namespace

class TestLinkPreviewCache : public QObject
{
    Q_OBJECT

    qint64 m_now = 1'000'000;
    LinkPreviewCache* make()
    {
        return new LinkPreviewCache(this, [this] { return m_now; });
    }

private slots:
    void init() { m_now = 1'000'000; }

    void concurrentRequestsForOneUrlFetchOnce()
    {
        auto* c = make();
        QSignalSpy fetch(c, &LinkPreviewCache::fetchRequested);
        QSignalSpy resolved(c, &LinkPreviewCache::resolved);

        QVERIFY(c->request(kA));
        QVERIFY(c->request(kA));   // second card, same link
        QVERIFY(c->request(kA));
        QCOMPARE(fetch.count(), 1);
        QCOMPARE(c->state(kA), State::Pending);

        c->fetchSucceeded(kA, page(QStringLiteral("Hello")));
        QCOMPARE(resolved.count(), 1);
        QCOMPARE(c->state(kA), State::Ready);
        QCOMPARE(c->preview(kA).title, QStringLiteral("Hello"));

        // Cached: no further fetch, and request() says "already known".
        QVERIFY(!c->request(kA));
        QCOMPARE(fetch.count(), 1);
    }

    void positiveResultExpires()
    {
        auto* c = make();
        QSignalSpy fetch(c, &LinkPreviewCache::fetchRequested);
        c->request(kA);
        c->fetchSucceeded(kA, page(QStringLiteral("x")));
        m_now += LinkPreviewCache::kPositiveTtlMs - 1;
        QCOMPARE(c->state(kA), State::Ready);
        m_now += 2;
        QCOMPARE(c->state(kA), State::Unknown);
        QVERIFY(c->request(kA));
        QCOMPARE(fetch.count(), 2);
    }

    void negativeResultsAreCachedWithShorterTtl()
    {
        auto* c = make();
        QSignalSpy fetch(c, &LinkPreviewCache::fetchRequested);

        // 404 M_NOT_FOUND: nothing to preview for this URL.
        c->request(kA);
        c->fetchFailed(kA, failure(404, R"({"errcode":"M_NOT_FOUND","error":"x"})"));
        QCOMPARE(c->state(kA), State::Empty);
        QVERIFY(!c->request(kA));
        QVERIFY(c->supported());

        // A 200 with nothing in it is a negative too.
        c->request(kB);
        c->fetchSucceeded(kB, QJsonObject{{QStringLiteral("og:site_name"), QStringLiteral("S")}});
        QCOMPARE(c->state(kB), State::Empty);
        QCOMPARE(fetch.count(), 2);

        QVERIFY(LinkPreviewCache::kNegativeTtlMs < LinkPreviewCache::kPositiveTtlMs);
        m_now += LinkPreviewCache::kNegativeTtlMs + 1;
        QCOMPARE(c->state(kA), State::Unknown);
        QVERIFY(c->request(kA));
        QCOMPARE(fetch.count(), 3);
    }

    void transientFailureRetriesSooner()
    {
        auto* c = make();
        c->request(kA);
        c->fetchFailed(kA, failure(0));   // timeout / no connection
        QCOMPARE(c->state(kA), State::Empty);
        m_now += LinkPreviewCache::kTransientTtlMs + 1;
        QCOMPARE(c->state(kA), State::Unknown);
    }

    void unsupportedServerIsRememberedAndNothingIsFetchedAgain_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QString>("body");
        QTest::newRow("M_UNRECOGNIZED 404") << 404 << R"({"errcode":"M_UNRECOGNIZED"})";
        QTest::newRow("M_UNRECOGNIZED 400") << 400 << R"({"errcode":"M_UNRECOGNIZED"})";
        QTest::newRow("bare 404") << 404 << "<html>not found</html>";
        QTest::newRow("bare 405") << 405 << "";
        QTest::newRow("501") << 501 << "";
    }
    void unsupportedServerIsRememberedAndNothingIsFetchedAgain()
    {
        QFETCH(int, status);
        QFETCH(QString, body);
        auto* c = make();
        QSignalSpy fetch(c, &LinkPreviewCache::fetchRequested);
        QSignalSpy resolved(c, &LinkPreviewCache::resolved);
        QSignalSpy supported(c, &LinkPreviewCache::supportedChanged);

        // Fill the in-flight slots and the queue behind them.
        for (int i = 0; i < LinkPreviewCache::kMaxInFlight + 3; ++i)
            c->request(QStringLiteral("https://example.org/%1").arg(i));
        QCOMPARE(fetch.count(), LinkPreviewCache::kMaxInFlight);

        const QString first = fetch.first().first().toString();
        c->fetchFailed(first, failure(status, body.toUtf8().constData()));
        QVERIFY(!c->supported());
        QCOMPARE(supported.count(), 1);
        // Every waiting card hears about it, so none sits "pending" forever.
        QCOMPARE(resolved.count(), LinkPreviewCache::kMaxInFlight + 3);
        QCOMPARE(c->queuedCount(), 0);

        // Late replies for the others change nothing and start nothing.
        c->fetchFailed(fetch.at(1).first().toString(), failure(status, body.toUtf8().constData()));
        QCOMPARE(supported.count(), 1);

        QVERIFY(!c->request(QStringLiteral("https://example.org/new")));
        QCOMPARE(fetch.count(), LinkPreviewCache::kMaxInFlight);
        QCOMPARE(c->state(kA), State::Empty);
    }

    void notFoundWithErrcodeIsNotUnsupported()
    {
        QVERIFY(!LinkPreviewCache::isUnsupportedFailure(
            failure(404, R"({"errcode":"M_NOT_FOUND"})")));
        QVERIFY(!LinkPreviewCache::isUnsupportedFailure(
            failure(400, R"({"errcode":"M_INVALID_PARAM"})")));
        QVERIFY(!LinkPreviewCache::isUnsupportedFailure(
            failure(403, R"({"errcode":"M_FORBIDDEN"})")));
        QVERIFY(!LinkPreviewCache::isUnsupportedFailure(failure(500)));
        QVERIFY(!LinkPreviewCache::isUnsupportedFailure(failure(0)));
    }

    void concurrencyIsCappedAndTheQueueDrains()
    {
        auto* c = make();
        QSignalSpy fetch(c, &LinkPreviewCache::fetchRequested);
        const int n = LinkPreviewCache::kMaxInFlight + 2;
        for (int i = 0; i < n; ++i)
            c->request(QStringLiteral("https://example.org/%1").arg(i));
        QCOMPARE(fetch.count(), LinkPreviewCache::kMaxInFlight);
        QCOMPARE(c->queuedCount(), 2);
        QCOMPARE(c->state(QStringLiteral("https://example.org/%1").arg(n - 1)), State::Pending);

        c->fetchSucceeded(fetch.first().first().toString(), page(QStringLiteral("t")));
        QCOMPARE(fetch.count(), LinkPreviewCache::kMaxInFlight + 1);
        QCOMPARE(c->inFlightCount(), LinkPreviewCache::kMaxInFlight);
    }

    void queueOverflowDropsTheOldestAndSaysSo()
    {
        auto* c = make();
        QSignalSpy resolved(c, &LinkPreviewCache::resolved);
        const int total = LinkPreviewCache::kMaxInFlight + LinkPreviewCache::kMaxQueued + 1;
        for (int i = 0; i < total; ++i)
            c->request(QStringLiteral("https://example.org/%1").arg(i));
        QCOMPARE(c->queuedCount(), LinkPreviewCache::kMaxQueued);
        QCOMPARE(resolved.count(), 1);
        const QString dropped = resolved.first().first().toString();
        QCOMPARE(dropped, QStringLiteral("https://example.org/%1").arg(LinkPreviewCache::kMaxInFlight));
        QCOMPARE(c->state(dropped), State::Unknown);
    }

    void rateLimitPausesAndRequeues()
    {
        auto* c = make();
        QSignalSpy fetch(c, &LinkPreviewCache::fetchRequested);
        QSignalSpy resolved(c, &LinkPreviewCache::resolved);
        c->request(kA);
        c->fetchFailed(kA, failure(429, R"({"errcode":"M_LIMIT_EXCEEDED","retry_after_ms":10000})"));
        // Still waiting, not given up on, and not re-sent while paused.
        QCOMPARE(c->state(kA), State::Pending);
        QCOMPARE(resolved.count(), 0);
        QCOMPARE(fetch.count(), 1);
        c->request(kB);
        QCOMPARE(fetch.count(), 1);

        // Once the wait is over the next pump sends the requeued URL first.
        m_now += 10'001;
        c->request(QStringLiteral("https://example.org/c"));
        QVERIFY(fetch.count() >= 2);
        QCOMPARE(fetch.at(1).first().toString(), kA);
    }

    void disabledMeansNoRequests()
    {
        auto* c = make();
        QSignalSpy fetch(c, &LinkPreviewCache::fetchRequested);
        QSignalSpy resolved(c, &LinkPreviewCache::resolved);
        for (int i = 0; i < LinkPreviewCache::kMaxInFlight + 2; ++i)
            c->request(QStringLiteral("https://example.org/%1").arg(i));
        const int sent = int(fetch.count());

        c->setEnabled(false);
        QCOMPARE(c->queuedCount(), 0);
        QCOMPARE(resolved.count(), 2);   // the two that never left the queue
        QVERIFY(!c->request(kA));
        QCOMPARE(fetch.count(), sent);
        QCOMPARE(c->state(kA), State::Empty);

        c->setEnabled(true);
        QVERIFY(c->request(kA));
    }

    void togglingAnnouncesItSoCardsCanAskAgain()
    {
        // Re-enabling used to be silent, and a card that had seen the
        // setting flip first had already recorded "no preview".
        auto* c = make();
        QSignalSpy changed(c, &LinkPreviewCache::enabledChanged);
        c->setEnabled(false);
        c->setEnabled(false);   // no change, no signal
        c->setEnabled(true);
        QCOMPARE(changed.count(), 2);
        QVERIFY(c->enabled());
    }

    void onlyHttpUrlsAreSent()
    {
        auto* c = make();
        QSignalSpy fetch(c, &LinkPreviewCache::fetchRequested);
        QVERIFY(!c->request(QStringLiteral("file:///etc/passwd")));
        QVERIFY(!c->request(QStringLiteral("javascript:alert(1)")));
        QVERIFY(!c->request(QStringLiteral("https://user:pw@example.org/")));
        QVERIFY(!c->request(QStringLiteral("https:///nohost")));
        QVERIFY(!c->request(QStringLiteral("https://example.org/") + QString(3000, QLatin1Char('a'))));
        QCOMPARE(fetch.count(), 0);
    }

    void lruEvictsLeastRecentlyUsed()
    {
        auto* c = make();
        connect(c, &LinkPreviewCache::fetchRequested, c, [c](const QString& url) {
            c->fetchSucceeded(url, page(url));
        });
        c->request(kA);
        for (int i = 0; i < LinkPreviewCache::kMaxEntries - 1; ++i)
            c->request(QStringLiteral("https://example.org/n%1").arg(i));
        QCOMPARE(c->size(), LinkPreviewCache::kMaxEntries);
        QCOMPARE(c->state(kA), State::Ready);   // touch: now most recent
        c->request(kB);
        QCOMPARE(c->size(), LinkPreviewCache::kMaxEntries);
        QCOMPARE(c->state(kA), State::Ready);
        QCOMPARE(c->state(QStringLiteral("https://example.org/n0")), State::Unknown);
    }

    void parseTrimsAndValidates()
    {
        QJsonObject o;
        o[QStringLiteral("og:title")] = QStringLiteral("  Two\n\tlines ") + QChar(0x202E)
            + QStringLiteral("evil  ");
        o[QStringLiteral("og:description")] = QString(5000, QLatin1Char('d'));
        o[QStringLiteral("og:site_name")] = QStringLiteral("Site");
        o[QStringLiteral("og:image")] = QStringLiteral("mxc://example.org/abc123");
        o[QStringLiteral("og:image:width")] = 640;
        o[QStringLiteral("og:image:height")] = 480;
        bool ok = false;
        auto p = LinkPreviewCache::parse(o, &ok);
        QVERIFY(ok);
        QCOMPARE(p.title, QStringLiteral("Two lines evil"));
        QCOMPARE(p.description.size(), LinkPreviewCache::kMaxDescriptionChars);
        QVERIFY(p.description.endsWith(QChar(0x2026)));
        QCOMPARE(p.imageMxc, QStringLiteral("mxc://example.org/abc123"));
        QCOMPARE(p.imageWidth, 640);

        // Only an mxc image is ever accepted: an http(s) og:image would be the
        // client fetching a third-party URL again.
        o[QStringLiteral("og:image")] = QStringLiteral("https://tracker.example/pixel.png");
        QVERIFY(LinkPreviewCache::parse(o).imageMxc.isEmpty());
        o[QStringLiteral("og:image")] = QStringLiteral("mxc://example.org/id/../x.bat");
        QVERIFY(LinkPreviewCache::parse(o).imageMxc.isEmpty());

        // Oversized image is dropped, the text survives.
        o[QStringLiteral("og:image")] = QStringLiteral("mxc://example.org/abc123");
        o[QStringLiteral("matrix:image:size")] = double(LinkPreviewCache::kMaxImageBytes + 1);
        p = LinkPreviewCache::parse(o, &ok);
        QVERIFY(ok);
        QVERIFY(p.imageMxc.isEmpty());

        // Non-string fields are ignored rather than stringified.
        QJsonObject bad;
        bad[QStringLiteral("og:title")] = 42;
        LinkPreviewCache::parse(bad, &ok);
        QVERIFY(!ok);
    }
};

QTEST_GUILESS_MAIN(TestLinkPreviewCache)
#include "test_link_preview_cache.moc"
