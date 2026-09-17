// Add-server URL resolution and the honesty of the login-flow probe.
//
// The incident, 2026-09-17: a user typed `bsfchat.com` into "Add server".
// That is the marketing domain; the homeserver is `chat.bsfchat.com`. The
// client had no well-known discovery at all, so it probed
// `https://bsfchat.com/_matrix/client/v3/login`, got nginx's HTML 404, and
// — because checkLoginFlows answered *every* error with
// `loginFlowsChecked(url, false, "", true)`, i.e. "assume password-only" —
// showed a registration form for a web server. Registering failed with the
// 404 page as the error text.
//
// So two properties are pinned here, both of them table-driven:
//
//   * the input the user types resolves to the homeserver we should talk
//     to, via .well-known when there is one and falling back honestly when
//     there is not (or when the one there lies); and
//   * the probe reports which of oidc / password / both / no_supported_flow
//     / unreachable / not_a_server actually happened, never inventing a
//     password flow that was not advertised.
//
// No network: ServerDiscovery takes its I/O as a FetchFn, so FakeHttp below
// answers from a table and records what was asked for. No event loop
// either — the callbacks run inline.

#include "net/ServerDiscovery.h"

#include <QtTest/QtTest>

#include <QHash>
#include <QStringList>

using namespace bsfchat;

namespace {

// The canned HTTP layer. Anything not in `routes` is a transport failure,
// which is what a dead host looks like from here.
class FakeHttp {
public:
    void route(const QString& url, const HttpResponse& res) { m_routes.insert(url, res); }
    void routeJson(const QString& url, int status, const QByteArray& body)
    {
        route(url, HttpResponse::http(status, body));
    }

    const QStringList& requests() const { return m_requests; }

    FetchFn fn()
    {
        return [this](const QString& url, std::function<void(const HttpResponse&)> done) {
            m_requests << url;
            auto it = m_routes.constFind(url);
            if (it == m_routes.constEnd()) {
                done(HttpResponse::unreachable(QStringLiteral("host not found")));
                return;
            }
            done(*it);
        };
    }

private:
    QHash<QString, HttpResponse> m_routes;
    QStringList m_requests;
};

// JSON bodies are written with single quotes and swapped to double quotes
// here. Not a style choice: moc 6.10.2 mis-lexes the quotes inside a
// R"(...)" literal, silently decides the file contains "no relevant
// classes", generates an empty .moc, and the test binary then fails to
// link with a missing vtable for the test class. Ordinary string literals
// are lexed correctly, and single quotes keep the JSON readable without a
// backslash on every character.
QByteArray json(const char* singleQuoted)
{
    return QByteArray(singleQuoted).replace('\'', '"');
}

// The real files, as curl sees them today.
QByteArray officialWellKnown()
{
    return json("{'m.homeserver':{'base_url':'https://chat.bsfchat.com'},"
                "'org.bsfchat.identity':{'issuer':'https://id.bsfchat.com'}}");
}
QByteArray versionsBody()
{
    return json("{'versions':['v1.12'],'unstable_features':{'bsfchat.server':true}}");
}
QByteArray oidcOnlyFlows()
{
    return json("{'flows':[{'identity_provider':'https://id.bsfchat.com','type':'m.login.token'}]}");
}
QByteArray nginx404()
{
    return "<html>\n<head><title>404 Not Found</title></head>\n<body>\n"
           "<center><h1>404 Not Found</h1></center>\n</body>\n</html>\n";
}

} // namespace

class TestServerDiscovery : public QObject {
    Q_OBJECT

private slots:
    // ---- normalisation -------------------------------------------------

    void normalise_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");

