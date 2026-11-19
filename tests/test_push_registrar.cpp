// PushRegistrar against a fake homeserver. Every request is held until the
// test answers it, so the order of register / delete is observable.
#include <QtTest/QtTest>
#include <QJsonArray>
#include <QJsonDocument>

#include "net/PushRegistrar.h"

using namespace bsfchat::client::push;

namespace {

struct FakeTransport : PushTransport {
    struct Call {
        QString account;
        QJsonObject body;
        Done done;
    };
    QList<Call> calls;
    void setPusher(const QString& account, const QJsonObject& body, Done done) override
    {
        calls.append({account, body, std::move(done)});
    }
    // Answer the oldest outstanding call.
    Call answer(int status, const QByteArray& body = "{}", bool transportError = false)
    {
        Call c = calls.takeFirst();
        c.done(Reply{status, body, transportError});
        return c;
    }
};

PusherIdentity androidId()
{
    PusherIdentity id;
    id.platform = Platform::Android;
    id.appId = QStringLiteral("com.bsfchat.app");
    id.deviceDisplayName = QStringLiteral("Pixel 8");
    id.lang = QStringLiteral("en-GB");
    return id;
}

bool isDeletion(const QJsonObject& b) { return b.value("kind").isNull(); }

QByteArray matrixError(const char* text)
{
    return QJsonDocument(QJsonObject{{"errcode", "M_INVALID_PARAM"}, {"error", text}}).toJson();
}

StatusInputs androidReady()
{
    StatusInputs in;
    in.compiledIn = true;
    in.notificationsOn = true;
    in.hasToken = true;
    return in;
}
AccountStatus acct(const char* host, PushRegistrar::State st,
                          const char* err = "")
{
    AccountStatus a;
    a.host = QString::fromLatin1(host);
    a.state = st;
    a.lastError = QString::fromLatin1(err);
    return a;
}

} // namespace

class TestPushRegistrar : public QObject {
    Q_OBJECT
private slots:
    void androidBodyIsPrivateByDefault()
    {
        const auto b = pusherBody(androidId(), "tok", kDefaultGatewayUrl, PayloadTier::EventIdOnly);
        QCOMPARE(b.value("kind").toString(), QStringLiteral("http"));
        QCOMPARE(b.value("app_id").toString(), QStringLiteral("com.bsfchat.app"));
        QCOMPARE(b.value("pushkey").toString(), QStringLiteral("tok"));
        QCOMPARE(b.value("app_display_name").toString(), QStringLiteral("BSFChat"));
        QCOMPARE(b.value("device_display_name").toString(), QStringLiteral("Pixel 8"));
        QCOMPARE(b.value("lang").toString(), QStringLiteral("en-GB"));
        QVERIFY(!b.contains("append"));
        const auto data = b.value("data").toObject();
        QCOMPARE(data.value("url").toString(),
                 QStringLiteral("https://push.bsfchat.com/_matrix/push/v1/notify"));
        QCOMPARE(data.value("format").toString(), QStringLiteral("event_id_only"));
        QVERIFY(!data.contains("default_payload"));
    }

    void iosBodyCarriesGenericAlert()
    {
        PusherIdentity id = androidId();
        id.platform = Platform::Ios;
        id.appId = QStringLiteral("com.bsfchat.app.ios");
        const auto b = pusherBody(id, "abcd", kDefaultGatewayUrl, PayloadTier::Names);
        const auto data = b.value("data").toObject();
        QCOMPARE(data.value("format").toString(), QStringLiteral("com.bsfchat.names"));
        const auto aps = data.value("default_payload").toObject().value("aps").toObject();
        QCOMPARE(aps.value("alert").toObject().value("body").toString(), QStringLiteral("New message"));
        QCOMPARE(aps.value("mutable-content").toInt(), 1);
        QCOMPARE(aps.value("sound").toString(), QStringLiteral("default"));
    }

