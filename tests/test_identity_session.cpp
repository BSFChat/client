// The app's BSFChat ID session: sign in once, then join servers with no
// browser. What these pin is the part that can sign a user out everywhere if
// it is wrong: refresh tokens are single-use, so requests must queue, and the
// rotated token must be saved before anything uses what it bought.

#include "core/SecretStore.h"
#include "identity/IdentitySession.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

namespace {

QString fakeIdToken(const QString& aud, const QString& nonce)
{
    auto b64 = [](const QByteArray& b) {
        return QString::fromLatin1(b.toBase64(QByteArray::Base64UrlEncoding
                                              | QByteArray::OmitTrailingEquals));
    };
    QJsonObject claims{{"aud", aud}, {"nonce", nonce}, {"sub", "acct-1"}};
    return b64("{\"alg\":\"none\"}") + "." + b64(QJsonDocument(claims).toJson()) + ".sig";
}

struct Call {
    QUrl url;
    QUrlQuery form;
    std::function<void(int, const QByteArray&)> done;
};

} // namespace

class TestIdentitySession : public QObject {
    Q_OBJECT

    MemorySecretStore* store = nullptr;
    IdentitySession* session = nullptr;
    QList<Call> calls;

    void reply(int i, int status, const QJsonObject& body)
    {
        calls[i].done(status, QJsonDocument(body).toJson());
    }
    // A well-formed success for call i, echoing the nonce it carried.
    void succeed(int i, const QString& newRefresh)
    {
        const auto& f = calls[i].form;
        QJsonObject body{{"refresh_token", newRefresh}, {"access_token", "at-" + newRefresh}};
        if (f.hasQueryItem("resource")) {
            body["id_token"] = fakeIdToken(f.queryItemValue("resource", QUrl::FullyDecoded),
                                           f.queryItemValue("nonce"));
        }
        reply(i, 200, body);
    }

private slots:
    void init()
    {
        store = new MemorySecretStore;
        calls.clear();
        session = new IdentitySession;
        session->setStore(store);
        session->setTransport([this](const QUrl& url, const QUrlQuery& form,
                                     std::function<void(int, const QByteArray&)> done) {
            calls.append({url, form, std::move(done)});
        });
    }
    void cleanup()
    {
        delete session;
        delete store;
    }

    void theRefreshTokenGoesToTheSecureStoreOnly()
    {
        session->adopt("https://id.example/", "rt1");
        QVERIFY(session->isSignedIn());
        QVERIFY(session->isPersisted());
        QCOMPARE(store->read(IdentitySession::storeKey("https://id.example")).value_or(""),
                 QString("rt1"));

        IdentitySession next;
        next.setStore(store);
        QVERIFY(next.restore("https://ID.example"));
        QCOMPARE(next.providerUrl(), QString("https://ID.example"));
    }

    void withNoSecureStoreItIsKeptInMemoryOnly()
    {
        store->setAvailable(false);
        session->adopt("https://id.example", "rt1");
        QVERIFY(session->isSignedIn());
        QVERIFY(!session->isPersisted());
        QCOMPARE(store->size(), 0);
        IdentitySession next;
        next.setStore(store);
        QVERIFY(!next.restore("https://id.example"));
    }

    // The keychain goes away mid-run (locked, daemon gone) and a rotation
    // cannot be saved. The copy it still holds is now spent; restoring it
    // next launch would be a replay, and a replay revokes the whole grant.
    void aRotationThatCannotBeSavedLeavesNoSpentTokenBehind()
    {
        session->adopt("https://id.example", "rt1");
        QVERIFY(session->isPersisted());
        store->setAvailable(false);
        QString err;
        session->requestAccessToken([&](const QString&, const QString& e) { err = e; });
        QCOMPARE(calls.size(), 1);
        succeed(0, "rt2");
        QVERIFY2(err.isEmpty(), qPrintable(err));
        QVERIFY(session->isSignedIn());
        QVERIFY(!session->isPersisted());
        QCOMPARE(store->size(), 0);
        IdentitySession next;
        next.setStore(store);
        QVERIFY(!next.restore("https://id.example"));
    }

    void aServerTokenIsForThatServerAndTheRotationIsSavedFirst()
    {
        session->adopt("https://id.example", "rt1");
        QString got, err, storedAtCallback;
        session->requestServerToken("https://chat.example",
                                    [&](const QString& t, const QString& e) {
            got = t;
            err = e;
            storedAtCallback = store->read(IdentitySession::storeKey("https://id.example"))
                                   .value_or("");
        });
        QCOMPARE(calls.size(), 1);
        QCOMPARE(calls[0].url.toString(), QString("https://id.example/token"));
        QCOMPARE(calls[0].form.queryItemValue("grant_type"), QString("refresh_token"));
        QCOMPARE(calls[0].form.queryItemValue("refresh_token"), QString("rt1"));
        QCOMPARE(calls[0].form.queryItemValue("resource", QUrl::FullyDecoded),
                 QString("https://chat.example"));
        QVERIFY(!calls[0].form.queryItemValue("nonce").isEmpty());

        succeed(0, "rt2");
        QVERIFY2(err.isEmpty(), qPrintable(err));
        QVERIFY(!got.isEmpty());
        QCOMPARE(storedAtCallback, QString("rt2"));
    }