        QTest::newRow("bare host gets https")
            << "bsfchat.com" << "https://bsfchat.com";
        QTest::newRow("explicit https kept")
            << "https://bsfchat.com" << "https://bsfchat.com";
        QTest::newRow("trailing slash stripped")
            << "chat.bsfchat.com/" << "https://chat.bsfchat.com";
        QTest::newRow("many trailing slashes stripped")
            << "https://chat.bsfchat.com///" << "https://chat.bsfchat.com";
        QTest::newRow("explicit port preserved")
            << "http://localhost:8448" << "http://localhost:8448";
        QTest::newRow("bare host with port gets https")
            << "matrix.example.org:8448" << "https://matrix.example.org:8448";
        QTest::newRow("redundant :443 preserved")
            << "https://example.org:443" << "https://example.org:443";
        QTest::newRow("surrounding whitespace trimmed")
            << "  bsfchat.com \n" << "https://bsfchat.com";
        QTest::newRow("host lowercased")
            << "HTTPS://Chat.BSFChat.COM" << "https://chat.bsfchat.com";
        QTest::newRow("path kept, trailing slash not")
            << "https://example.org/matrix/" << "https://example.org/matrix";
        QTest::newRow("query and fragment dropped")
            << "https://example.org/?utm=1#x" << "https://example.org";