    void gatewayNormalisation()
    {
        QCOMPARE(normaliseGatewayUrl("https://push.bsfchat.com"),
                 QStringLiteral("https://push.bsfchat.com/_matrix/push/v1/notify"));
        QCOMPARE(normaliseGatewayUrl(" https://push.example.org/custom/notify "),
                 QStringLiteral("https://push.example.org/custom/notify"));
        QCOMPARE(normaliseGatewayUrl("ftp://x"), QString());
        QCOMPARE(normaliseGatewayUrl("not a url"), QString());
        QCOMPARE(normaliseGatewayUrl(""), QString());
    }

    void classify()
    {
        QCOMPARE(classifyReply(200, "{}", false), ReplyKind::Ok);
        QCOMPARE(classifyReply(0, {}, true), ReplyKind::Retryable);
        QCOMPARE(classifyReply(502, {}, false), ReplyKind::Retryable);
        QCOMPARE(classifyReply(429, {}, false), ReplyKind::Retryable);
        QCOMPARE(classifyReply(401, {}, false), ReplyKind::AuthGone);
        QCOMPARE(classifyReply(400, matrixError("data.url is not an allowed push gateway for this server"), false),
                 ReplyKind::GatewayRefused);
        QCOMPARE(classifyReply(400, matrixError("this server has no push.allowed_gateway_prefixes configured, so no push gateway may be registered"), false),
                 ReplyKind::GatewayRefused);
        QCOMPARE(classifyReply(400, matrixError("data.format must be omitted or one of \"event_id_only\"; this server's push.max_payload is \"event_id_only\""), false),
                 ReplyKind::FormatRefused);
        QCOMPARE(classifyReply(404, {}, false), ReplyKind::Refused);
    }

    void nothingWithoutTokenOrConsent()
    {
        FakeTransport t;
        PushRegistrar r(&t, androidId());
        r.setAutoRetry(false);
        r.addAccount("a");
        QVERIFY(t.calls.isEmpty());
        r.setToken("tok");
        QVERIFY(t.calls.isEmpty());           // not wanted yet
        r.setWanted(true);
        QCOMPARE(t.calls.size(), 1);
        QCOMPARE(r.state("a"), PushRegistrar::State::Busy);
    }

    void registersAndSettles()
    {
        FakeTransport t;
        PushRegistrar r(&t, androidId());
        r.setAutoRetry(false);
        QSignalSpy persisted(&r, &PushRegistrar::registeredPushkeyChanged);
        r.setWanted(true);
        r.setToken("tok");
        r.addAccount("a");
        auto c = t.answer(200);
        QCOMPARE(c.body.value("pushkey").toString(), QStringLiteral("tok"));
        QVERIFY(t.calls.isEmpty());
        QCOMPARE(r.state("a"), PushRegistrar::State::Registered);
        QCOMPARE(persisted.size(), 1);
        QCOMPARE(persisted.at(0).at(1).toString(), QStringLiteral("tok"));
        // Re-adding and re-setting the same inputs sends nothing.
        r.addAccount("a");
        r.setToken("tok");
        QVERIFY(t.calls.isEmpty());
    }

    void tokenRotationRegistersNewThenDeletesOld()
    {
        FakeTransport t;
        PushRegistrar r(&t, androidId());
        r.setAutoRetry(false);
        r.setWanted(true);
        r.setToken("old");
        r.addAccount("a");
        t.answer(200);
        r.setToken("new");
        QCOMPARE(t.calls.size(), 1);
        QCOMPARE(t.calls.first().body.value("pushkey").toString(), QStringLiteral("new"));
        QVERIFY(!isDeletion(t.calls.first().body));
        t.answer(200);
        QCOMPARE(t.calls.size(), 1);
        QVERIFY(isDeletion(t.calls.first().body));
        QCOMPARE(t.calls.first().body.value("pushkey").toString(), QStringLiteral("old"));
        QCOMPARE(t.calls.first().body.value("app_id").toString(), QStringLiteral("com.bsfchat.app"));
        t.answer(200);
        QVERIFY(t.calls.isEmpty());
        QCOMPARE(r.state("a"), PushRegistrar::State::Registered);
    }

