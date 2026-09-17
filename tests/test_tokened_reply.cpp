// Correlating an async reply with the request that asked for it.
//
// MatrixClient answers on connection-wide signal pairs — mediaUploaded /
// mediaUploadError, createRoomSuccess / createRoomError — and several
// unrelated flows wait on the same pair at once: a file attachment and an
// avatar upload; a DM and a channel; two DMs started back to back. Those call
// sites all used Qt::SingleShotConnection believing it meant "only one slot
// runs per emission".
//
// It does not. It disconnects a slot AFTER that slot has run, but a single
// emit still invokes every connected slot. `singleShotStillInvokesEveryHandler`
// below demonstrates that directly, because the whole family of bugs it caused
// rests on one plausible-sounding but wrong belief:
//
//   * a file attachment was posted carrying the avatar's mxc:// URI, because
//     whichever upload finished first delivered its result to both handlers;
//   * a failed send left its success handler armed forever, so it fired on a
//     later, unrelated upload;
//   * two DMs: the first room id went to both handlers, so one DM was
//     labelled with the other's peer and the second was never a DM at all;
//   * a failed private-channel create left a hook armed that applied
//     @everyone DENY VIEW_CHANNEL to the next channel — or to a DM room.

#include "net/TokenedReply.h"

#include <QSignalSpy>
#include <QTest>

using bsfchat::net::awaitTokenedReply;
using bsfchat::net::newRequestToken;

// Stands in for MatrixClient: a pair of token-carrying result signals.
class FakeClient : public QObject
{
    Q_OBJECT
public:
signals:
    void ok(const QString& token, const QString& value);
    void failed(const QString& token, const QString& why);
};

class TestTokenedReply : public QObject
{
    Q_OBJECT

private slots:
    void singleShotStillInvokesEveryHandler()
    {
        // The belief the old code rested on, shown to be false. Kept so
        // nobody reintroduces the pattern thinking it is safe.
        FakeClient client;
        QStringList seen;
        QObject::connect(&client, &FakeClient::ok, this,
                         [&seen](const QString&, const QString& v) { seen << "a:" + v; },
                         Qt::SingleShotConnection);
        QObject::connect(&client, &FakeClient::ok, this,
                         [&seen](const QString&, const QString& v) { seen << "b:" + v; },
                         Qt::SingleShotConnection);

        emit client.ok(QStringLiteral("t1"), QStringLiteral("first"));
        QCOMPARE(seen, QStringList({"a:first", "b:first"}));

        // Both are gone now — which is the other half of the problem: the
        // second request's reply reaches nobody.
        emit client.ok(QStringLiteral("t2"), QStringLiteral("second"));
        QCOMPARE(seen.size(), 2);
    }

    void eachRequestGetsOnlyItsOwnReply()
    {
        FakeClient client;
        QStringList aGot, bGot;
        const QString ta = newRequestToken();
        const QString tb = newRequestToken();
        QVERIFY(ta != tb);

        awaitTokenedReply(&client, this, ta, &FakeClient::ok, &FakeClient::failed,
                          [&aGot](const QString& v) { aGot << "ok:" + v; },
                          [&aGot](const QString& e) { aGot << "err:" + e; });
        awaitTokenedReply(&client, this, tb, &FakeClient::ok, &FakeClient::failed,
                          [&bGot](const QString& v) { bGot << "ok:" + v; },
                          [&bGot](const QString& e) { bGot << "err:" + e; });

        // B's upload finishes first. A must not see it.
        emit client.ok(tb, QStringLiteral("mxc://avatar"));
        QCOMPARE(bGot, QStringList({"ok:mxc://avatar"}));
        QVERIFY(aGot.isEmpty());

        emit client.ok(ta, QStringLiteral("mxc://attachment"));
        QCOMPARE(aGot, QStringList({"ok:mxc://attachment"}));
        QCOMPARE(bGot.size(), 1);
    }

    void failureTearsDownTheSuccessHandlerToo()
    {
        // The leak: on error only the error handler used to fire and
        // disconnect, leaving the success handler armed for somebody else's
        // upload later.
        FakeClient client;
        QStringList got;
        const QString token = newRequestToken();
        awaitTokenedReply(&client, this, token, &FakeClient::ok, &FakeClient::failed,
                          [&got](const QString& v) { got << "ok:" + v; },
                          [&got](const QString& e) { got << "err:" + e; });

        emit client.failed(token, QStringLiteral("too large"));
        QCOMPARE(got, QStringList({"err:too large"}));

        // A late or duplicated success for the same token must be ignored.
        emit client.ok(token, QStringLiteral("mxc://late"));
        QCOMPARE(got.size(), 1);
    }

    void successTearsDownTheErrorHandlerToo()
    {
        // The mirror leak: a spurious "send failed" toast on the next
        // unrelated upload error.
        FakeClient client;
        QStringList got;
        const QString token = newRequestToken();
        awaitTokenedReply(&client, this, token, &FakeClient::ok, &FakeClient::failed,
                          [&got](const QString& v) { got << "ok:" + v; },
                          [&got](const QString& e) { got << "err:" + e; });

        emit client.ok(token, QStringLiteral("mxc://done"));
        emit client.failed(token, QStringLiteral("boom"));
        QCOMPARE(got, QStringList({"ok:mxc://done"}));
    }

    void unrelatedTokensAreIgnoredEntirely()
    {
        // createCategoryRoom reports success on its own signal but its
        // failures still ride createRoomError with an empty token. Nothing
        // that is waiting for a specific request may claim it.
        FakeClient client;
        int calls = 0;
        awaitTokenedReply(&client, this, newRequestToken(),
                          &FakeClient::ok, &FakeClient::failed,
                          [&calls](const QString&) { ++calls; },
                          [&calls](const QString&) { ++calls; });

        emit client.failed(QString(), QStringLiteral("category create failed"));
        emit client.ok(QStringLiteral("somebody-else"), QStringLiteral("mxc://x"));
        QCOMPARE(calls, 0);
    }

    void handlersDoNotOutliveTheirContext()
    {
        // ServerConnection is the context object in production and can be
        // destroyed mid-flight (server removed while an upload runs).
        FakeClient client;
        int calls = 0;
        {
            QObject context;
            awaitTokenedReply(&client, &context, QStringLiteral("t"),
                              &FakeClient::ok, &FakeClient::failed,
                              [&calls](const QString&) { ++calls; },
                              [&calls](const QString&) { ++calls; });
        }
        emit client.ok(QStringLiteral("t"), QStringLiteral("v"));
        QCOMPARE(calls, 0);
    }

    void tokensAreUnique()
    {
        QSet<QString> seen;
        for (int i = 0; i < 1000; ++i) seen.insert(newRequestToken());
        QCOMPARE(seen.size(), 1000);
    }
};

QTEST_MAIN(TestTokenedReply)
#include "test_tokened_reply.moc"
