#include "net/ServerDiscovery.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLatin1String>
#include <QUrl>

namespace bsfchat {

QString loginFlowKindName(LoginFlowKind kind)
{
    switch (kind) {
    case LoginFlowKind::Oidc: return QStringLiteral("oidc");
    case LoginFlowKind::Password: return QStringLiteral("password");
    case LoginFlowKind::Both: return QStringLiteral("both");
    case LoginFlowKind::NoSupportedFlow: return QStringLiteral("no_supported_flow");
    case LoginFlowKind::Unreachable: return QStringLiteral("unreachable");
    case LoginFlowKind::NotAServer: return QStringLiteral("not_a_server");
    }
    return QStringLiteral("unreachable");
}

QString normaliseServerUrl(const QString& input)
{
    QString s = input.trimmed();
    if (s.isEmpty()) return {};

    // A bare host[:port] is what people type. Default to https — the
    // add-server flow has always done this (see updateServerUrl), and
    // http:// is only ever typed deliberately, for localhost.
    if (!s.contains(QStringLiteral("://"))) s = QStringLiteral("https://") + s;

    const QUrl url(s, QUrl::StrictMode);
    if (!url.isValid()) return {};

    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) return {};
    if (url.host().isEmpty()) return {};

    QString out = scheme + QStringLiteral("://") + url.host().toLower();
    // An explicit port survives, including a redundant :443 — the user may
    // be pointing at a second homeserver on one host, and silently dropping
    // it would send them somewhere else.
    if (url.port() != -1) out += QLatin1Char(':') + QString::number(url.port());

    // Query and fragment are noise on a homeserver base URL (people paste
    // them in from a browser); a path is not, so keep it, minus the
    // trailing slashes that would double up when we append an API path.
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    out += path;

    return out;
}

QString originOf(const QString& normalisedUrl)
{
    if (normalisedUrl.isEmpty()) return {};
    const QUrl url(normalisedUrl, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty()) return {};
    QString out = url.scheme().toLower() + QStringLiteral("://") + url.host().toLower();
    if (url.port() != -1) out += QLatin1Char(':') + QString::number(url.port());
    return out;
}

FlowProbe classifyLoginFlows(const HttpResponse& res)
{
    FlowProbe probe;

    if (res.transportError) {
        probe.kind = LoginFlowKind::Unreachable;
        probe.detail = res.errorText;
        return probe;
    }
    // Something is listening and it is unwell — a homeserver behind a
    // proxy that is down answers 502, which is "cannot reach it right
    // now", not "this is not a server".
    if (res.status >= 500) {
        probe.kind = LoginFlowKind::Unreachable;
        probe.detail = QStringLiteral("HTTP %1").arg(res.status);
        return probe;
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(res.body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        // The landing-page case: an HTML 404 from nginx. Emphatically not a
        // reason to offer someone a registration form.
        probe.kind = LoginFlowKind::NotAServer;
        probe.detail = (res.status != 0) ? QStringLiteral("HTTP %1").arg(res.status) : QString();
        return probe;
    }

    const QJsonObject obj = doc.object();
    if (!obj.value(QStringLiteral("flows")).isArray()) {
        // Valid JSON, but not a login-flows document: a Matrix error body
        // (M_UNRECOGNIZED), or some unrelated API answering on that path.
        probe.kind = LoginFlowKind::NotAServer;
        probe.detail = obj.value(QStringLiteral("error")).toString();
        if (probe.detail.isEmpty() && res.status != 0)
            probe.detail = QStringLiteral("HTTP %1").arg(res.status);
        return probe;
    }

    bool oidc = false;
    bool password = false;
    for (const auto& value : obj.value(QStringLiteral("flows")).toArray()) {
        const QJsonObject flow = value.toObject();
        const QString type = flow.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("m.login.token")) {
            oidc = true;
            if (probe.identityProvider.isEmpty())
                probe.identityProvider = flow.value(QStringLiteral("identity_provider")).toString();
        } else if (type == QLatin1String("m.login.password")) {
            password = true;
        }
    }

    if (oidc && password) probe.kind = LoginFlowKind::Both;
    else if (oidc) probe.kind = LoginFlowKind::Oidc;
    else if (password) probe.kind = LoginFlowKind::Password;
    else probe.kind = LoginFlowKind::NoSupportedFlow;

    return probe;
}