    void rotationWhileNotRunningDeletesPersistedKey()
    {
        FakeTransport t;
        PushRegistrar r(&t, androidId());
        r.setAutoRetry(false);
        r.setWanted(true);
        r.setToken("new");
        r.addAccount("a", "old");          // what the previous run left registered
        t.answer(200);                      // register new
        QCOMPARE(t.calls.size(), 1);
        QVERIFY(isDeletion(t.calls.first().body));
        QCOMPARE(t.calls.first().body.value("pushkey").toString(), QStringLiteral("old"));
    }

    void restartRefreshesSameKeyWithoutDeleting()
    {
        FakeTransport t;
        PushRegistrar r(&t, androidId());
        r.setAutoRetry(false);
        r.setWanted(true);
        r.setToken("tok");
        r.addAccount("a", "tok");
        QCOMPARE(t.calls.size(), 1);
        QVERIFY(!isDeletion(t.calls.first().body));
        t.answer(200);
        QVERIFY(t.calls.isEmpty());
    }

    void optOutDeletes()
    {
        FakeTransport t;
        PushRegistrar r(&t, androidId());
        r.setAutoRetry(false);
        QSignalSpy persisted(&r, &PushRegistrar::registeredPushkeyChanged);
        r.setWanted(true);
        r.setToken("tok");
        r.addAccount("a");
        t.answer(200);
        r.setWanted(false);
        QCOMPARE(t.calls.size(), 1);
        QVERIFY(isDeletion(t.calls.first().body));
        t.answer(200);
        QCOMPARE(r.state("a"), PushRegistrar::State::Idle);
        QCOMPARE(persisted.last().at(1).toString(), QString());
    }

    void signOutHandsBackDeletions()
    {
        FakeTransport t;
        PushRegistrar r(&t, androidId());
        r.setAutoRetry(false);
        r.setWanted(true);
        r.setToken("tok");
        r.addAccount("a");
        t.answer(200);
        const auto owed = r.takeAccount("a");
        QCOMPARE(owed.size(), 1);
        QVERIFY(isDeletion(owed.first()));
        QCOMPARE(owed.first().value("pushkey").toString(), QStringLiteral("tok"));
        QVERIFY(!r.hasAccount("a"));
        // Nothing more happens for a forgotten account.
        r.setToken("other");
        QVERIFY(t.calls.isEmpty());
    }

    void lateReplyAfterSignOutIsIgnored()
    {
        FakeTransport t;
        PushRegistrar r(&t, androidId());
        r.setAutoRetry(false);
        r.setWanted(true);
        r.setToken("tok");
        r.addAccount("a");
        const auto owed = r.takeAccount("a");   // registration still in flight
        QCOMPARE(owed.size(), 1);               // owes the in-flight token too
        t.answer(200);
        QVERIFY(!r.hasAccount("a"));
        QVERIFY(t.calls.isEmpty());
    }

    void gatewayRefusedIsQuietAndNotRetried()
    {
        FakeTransport t;
        PushRegistrar r(&t, androidId());
        r.setAutoRetry(false);
        r.setWanted(true);
        r.setToken("tok");
        r.addAccount("a");
        t.answer(400, matrixError("data.url is not an allowed push gateway for this server"));
        QVERIFY(t.calls.isEmpty());
        QCOMPARE(r.state("a"), PushRegistrar::State::GatewayRefused);
        r.retryFailed();
        r.setTier(PayloadTier::Names);          // still the same gateway
        QVERIFY(t.calls.isEmpty());
        r.setGatewayUrl("https://push.example.org");
        QCOMPARE(t.calls.size(), 1);
        QCOMPARE(t.calls.first().body.value("data").toObject().value("url").toString(),
                 QStringLiteral("https://push.example.org/_matrix/push/v1/notify"));
    }