    void concurrentJoinsQueueAndEachUsesTheLatestToken()
    {
        // Two at once would present the same single-use token twice, which
        // the provider treats as theft and answers by signing the user out.
        session->adopt("https://id.example", "rt1");
        int done = 0;
        auto cb = [&](const QString& t, const QString& e) {
            QVERIFY2(e.isEmpty(), qPrintable(e));
            QVERIFY(!t.isEmpty());
            ++done;
        };
        session->requestServerToken("https://a.example", cb);
        session->requestServerToken("https://b.example", cb);
        QCOMPARE(calls.size(), 1); // the second waits
        succeed(0, "rt2");
        QCOMPARE(calls.size(), 2);
        QCOMPARE(calls[1].form.queryItemValue("refresh_token"), QString("rt2"));
        succeed(1, "rt3");
        QCOMPARE(done, 2);
    }

    void aTokenForAnotherServerIsRefused()
    {
        session->adopt("https://id.example", "rt1");
        QString got, err;
        session->requestServerToken("https://chat.example",
                                    [&](const QString& t, const QString& e) { got = t; err = e; });
        reply(0, 200, {{"refresh_token", "rt2"},
                       {"id_token", fakeIdToken("https://other.example",
                                                calls[0].form.queryItemValue("nonce"))}});
        QVERIFY(got.isEmpty());
        QVERIFY(!err.isEmpty());
        // The rotation still happened and is still saved.
        QCOMPARE(store->read(IdentitySession::storeKey("https://id.example")).value_or(""),
                 QString("rt2"));
    }

    void aRejectedRefreshTokenSignsOutAndFailsWhatWasQueued()
    {
        session->adopt("https://id.example", "rt1");
        QSignalSpy out(session, &IdentitySession::signedOut);
        QStringList errors;
        auto cb = [&](const QString&, const QString& e) { errors << e; };
        session->requestServerToken("https://a.example", cb);
        session->requestServerToken("https://b.example", cb);
        reply(0, 400, {{"error", "invalid_grant"}});
        QCOMPARE(out.count(), 1);
        QVERIFY(!session->isSignedIn());
        QCOMPARE(store->size(), 0);
        QCOMPARE(errors.size(), 2);
        QCOMPARE(calls.size(), 1); // the queued one never presented a dead token
    }

    // A server the account never consented to on the /authorize page: the
    // provider refuses the resource WITHOUT rotating. The session must stay,
    // the same token must be presented next (rotating locally would be a
    // replay), and what was queued behind it must still run.
    void aServerWithoutConsentFallsBackAndKeepsTheSameToken()
    {
        session->adopt("https://id.example", "rt1");
        QSignalSpy out(session, &IdentitySession::signedOut);
        QString firstErr, secondTok, secondErr;
        session->requestServerToken("https://new.example",
                                    [&](const QString& t, const QString& e) {
            QVERIFY(t.isEmpty());
            firstErr = e;
        });
        session->requestServerToken("https://known.example",
                                    [&](const QString& t, const QString& e) {
            secondTok = t;
            secondErr = e;
        });
        QCOMPARE(calls.size(), 1);
        reply(0, 400, {{"error", "invalid_target"},
                       {"error_description", "consent required for this server"}});
        QVERIFY(!firstErr.isEmpty());
        QCOMPARE(out.count(), 0);
        QVERIFY(session->isSignedIn());
        QCOMPARE(store->read(IdentitySession::storeKey("https://id.example")).value_or(""),
                 QString("rt1"));
        // The queue moved on, presenting the unspent token.
        QCOMPARE(calls.size(), 2);
        QCOMPARE(calls[1].form.queryItemValue("refresh_token"), QString("rt1"));
        succeed(1, "rt2");
        QVERIFY2(secondErr.isEmpty(), qPrintable(secondErr));
        QVERIFY(!secondTok.isEmpty());
    }

    void aNetworkFailureKeepsTheToken()
    {
        session->adopt("https://id.example", "rt1");
        QString err;
        session->requestServerToken("https://a.example",
                                    [&](const QString&, const QString& e) { err = e; });
        calls[0].done(0, QByteArray());
        QVERIFY(!err.isEmpty());
        QVERIFY(session->isSignedIn());
        QCOMPARE(store->read(IdentitySession::storeKey("https://id.example")).value_or(""),
                 QString("rt1"));
    }

    void aServerUsingTheSameIdIsRecognised()
    {
        session->adopt("https://id.bsfchat.com", "rt1");
        QVERIFY(session->servesProvider("https://ID.bsfchat.com/"));
        QVERIFY(!session->servesProvider("https://id.example.org"));
        QVERIFY(!session->servesProvider(""));
        session->signOut();
        QVERIFY(!session->servesProvider("https://id.bsfchat.com"));
    }
};

QTEST_GUILESS_MAIN(TestIdentitySession)
#include "test_identity_session.moc"