        // Not addresses we can talk to. Callers read the empty string as
        // "not a server" rather than building https:///_matrix/... out of it.
        QTest::newRow("empty") << "" << "";
        QTest::newRow("whitespace only") << "   " << "";
        QTest::newRow("unsupported scheme") << "ftp://example.org" << "";
        QTest::newRow("matrix id is not a url") << "@bob:example.org" << "";
        QTest::newRow("scheme with no host") << "https://" << "";
    }

    void normalise()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        QCOMPARE(normaliseServerUrl(input), expected);
    }

    void origin_dropsPath()
    {
        // The well-known file lives at the origin root even when the
        // homeserver itself is under a path.
        QCOMPARE(originOf(QStringLiteral("https://example.org/matrix")),
                 QStringLiteral("https://example.org"));
        QCOMPARE(originOf(QStringLiteral("http://localhost:8448")),
                 QStringLiteral("http://localhost:8448"));
        QCOMPARE(originOf(QString()), QString());
    }

    // ---- resolution ----------------------------------------------------

    // Every row: what the user typed, what the well-known endpoint answers,
    // and where we must end up. `wellKnownStatus` of 0 means the host does
    // not answer that path at all.
    void resolve_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<int>("wellKnownStatus");
        QTest::addColumn<QByteArray>("wellKnownBody");
        QTest::addColumn<bool>("baseUrlAlive");
        QTest::addColumn<QString>("expectedHomeserver");
        QTest::addColumn<bool>("expectedRedirect");
        QTest::addColumn<bool>("expectNote");

        QTest::newRow("well-known present -> real homeserver")
            << "bsfchat.com" << 200 << officialWellKnown() << true
            << "https://chat.bsfchat.com" << true << false;

        QTest::newRow("well-known present, scheme added by us")
            << "https://bsfchat.com" << 200 << officialWellKnown() << true
            << "https://chat.bsfchat.com" << true << false;

        // The state of the world as this test was written: the file is
        // announced but nginx still 404s it. Falling back must be silent —
        // most self-hosted servers have no well-known and that is normal.
        QTest::newRow("well-known 404 -> input is the homeserver")
            << "chat.bsfchat.com/" << 404 << nginx404() << false
            << "https://chat.bsfchat.com" << false << false;

        QTest::newRow("well-known malformed json -> fall back quietly")
            << "bsfchat.com" << 200 << QByteArray("{not json at all") << false
            << "https://bsfchat.com" << false << false;

        QTest::newRow("well-known html with 200 -> fall back quietly")
            << "bsfchat.com" << 200 << nginx404() << false
            << "https://bsfchat.com" << false << false;

        QTest::newRow("well-known without m.homeserver -> fall back quietly")
            << "bsfchat.com" << 200 << json("{'m.identity_server':{}}") << false
            << "https://bsfchat.com" << false << false;

        // The one fallback the user has to hear about: the file named a
        // homeserver and that homeserver is gone.
        QTest::newRow("base_url dead -> fall back and report")
            << "bsfchat.com" << 200 << officialWellKnown() << false
            << "https://bsfchat.com" << false << true;

        QTest::newRow("base_url unusable -> fall back and report")
            << "bsfchat.com" << 200 << json("{'m.homeserver':{'base_url':'not a url'}}")
            << false << "https://bsfchat.com" << false << true;

        QTest::newRow("host does not answer at all -> input, no note")
            << "nope.invalid" << 0 << QByteArray() << false
            << "https://nope.invalid" << false << false;

        QTest::newRow("explicit port preserved through resolution")
            << "http://localhost:8448" << 404 << QByteArray() << false
            << "http://localhost:8448" << false << false;

        QTest::newRow("well-known naming the same host is not a redirect")
            << "chat.bsfchat.com" << 200
            << json("{'m.homeserver':{'base_url':'https://chat.bsfchat.com/'}}") << true
            << "https://chat.bsfchat.com" << false << false;
    }

    void resolve()
    {
        QFETCH(QString, input);
        QFETCH(int, wellKnownStatus);
        QFETCH(QByteArray, wellKnownBody);
        QFETCH(bool, baseUrlAlive);
        QFETCH(QString, expectedHomeserver);
        QFETCH(bool, expectedRedirect);
        QFETCH(bool, expectNote);

        FakeHttp http;
        const QString origin = originOf(normaliseServerUrl(input));
        if (wellKnownStatus != 0) {
            http.routeJson(origin + QStringLiteral("/.well-known/matrix/client"),
                           wellKnownStatus, wellKnownBody);
        }
        if (baseUrlAlive) {
            http.routeJson(QStringLiteral("https://chat.bsfchat.com/_matrix/client/versions"),
                           200, versionsBody());
        }

        ServerDiscovery discovery(http.fn());
        std::optional<Resolution> got;
        discovery.resolve(input, [&got](Resolution r) { got = std::move(r); });

        QVERIFY2(got.has_value(), "resolve() must always call its callback");
        QCOMPARE(got->homeserver, expectedHomeserver);
        QCOMPARE(got->redirected(), expectedRedirect);
        QCOMPARE(got->note.isEmpty(), !expectNote);
    }

    void resolve_readsWellKnownAtOriginNotUnderPath()
    {
        FakeHttp http;
        ServerDiscovery discovery(http.fn());
        std::optional<Resolution> got;
        discovery.resolve(QStringLiteral("https://example.org/matrix"),
                          [&got](Resolution r) { got = std::move(r); });

        QVERIFY(got.has_value());
        QCOMPARE(http.requests().size(), 1);
        QCOMPARE(http.requests().first(),
                 QStringLiteral("https://example.org/.well-known/matrix/client"));
        // The path survives into the homeserver URL even though the
        // well-known lookup ignored it.
        QCOMPARE(got->homeserver, QStringLiteral("https://example.org/matrix"));
    }

    void resolve_capturesIdentityIssuer()
    {
        FakeHttp http;
        http.routeJson(QStringLiteral("https://bsfchat.com/.well-known/matrix/client"), 200,
                       officialWellKnown());
        http.routeJson(QStringLiteral("https://chat.bsfchat.com/_matrix/client/versions"), 200,
                       versionsBody());

        ServerDiscovery discovery(http.fn());
        std::optional<Resolution> got;
        discovery.resolve(QStringLiteral("bsfchat.com"),
                          [&got](Resolution r) { got = std::move(r); });

        QVERIFY(got.has_value());
        QCOMPARE(got->identityIssuer, QStringLiteral("https://id.bsfchat.com"));
        QCOMPARE(got->source, HomeserverSource::WellKnown);
    }

    void resolve_invalidInputNeverHitsTheNetwork()
    {
        FakeHttp http;
        ServerDiscovery discovery(http.fn());
        std::optional<Resolution> got;
        discovery.resolve(QStringLiteral("@bob:example.org"),
                          [&got](Resolution r) { got = std::move(r); });

        QVERIFY(got.has_value());
        QVERIFY(!got->valid());
        QVERIFY(http.requests().isEmpty());
    }

    // ---- flow classification -------------------------------------------

    void classify_data()
    {
        QTest::addColumn<bool>("transportError");
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<int>("expectedKind");
        QTest::addColumn<QString>("expectedProvider");

        QTest::newRow("official server: token only")
            << false << 200 << oidcOnlyFlows() << int(LoginFlowKind::Oidc)
            << "https://id.bsfchat.com";

        QTest::newRow("password only")
            << false << 200 << json("{'flows':[{'type':'m.login.password'}]}")
            << int(LoginFlowKind::Password) << "";

        QTest::newRow("both")
            << false << 200
            << json("{'flows':[{'type':'m.login.password'},"
                    "{'type':'m.login.token','identity_provider':'https://id.x'}]}")
            << int(LoginFlowKind::Both) << "https://id.x";

        QTest::newRow("a matrix server offering nothing we drive")
            << false << 200 << json("{'flows':[{'type':'m.login.sso'}]}")
            << int(LoginFlowKind::NoSupportedFlow) << "";

        QTest::newRow("empty flows array")
            << false << 200 << json("{'flows':[]}")
            << int(LoginFlowKind::NoSupportedFlow) << "";

        // THE regression: the landing site. Must never read as "password".
        QTest::newRow("landing site html 404")
            << false << 404 << nginx404() << int(LoginFlowKind::NotAServer) << "";

        QTest::newRow("html served with 200")
            << false << 200 << nginx404() << int(LoginFlowKind::NotAServer) << "";

        QTest::newRow("json without flows")
            << false << 404 << json("{'errcode':'M_UNRECOGNIZED','error':'Unknown'}")
            << int(LoginFlowKind::NotAServer) << "";

        QTest::newRow("empty body")
            << false << 200 << QByteArray() << int(LoginFlowKind::NotAServer) << "";

        QTest::newRow("dns failure")
            << true << 0 << QByteArray() << int(LoginFlowKind::Unreachable) << "";

        // A proxy in front of a stopped homeserver. The server exists; it
        // is down. Telling the user "no BSFChat server found" would send
        // them hunting for a typo that isn't there.
        QTest::newRow("bad gateway is unreachable, not missing")
            << false << 502 << QByteArray("<html>502</html>") << int(LoginFlowKind::Unreachable)
            << "";
    }

    void classify()
    {
        QFETCH(bool, transportError);
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(int, expectedKind);
        QFETCH(QString, expectedProvider);

        const HttpResponse res = transportError
            ? HttpResponse::unreachable(QStringLiteral("boom"))
            : HttpResponse::http(status, body);

        const FlowProbe probe = classifyLoginFlows(res);
        QCOMPARE(int(probe.kind), expectedKind);
        QCOMPARE(probe.identityProvider, expectedProvider);

        // The invariant the dialog leans on: only an advertised password
        // flow may ever put a register form on screen.
        const bool mayRegister = probe.password();
        QCOMPARE(mayRegister,
                 expectedKind == int(LoginFlowKind::Password)
                     || expectedKind == int(LoginFlowKind::Both));
    }

    void kindNamesAreStable()
    {
        // QML compares these as strings; renaming one silently breaks the
        // dialog's messaging with no compile error anywhere.
        QCOMPARE(loginFlowKindName(LoginFlowKind::Oidc), QStringLiteral("oidc"));
        QCOMPARE(loginFlowKindName(LoginFlowKind::Password), QStringLiteral("password"));
        QCOMPARE(loginFlowKindName(LoginFlowKind::Both), QStringLiteral("both"));
        QCOMPARE(loginFlowKindName(LoginFlowKind::NoSupportedFlow),
                 QStringLiteral("no_supported_flow"));
        QCOMPARE(loginFlowKindName(LoginFlowKind::Unreachable), QStringLiteral("unreachable"));
        QCOMPARE(loginFlowKindName(LoginFlowKind::NotAServer), QStringLiteral("not_a_server"));
    }

    // ---- end to end -----------------------------------------------------

    void probe_theReportedBug()
    {
        // `bsfchat.com` typed into Add server, with the well-known file
        // live: resolves to chat.bsfchat.com, reports OIDC, offers no
        // password flow, and says it moved.
        FakeHttp http;
        http.routeJson(QStringLiteral("https://bsfchat.com/.well-known/matrix/client"), 200,
                       officialWellKnown());
        http.routeJson(QStringLiteral("https://chat.bsfchat.com/_matrix/client/versions"), 200,
                       versionsBody());
        http.routeJson(QStringLiteral("https://chat.bsfchat.com/_matrix/client/v3/login"), 200,
                       oidcOnlyFlows());

        ServerDiscovery discovery(http.fn());
        std::optional<Discovery> got;
        discovery.resolveAndProbe(QStringLiteral("bsfchat.com"),
                                  [&got](Discovery d) { got = std::move(d); });

        QVERIFY(got.has_value());
        QCOMPARE(got->resolution.homeserver, QStringLiteral("https://chat.bsfchat.com"));
        QVERIFY(got->resolution.redirected());
        QCOMPARE(got->probe.kind, LoginFlowKind::Oidc);
        QCOMPARE(got->probe.identityProvider, QStringLiteral("https://id.bsfchat.com"));
        QVERIFY(!got->probe.password());
    }

    void probe_theReportedBug_withoutWellKnown()
    {
        // Same input, but with the well-known file still 404ing (which is
        // what curl gets today). We cannot rescue the user's typo, but the
        // old code's "assume password-only" is gone: the outcome is
        // not_a_server, so the dialog says so instead of offering to
        // register them on a web server.
        FakeHttp http;
        http.routeJson(QStringLiteral("https://bsfchat.com/.well-known/matrix/client"), 404,
                       nginx404());
        http.routeJson(QStringLiteral("https://bsfchat.com/_matrix/client/v3/login"), 404,
                       nginx404());

        ServerDiscovery discovery(http.fn());
        std::optional<Discovery> got;
        discovery.resolveAndProbe(QStringLiteral("bsfchat.com"),
                                  [&got](Discovery d) { got = std::move(d); });

        QVERIFY(got.has_value());
        QCOMPARE(got->resolution.homeserver, QStringLiteral("https://bsfchat.com"));
        QCOMPARE(got->probe.kind, LoginFlowKind::NotAServer);
        QVERIFY(!got->probe.password());
        QVERIFY(!got->probe.usable());
    }

    void probe_selfHostedPasswordServer()
    {
        // The dev loop: no well-known, password flow, http on a port.
        FakeHttp http;
        http.routeJson(QStringLiteral("http://localhost:8448/_matrix/client/v3/login"), 200,
                       json("{'flows':[{'type':'m.login.password'}]}"));

        ServerDiscovery discovery(http.fn());
        std::optional<Discovery> got;
        discovery.resolveAndProbe(QStringLiteral("http://localhost:8448/"),
                                  [&got](Discovery d) { got = std::move(d); });

        QVERIFY(got.has_value());
        QCOMPARE(got->resolution.homeserver, QStringLiteral("http://localhost:8448"));
        QCOMPARE(got->probe.kind, LoginFlowKind::Password);
        QVERIFY(got->probe.password());
    }

    void probe_deadHostIsUnreachableNotMissing()
    {
        FakeHttp http;  // routes nothing: every request is a transport error
        ServerDiscovery discovery(http.fn());
        std::optional<Discovery> got;
        discovery.resolveAndProbe(QStringLiteral("chat.example.invalid"),
                                  [&got](Discovery d) { got = std::move(d); });

        QVERIFY(got.has_value());
        QCOMPARE(got->probe.kind, LoginFlowKind::Unreachable);
    }

    void probe_nonsenseInputNeverHitsTheNetwork()
    {
        FakeHttp http;
        ServerDiscovery discovery(http.fn());
        std::optional<Discovery> got;
        discovery.resolveAndProbe(QStringLiteral("   "),
                                  [&got](Discovery d) { got = std::move(d); });

        QVERIFY(got.has_value());
        QCOMPARE(got->probe.kind, LoginFlowKind::NotAServer);
        QVERIFY(http.requests().isEmpty());
    }

    void probe_withNoFetchStillAnswers()
    {
        // A default-constructed ServerDiscovery has no I/O. It must still
        // call back — a dropped callback is a dialog stuck on "Checking…".
        ServerDiscovery discovery;
        std::optional<Discovery> got;
        discovery.resolveAndProbe(QStringLiteral("example.org"),
                                  [&got](Discovery d) { got = std::move(d); });

        QVERIFY(got.has_value());
        QCOMPARE(got->probe.kind, LoginFlowKind::Unreachable);
    }
};

QTEST_APPLESS_MAIN(TestServerDiscovery)
#include "test_server_discovery.moc"