    void namesRefusedFallsBackToEventIdOnly()
    {
        FakeTransport t;
        PushRegistrar r(&t, androidId());
        r.setAutoRetry(false);
        r.setWanted(true);
        r.setTier(PayloadTier::Names);
        r.setToken("tok");
        r.addAccount("a");
        auto c = t.answer(400, matrixError("data.format must be omitted or one of \"event_id_only\""));
        QCOMPARE(c.body.value("data").toObject().value("format").toString(),
                 QStringLiteral("com.bsfchat.names"));
        QCOMPARE(t.calls.size(), 1);
        QCOMPARE(t.calls.first().body.value("data").toObject().value("format").toString(),
                 QStringLiteral("event_id_only"));
        t.answer(200);
        QCOMPARE(r.state("a"), PushRegistrar::State::Registered);
        QVERIFY(r.namesRefused("a"));
    }

    void transientFailureRetries()
    {
        FakeTransport t;
        PushRegistrar r(&t, androidId());
        r.setAutoRetry(false);
        r.setWanted(true);
        r.setToken("tok");
        r.addAccount("a");
        t.answer(0, {}, true);
        QCOMPARE(r.state("a"), PushRegistrar::State::Failed);
        QVERIFY(t.calls.isEmpty());
        r.retryFailed();
        QCOMPARE(t.calls.size(), 1);
        t.answer(200);
        QCOMPARE(r.state("a"), PushRegistrar::State::Registered);
    }

    void authGoneIsNotRetried()
    {
        FakeTransport t;
        PushRegistrar r(&t, androidId());
        r.setAutoRetry(false);
        r.setWanted(true);
        r.setToken("tok");
        r.addAccount("a");
        t.answer(401, R"({"errcode":"M_UNKNOWN_TOKEN","error":"gone"})");
        QCOMPARE(r.state("a"), PushRegistrar::State::Refused);
        r.retryFailed();
        QVERIFY(t.calls.isEmpty());
        // A re-signed-in account is dropped and re-added: fresh start.
        r.dropAccount("a");
        r.addAccount("a");
        QCOMPARE(t.calls.size(), 1);
    }

    void accountsAreIndependent()
    {
        FakeTransport t;
        PushRegistrar r(&t, androidId());
        r.setAutoRetry(false);
        r.setWanted(true);
        r.setToken("tok");
        r.addAccount("a");
        r.addAccount("b");
        QCOMPARE(t.calls.size(), 2);
        // b's server refuses the gateway; a is unaffected.
        FakeTransport::Call ca = t.calls.takeFirst();
        FakeTransport::Call cb = t.calls.takeFirst();
        cb.done({400, matrixError("data.url is not an allowed push gateway for this server"), false});
        ca.done({200, "{}", false});
        QCOMPARE(r.state("a"), PushRegistrar::State::Registered);
        QCOMPARE(r.state("b"), PushRegistrar::State::GatewayRefused);
    }

    // ---- Status line and Android background decision (audit 3 #9) ----

    void backgroundStaysOffWhenEveryServerHasAPusher()
    {
        StatusInputs in = androidReady();
        in.accounts = {acct("a.org", PushRegistrar::State::Registered),
                       acct("b.org", PushRegistrar::State::Registered)};
        const auto s = summariseStatus(in);
        QVERIFY(!s.backgroundSyncWanted);
        QCOMPARE(s.registered, 2);
        QCOMPARE(s.text, QStringLiteral("Push is on for 2 server(s)."));
    }

