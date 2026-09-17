#pragma once

// Turning what a user typed into "Add server" into a homeserver URL we can
// actually talk to, and reporting honestly what we found there.
//
// The bug this exists for (2026-09-17): someone typed `bsfchat.com` — the
// product's marketing domain, not the homeserver, which is
// `chat.bsfchat.com` — and got a password/register form. That form appeared
// because ServerManager::checkLoginFlows treated *any* failure of
// GET <url>/_matrix/client/v3/login as "assume password-only". The landing
// site answers that path with an nginx 404 HTML page, so the client
// cheerfully offered to register an account on a web server. Registration
// then failed with whatever the 404 body happened to say.
//
// Two things are fixed here:
//
//   1. Matrix well-known discovery, which the client never did at all
//      (`grep -rn well-known src/` was empty). `<origin>/.well-known/
//      matrix/client` names the real homeserver, so `bsfchat.com` can
//      resolve to `chat.bsfchat.com` the way every other Matrix client
//      already resolves it.
//   2. An honest probe outcome. "Couldn't reach it", "that isn't a Matrix
//      server" and "it only offers OIDC" are three different things and the
//      dialog has to be able to say which one happened. They are three
//      different LoginFlowKind values, not one silent `password = true`.
//
// Everything here is free of QtNetwork: I/O arrives as a FetchFn the caller
// supplies, so tests/test_server_discovery.cpp can drive the whole state
// machine from a table of canned responses with no event loop and no
// network. The real QNetworkAccessManager-backed FetchFn (with the bounded
// timeout) lives in HttpFetch.h.

#include <QByteArray>
#include <QString>

#include <functional>

namespace bsfchat {

// One HTTP response, reduced to what discovery cares about.
struct HttpResponse {
    // True when no HTTP response was obtained at all: DNS failure, refused
    // connection, TLS failure, or our own timeout. Distinct from a 404,
    // which is a perfectly good answer meaning "not here".
    bool transportError = false;
    int status = 0;
    QByteArray body;
    QString errorText;

    static HttpResponse unreachable(const QString& what)
    {
        HttpResponse r;
        r.transportError = true;
        r.errorText = what;
        return r;
    }
    static HttpResponse http(int status, const QByteArray& body = {})
    {
        HttpResponse r;
        r.status = status;
        r.body = body;
        return r;
    }
};

// GET `url`, then call `done` exactly once. Implementations must always
// call it — a fetch that silently drops the callback leaves the dialog
// spinning on "Checking server capabilities…" forever.
using FetchFn = std::function<void(const QString& url,
                                   std::function<void(const HttpResponse&)> done)>;

// What the server said when asked for its login flows.
enum class LoginFlowKind {
    Oidc,             // m.login.token only — the official BSFChat server
    Password,         // m.login.password only
    Both,             // both, so "Use password instead" is meaningful
    NoSupportedFlow,  // a real Matrix server, but nothing we can drive
    Unreachable,      // no answer, or the server answered 5xx
    NotAServer,       // answered, but not with Matrix login flows
};

// Stable lowercase wire names, for the QML signal. Kept as strings rather
// than an int enum so a QML handler reads as `outcome === "not_a_server"`
// instead of a magic number.
QString loginFlowKindName(LoginFlowKind kind);

struct FlowProbe {
    LoginFlowKind kind = LoginFlowKind::Unreachable;
    // The identity_provider advertised alongside m.login.token.
    QString identityProvider;
    // Detail for the inline message when something went wrong.
    QString detail;

    bool oidc() const { return kind == LoginFlowKind::Oidc || kind == LoginFlowKind::Both; }
    bool password() const { return kind == LoginFlowKind::Password || kind == LoginFlowKind::Both; }
    // Did we find a usable BSFChat/Matrix server at all?
    bool usable() const { return oidc() || password(); }
};

// Classify one response to GET <homeserver>/_matrix/client/v3/login.
// Pure — exported so the table test can hit it directly.
FlowProbe classifyLoginFlows(const HttpResponse& res);

enum class HomeserverSource {
    UserInput,  // what was typed, normalised
    WellKnown,  // <origin>/.well-known/matrix/client told us
};

struct Resolution {
    // The user's input, normalised (scheme added, trailing slashes gone).
    // Empty when the input could not be parsed as a server address.
    QString requestedUrl;
    // The URL to actually talk to. Empty only when requestedUrl is.
    QString homeserver;
    HomeserverSource source = HomeserverSource::UserInput;
    // org.bsfchat.identity.issuer from the well-known file, when present.
    QString identityIssuer;
    // Set only when something went wrong that the user should hear about:
    // a well-known file that named a homeserver which then did not answer.
    // A *missing* well-known file is ordinary and leaves this empty.
    QString note;

    // True when discovery sent us somewhere other than what was typed —
    // the case the dialog reports as "Found server at <resolved>".
    bool redirected() const
    {
        return source == HomeserverSource::WellKnown && homeserver != requestedUrl;
    }
    bool valid() const { return !homeserver.isEmpty(); }
};

struct Discovery {
    Resolution resolution;
    FlowProbe probe;
};

// `bsfchat.com` -> `https://bsfchat.com`; `chat.bsfchat.com/` ->
// `https://chat.bsfchat.com`; `http://localhost:8448` unchanged. Scheme and
// host are lowercased, an explicit port is kept (including a redundant
// :443), query and fragment are dropped, a path is kept minus its trailing
// slashes — a homeserver is allowed to live under one. Returns an empty
// string for input that is not an http(s) address at all, which callers
// must treat as "not a server".
QString normaliseServerUrl(const QString& input);

// scheme://host[:port] of an already-normalised URL. The well-known file is
// always at the origin root, never under the homeserver's path.
QString originOf(const QString& normalisedUrl);

class ServerDiscovery {
public:
    ServerDiscovery() = default;
    explicit ServerDiscovery(FetchFn fetch) : m_fetch(std::move(fetch)) {}

    // Normalise, then look for a well-known file. When one names a
    // homeserver we check that homeserver answers before believing it, so a
    // stale well-known pointing at a dead host falls back to the typed URL
    // with `note` explaining why rather than stranding the user on a host
    // that will never answer.
    void resolve(const QString& input, std::function<void(Resolution)> done) const;

    // resolve(), then probe the resolved homeserver's login flows.
    void resolveAndProbe(const QString& input, std::function<void(Discovery)> done) const;

    // Path suffixes, in one place so the test and the code agree.
    static constexpr const char* kWellKnownPath = "/.well-known/matrix/client";
    static constexpr const char* kLoginPath = "/_matrix/client/v3/login";
    static constexpr const char* kVersionsPath = "/_matrix/client/versions";

private:
    FetchFn m_fetch;
};

} // namespace bsfchat