namespace {

// Does this answer to /_matrix/client/versions look like a homeserver?
// Used only to sanity-check a base_url a well-known file handed us, so it
// is deliberately cheap: right shape, don't care which versions.
bool looksLikeHomeserver(const HttpResponse& res)
{
    if (res.transportError) return false;
    if (res.status != 200) return false;
    const QJsonDocument doc = QJsonDocument::fromJson(res.body);
    return doc.isObject() && doc.object().value(QStringLiteral("versions")).isArray();
}

} // namespace

void ServerDiscovery::resolve(const QString& input, std::function<void(Resolution)> done) const
{
    Resolution base;
    base.requestedUrl = normaliseServerUrl(input);
    base.homeserver = base.requestedUrl;
    base.source = HomeserverSource::UserInput;

    // Nothing to fetch against — bail before we build a nonsense URL.
    if (!base.valid() || !m_fetch) {
        done(base);
        return;
    }

    const QString origin = originOf(base.requestedUrl);
    const QString wellKnownUrl = origin + QLatin1String(kWellKnownPath);

    m_fetch(wellKnownUrl, [this, base, origin, done](const HttpResponse& res) mutable {
        // No well-known file is the overwhelmingly common case (every
        // self-hosted server that never set one up). It is not an error and
        // must not be reported as one: fall through to the typed URL.
        if (res.transportError || res.status != 200) {
            done(base);
            return;
        }

        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(res.body, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            done(base);
            return;
        }

        const QJsonObject obj = doc.object();
        base.identityIssuer = obj.value(QStringLiteral("org.bsfchat.identity"))
                                  .toObject()
                                  .value(QStringLiteral("issuer"))
                                  .toString();

        const QString rawBase = obj.value(QStringLiteral("m.homeserver"))
                                    .toObject()
                                    .value(QStringLiteral("base_url"))
                                    .toString();
        if (rawBase.isEmpty()) {
            done(base);
            return;
        }

        const QString candidate = normaliseServerUrl(rawBase);
        if (candidate.isEmpty()) {
            base.note = QStringLiteral(
                            "%1 advertises a homeserver at \"%2\", which is not a usable address.")
                            .arg(origin, rawBase);
            done(base);
            return;
        }

        // Believe the file only after the homeserver it names answers. A
        // well-known left behind by an old migration would otherwise send
        // every new user to a host that has not existed for months, and
        // they would have no way to tell where the address came from.
        m_fetch(candidate + QLatin1String(kVersionsPath),
                [base, candidate, origin, done](const HttpResponse& check) mutable {
                    if (!looksLikeHomeserver(check)) {
                        base.note = QStringLiteral(
                                        "%1 points at %2, which did not answer — trying %3 itself.")
                                        .arg(origin, candidate, base.requestedUrl);
                        done(base);
                        return;
                    }
                    base.homeserver = candidate;
                    base.source = HomeserverSource::WellKnown;
                    done(base);
                });
    });
}

void ServerDiscovery::resolveAndProbe(const QString& input,
                                      std::function<void(Discovery)> done) const
{
    resolve(input, [this, done](const Resolution& resolution) {
        Discovery out;
        out.resolution = resolution;

        if (!resolution.valid()) {
            out.probe.kind = LoginFlowKind::NotAServer;
            done(out);
            return;
        }
        if (!m_fetch) {
            out.probe.kind = LoginFlowKind::Unreachable;
            done(out);
            return;
        }

        m_fetch(resolution.homeserver + QLatin1String(kLoginPath),
                [out, done](const HttpResponse& res) mutable {
                    out.probe = classifyLoginFlows(res);
                    done(out);
                });
    });
}

} // namespace bsfchat