    void refusedGatewayKeepsTheBackgroundConnection()
    {
        // The bug: push replaced SyncService even for a server that
        // refuses the gateway, which then never notified at all.
        StatusInputs in = androidReady();
        in.accounts = {acct("a.org", PushRegistrar::State::Registered),
                       acct("self.host", PushRegistrar::State::GatewayRefused, "not allowed")};
        const auto s = summariseStatus(in);
        QVERIFY(s.backgroundSyncWanted);
        QCOMPARE(s.onBackground, QStringList{"self.host"});
        QCOMPARE(s.refused, 1);
        QVERIFY(s.text.contains("self.host does not allow this push gateway"));
        QVERIFY(s.text.contains("background connection for self.host"));
    }

    void refusedAndFailedAreCountedWithReasons()
    {
        StatusInputs in = androidReady();
        in.accounts = {acct("r.org", PushRegistrar::State::Refused, "M_UNKNOWN"),
                       acct("f.org", PushRegistrar::State::Failed, "network error")};
        const auto s = summariseStatus(in);
        QVERIFY(s.backgroundSyncWanted);
        QCOMPARE(s.refused, 1);
        QCOMPARE(s.failed, 1);
        QVERIFY(!s.text.contains("Push is on"));
        QVERIFY(s.text.contains("1 server(s) refused push: r.org (M_UNKNOWN)."));
        QVERIFY(s.text.contains("Could not register push with 1 server(s), retrying: "
                                "f.org (network error)."));
    }

    void invalidGatewayOrRetryingTokenUsesBackground()
    {
        StatusInputs in = androidReady();
        in.gatewayValid = false;
        in.accounts = {acct("a.org", PushRegistrar::State::Idle)};
        auto s = summariseStatus(in);
        QVERIFY(s.backgroundSyncWanted);
        QVERIFY(s.text.contains("not valid"));

        in = androidReady();
        in.hasToken = false;
        in.tokenRetrying = true;
        s = summariseStatus(in);
        QVERIFY(s.backgroundSyncWanted);
        QVERIFY(s.text.contains("retrying"));

        // Merely waiting for the first token is not a reason to start it.
        in.tokenRetrying = false;
        s = summariseStatus(in);
        QVERIFY(!s.backgroundSyncWanted);
        QCOMPARE(s.text, QStringLiteral("Waiting for the push service…"));
    }

    void iosNeverWantsBackgroundAndExplainsRefusal()
    {
        StatusInputs in = androidReady();
        in.ios = true;
        in.accounts = {acct("self.host", PushRegistrar::State::GatewayRefused)};
        const auto s = summariseStatus(in);
        QVERIFY(!s.backgroundSyncWanted);
        QVERIFY(s.onBackground.isEmpty());
        QVERIFY(s.text.contains("only notify you while BSFChat is open"));
    }

    void blockedPermissionIsReportedFirst()
    {
        StatusInputs in = androidReady();
        in.permissionBlocked = true;
        in.accounts = {acct("a.org", PushRegistrar::State::Registered)};
        QCOMPARE(summariseStatus(in).text,
                 QStringLiteral("Notifications are blocked in system settings."));
    }

    void userChoiceAndOffStates()
    {
        StatusInputs in = androidReady();
        in.backgroundChosen = true;
        auto s = summariseStatus(in);
        QVERIFY(s.backgroundSyncWanted);
        QCOMPARE(s.text, QStringLiteral("Using a background connection instead of push."));

        in = androidReady();
        in.notificationsOn = false;
        s = summariseStatus(in);
        QVERIFY(!s.backgroundSyncWanted);
        QVERIFY(s.text.isEmpty());

        in = androidReady();
        in.compiledIn = false;
        QVERIFY(summariseStatus(in).backgroundSyncWanted);
    }

    void backoffGrowsAndCaps()
    {
        QCOMPARE(PushRegistrar::backoffMs(1), 5000);
        QCOMPARE(PushRegistrar::backoffMs(2), 10000);
        QCOMPARE(PushRegistrar::backoffMs(100), 30 * 60 * 1000);
    }
};

QTEST_GUILESS_MAIN(TestPushRegistrar)
#include "test_push_registrar.moc"
