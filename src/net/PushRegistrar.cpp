#include "net/PushRegistrar.h"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonDocument>
#include <QTimer>
#include <QUrl>

#include <algorithm>

namespace bsfchat::client::push {

QString normaliseGatewayUrl(const QString& input)
{
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty()) return {};
    QUrl url(trimmed, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty()) return {};
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("https") && scheme != QLatin1String("http")) return {};
    if (!url.userInfo().isEmpty() || url.hasFragment()) return {};
    // A bare origin means "the Matrix push gateway API on that host".
    if (url.path().isEmpty() || url.path() == QLatin1String("/"))
        url.setPath(QString::fromLatin1(kNotifyPath));
    return url.toString(QUrl::FullyEncoded);
}

QJsonObject pusherBody(const PusherIdentity& id, const QString& pushkey,
                       const QString& gatewayUrl, PayloadTier tier, bool append)
{
    QJsonObject data{
        {QStringLiteral("url"), gatewayUrl},
        {QStringLiteral("format"),
         QString::fromLatin1(tier == PayloadTier::Names ? kFormatNames : kFormatEventIdOnly)},
    };
    if (id.platform == Platform::Ios) {
        // Sygnal's APNs pushkin, for a notification without a `type` (which is
        // what event_id_only is), sends this object as the APNs payload and
        // adds room_id / event_id / unread_count at the top level. Without it
        // an event_id_only push would be a silent one, and a silent push is
        // not a notification. Fixed text, so it says nothing to Apple; the
        // names tier (which carries `type`) takes Sygnal's other path and its
        // loc-key alerts instead (ios/en.lproj/Localizable.strings).
        //
        // mutable-content lets a future Notification Service Extension fetch
        // the event and rewrite the text on the device. Harmless without one.
        const QJsonObject alert{
            {QStringLiteral("title"), QStringLiteral("BSFChat")},
            {QStringLiteral("body"), QStringLiteral("New message")},
        };
        const QJsonObject aps{
            {QStringLiteral("alert"), alert},
            {QStringLiteral("sound"), QStringLiteral("default")},
            {QStringLiteral("mutable-content"), 1},
        };
        data.insert(QStringLiteral("default_payload"),
                    QJsonObject{{QStringLiteral("aps"), aps}});
    }
    QJsonObject body{
        {QStringLiteral("kind"), QStringLiteral("http")},
        {QStringLiteral("app_id"), id.appId},
        {QStringLiteral("pushkey"), pushkey},
        {QStringLiteral("app_display_name"), id.appDisplayName},
        {QStringLiteral("device_display_name"), id.deviceDisplayName},
        {QStringLiteral("data"), data},
    };
    if (!id.lang.isEmpty()) body.insert(QStringLiteral("lang"), id.lang);
    // `append` normally stays at its default, false: this app_id + pushkey
    // then belongs to one account alone on that homeserver, so a sign-in
    // whose sign-out could not delete its pusher stops receiving through
    // this device. With two accounts of THIS device on one homeserver the
    // same default makes them evict each other (audit 3), so the registrar
    // asks for append=true exactly then.
    if (append) body.insert(QStringLiteral("append"), true);
    return body;
}

QJsonObject deletionBody(const QString& appId, const QString& pushkey)
{
    return QJsonObject{
        {QStringLiteral("kind"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("app_id"), appId},
        {QStringLiteral("pushkey"), pushkey},
    };
}

ReplyKind classifyReply(int status, const QByteArray& body, bool transportError)
{
    if (status >= 200 && status < 300) return ReplyKind::Ok;
    if (status == 0 || (transportError && status == 0)) return ReplyKind::Retryable;
    if (status == 401 || status == 403) return ReplyKind::AuthGone;
    if (status == 429 || status >= 500) return ReplyKind::Retryable;
    if (status == 400) {
        const QString error = QJsonDocument::fromJson(body).object()
                                  .value(QStringLiteral("error")).toString();
        // Server api/PushHandler.cpp: every format refusal names data.format,
        // every gateway refusal names data.url or the allowlist.
        if (error.contains(QLatin1String("data.format"))) return ReplyKind::FormatRefused;
        if (error.contains(QLatin1String("data.url"))
            || error.contains(QLatin1String("gateway"), Qt::CaseInsensitive))
            return ReplyKind::GatewayRefused;
        return ReplyKind::Refused;
    }
    return ReplyKind::Refused;
}

} // namespace bsfchat::client::push

namespace push = bsfchat::client::push;

PushRegistrar::PushRegistrar(PushTransport* transport, PusherIdentity identity,
                             QObject* parent)
    : QObject(parent)
    , m_transport(transport)
    , m_identity(std::move(identity))
    , m_gatewayUrl(QString::fromLatin1(push::kDefaultGatewayUrl))
{
}

void PushRegistrar::setToken(const QString& pushkey)
{
    if (m_token == pushkey) return;
    m_token = pushkey;
    pumpAll();
}

void PushRegistrar::setWanted(bool wanted)
{
    if (m_wanted == wanted) return;
    m_wanted = wanted;
    pumpAll();
}

void PushRegistrar::setTier(PayloadTier tier)
{
    if (m_tier == tier) return;
    m_tier = tier;
    // A fresh choice of names is a fresh question to every server, including
    // one that refused it before (its operator may have raised the ceiling).
    for (auto& a : m_accounts) a.namesRefused = false;
    pumpAll();
}

void PushRegistrar::setGatewayUrl(const QString& url)
{
    const QString normalised = push::normaliseGatewayUrl(url);
    if (m_gatewayUrl == normalised) return;
    if (normalised.isEmpty() && !url.trimmed().isEmpty())
        qWarning().noquote() << "[push] ignoring invalid gateway URL" << url;
    m_gatewayUrl = normalised;
    pumpAll();
}

void PushRegistrar::addAccount(const QString& account, const QString& registeredPushkey)
{
    if (account.isEmpty()) return;
    if (!m_accounts.contains(account)) {
        Account a;
        a.registeredKey = registeredPushkey;
        a.generation = m_nextGeneration++;
        m_accounts.insert(account, a);
    }
    pump(account);
}

QList<QJsonObject> PushRegistrar::takeAccount(const QString& account)
{
    QList<QJsonObject> out;
    auto it = m_accounts.find(account);
    if (it == m_accounts.end()) return out;
    QStringList keys = it->stale;
    if (!it->registeredKey.isEmpty() && !keys.contains(it->registeredKey))
        keys.append(it->registeredKey);
    // A registration in flight may land after this; its pushkey is the
    // current token, so owe a deletion for that too.
    if (it->busy && !m_token.isEmpty() && !keys.contains(m_token)) keys.append(m_token);
    for (const QString& k : keys) out.append(push::deletionBody(m_identity.appId, k));
    const bool hadRegistration = !it->registeredKey.isEmpty();
    m_accounts.erase(it);
    if (hadRegistration) emit registeredPushkeyChanged(account, QString());
    emit stateChanged(account);
    return out;
}

void PushRegistrar::dropAccount(const QString& account)
{
    if (m_accounts.remove(account) > 0) emit stateChanged(account);
}

PushRegistrar::State PushRegistrar::state(const QString& account) const
{
    auto it = m_accounts.constFind(account);
    return it == m_accounts.constEnd() ? State::Idle : it->state;
}

bool PushRegistrar::namesRefused(const QString& account) const
{
    auto it = m_accounts.constFind(account);
    return it != m_accounts.constEnd() && it->namesRefused;
}

QString PushRegistrar::lastError(const QString& account) const
{
    auto it = m_accounts.constFind(account);
    return it == m_accounts.constEnd() ? QString() : it->lastError;
}

void PushRegistrar::retryFailed()
{
    for (const QString& account : m_accounts.keys()) {
        auto it = m_accounts.find(account);
        if (it != m_accounts.end() && it->state == State::Failed && !it->busy) pump(account);
    }
}

int PushRegistrar::backoffMs(int failures)
{
    // 5 s, 10 s, 20 s … capped at 30 min.
    qint64 ms = 5000;
    for (int i = 1; i < failures && ms < 30LL * 60 * 1000; ++i) ms *= 2;
    return int(std::min<qint64>(ms, 30LL * 60 * 1000));
}

namespace {
QString homeserverOf(const QString& account)
{
    // Keys without a server part (tests) are their own homeserver.
    const int bar = account.indexOf(QLatin1Char('|'));
    return bar < 0 ? account : account.mid(bar + 1);
}
// `append` is a request flag, not part of the pusher the server keeps, so
// a change in it alone is no reason to re-register.
QJsonObject withoutAppend(QJsonObject body)
{
    body.remove(QStringLiteral("append"));
    return body;
}
} // namespace

bool PushRegistrar::sharesHomeserver(const QString& account) const
{
    const QString hs = homeserverOf(account);
    for (auto it = m_accounts.constBegin(); it != m_accounts.constEnd(); ++it)
        if (it.key() != account && homeserverOf(it.key()) == hs) return true;
    return false;
}

void PushRegistrar::reregisterSiblings(const QString& account)
{
    // `account` just registered with append=false, which removes every
    // other user's pusher for this pushkey on its homeserver. Any sibling
    // there (it appeared while that request was on the wire) has to
    // register again — with append=true now, since the two share a server.
    const QString hs = homeserverOf(account);
    QStringList again;
    for (auto it = m_accounts.begin(); it != m_accounts.end(); ++it) {
        if (it.key() == account || homeserverOf(it.key()) != hs) continue;
        if (it->busy) {
            it->reregister = true;
        } else if (!it->registeredBody.isEmpty()) {
            it->registeredBody = {};
            again << it.key();
        }
    }
    for (const QString& k : again) pump(k);
}

QJsonObject PushRegistrar::desiredBody(const QString& account, const Account& a) const
{
    if (!m_wanted || m_token.isEmpty() || m_gatewayUrl.isEmpty()) return {};
    const PayloadTier tier =
        (m_tier == PayloadTier::Names && !a.namesRefused) ? PayloadTier::Names
                                                          : PayloadTier::EventIdOnly;
    return push::pusherBody(m_identity, m_token, m_gatewayUrl, tier,
                            sharesHomeserver(account));
}

void PushRegistrar::pumpAll()
{
    for (const QString& account : m_accounts.keys()) pump(account);
}

void PushRegistrar::setState(const QString& account, Account& a, State s)
{
    if (a.state == s) return;
    a.state = s;
    emit stateChanged(account);
}

void PushRegistrar::setRegistered(const QString& account, Account& a, const QString& key,
                                  const QJsonObject& body)
{
    const bool keyChanged = a.registeredKey != key;
    a.registeredKey = key;
    a.registeredBody = body;
    if (keyChanged) emit registeredPushkeyChanged(account, key);
}

void PushRegistrar::scheduleRetry(const QString& account)
{
    auto it = m_accounts.find(account);
    if (it == m_accounts.end() || !m_autoRetry) return;
    const quint64 gen = it->generation;
    QTimer::singleShot(backoffMs(it->failures), this, [this, account, gen]() {
        auto found = m_accounts.find(account);
        if (found == m_accounts.end() || found->generation != gen) return;
        if (found->state == State::Failed && !found->busy) pump(account);
    });
}

void PushRegistrar::pump(const QString& account)
{
    auto it = m_accounts.find(account);
    if (it == m_accounts.end() || !m_transport) return;
    Account& a = *it;
    if (a.busy) return;

    const QJsonObject want = desiredBody(account, a);
    const QString wantKey = want.isEmpty() ? QString() : m_token;
    const bool inPlace = !want.isEmpty() && a.registeredKey == wantKey
                         && withoutAppend(a.registeredBody) == withoutAppend(want);
    const bool gatewayRefused = !want.isEmpty() && !a.refusedGateway.isEmpty()
                                && a.refusedGateway == m_gatewayUrl;
    const bool bodyRefused = !want.isEmpty()
                             && withoutAppend(a.refusedBody) == withoutAppend(want);
    const quint64 gen = a.generation;

    // 1. Register what is wanted, unless it is already in place or this
    //    server has said no to exactly this.
    if (!want.isEmpty() && !inPlace && !gatewayRefused && !bodyRefused) {
        a.busy = true;
        setState(account, a, State::Busy);
        m_transport->setPusher(account, want,
            [this, account, gen, want, wantKey](const PushTransport::Reply& r) {
            auto found = m_accounts.find(account);
            if (found == m_accounts.end() || found->generation != gen) return;
            Account& acc = *found;
            acc.busy = false;
            const auto kind = push::classifyReply(r.status, r.body, r.transportError);
            const QString why = QJsonDocument::fromJson(r.body).object()
                                    .value(QStringLiteral("error")).toString();
            switch (kind) {
            case push::ReplyKind::Ok: {
                const QString previous = acc.registeredKey;
                if (!previous.isEmpty() && previous != wantKey && !acc.stale.contains(previous))
                    acc.stale.append(previous);
                acc.stale.removeAll(wantKey);
                setRegistered(account, acc, wantKey, want);
                acc.failures = 0;
                acc.lastError.clear();
                acc.refusedGateway.clear();
                acc.refusedBody = {};
                if (acc.reregister) {
                    // Another account's append=false registration may have
                    // landed after this one and evicted it.
                    acc.reregister = false;
                    acc.registeredBody = {};
                }
                qInfo().noquote() << "[push] pusher registered on" << account;
                if (!want.value(QStringLiteral("append")).toBool())
                    reregisterSiblings(account);
                break;
            }
            case push::ReplyKind::FormatRefused:
                if (!acc.namesRefused && m_tier == PayloadTier::Names) {
                    // This server's push.max_payload is below "names". Fall back
                    // to the private tier rather than to no push at all.
                    acc.namesRefused = true;
                    qInfo().noquote() << "[push]" << account
                                      << "does not allow the names tier; using event_id_only";
                    break;
                }
                acc.refusedBody = want;
                acc.lastError = why;
                qWarning().noquote() << "[push]" << account << "refused the pusher:" << why;
                break;
            case push::ReplyKind::GatewayRefused:
                // A self-hosted server that has not listed our gateway in
                // push.allowed_gateway_prefixes. Normal; not an error to shout
                // about, and pointless to retry until the URL changes.
                acc.refusedGateway = want.value(QStringLiteral("data")).toObject()
                                       .value(QStringLiteral("url")).toString();
                acc.lastError = why;
                qInfo().noquote() << "[push]" << account
                                  << "does not allow this push gateway:" << why;
                break;
            case push::ReplyKind::Refused:
            case push::ReplyKind::AuthGone:
                acc.refusedBody = want;
                acc.lastError = why.isEmpty() ? QStringLiteral("HTTP %1").arg(r.status) : why;
                qWarning().noquote() << "[push]" << account << "refused the pusher (HTTP"
                                     << r.status << "):" << acc.lastError;
                break;
            case push::ReplyKind::Retryable:
                ++acc.failures;
                acc.lastError = why.isEmpty() ? QStringLiteral("network error") : why;
                setState(account, acc, State::Failed);
                scheduleRetry(account);
                return;
            }
            pump(account);
        });
        return;
    }

    // 2. Delete what is no longer wanted: replaced tokens first, then the
    //    registered pusher itself if push is off for this account.
    QString del;
    if (!a.stale.isEmpty()) del = a.stale.first();
    else if (want.isEmpty() && !a.registeredKey.isEmpty()) del = a.registeredKey;
    if (!del.isEmpty()) {
        a.busy = true;
        setState(account, a, State::Busy);
        m_transport->setPusher(account, push::deletionBody(m_identity.appId, del),
            [this, account, gen, del](const PushTransport::Reply& r) {
            auto found = m_accounts.find(account);
            if (found == m_accounts.end() || found->generation != gen) return;
            Account& acc = *found;
            acc.busy = false;
            const auto kind = push::classifyReply(r.status, r.body, r.transportError);
            if (kind == push::ReplyKind::Retryable) {
                ++acc.failures;
                setState(account, acc, State::Failed);
                scheduleRetry(account);
                return;
            }
            // Anything else is final: deleted, or the server will never
            // accept the deletion (and the gateway's rejection of a dead
            // token makes the server drop found in time anyway).
            acc.failures = 0;
            acc.stale.removeAll(del);
            if (acc.registeredKey == del) setRegistered(account, acc, QString(), {});
            pump(account);
        });
        return;
    }

    // 3. Settled.
    if (inPlace) setState(account, a, State::Registered);
    else if (gatewayRefused) setState(account, a, State::GatewayRefused);
    else if (bodyRefused) setState(account, a, State::Refused);
    else setState(account, a, State::Idle);
}

namespace bsfchat::client::push {

namespace {
QString trp(const char* text, int n = -1)
{
    // Same context as before this moved out of PushManager, so existing
    // translations still apply.
    return QCoreApplication::translate("PushManager", text, nullptr, n);
}
QString hostWithReason(const AccountStatus& a)
{
    return a.lastError.isEmpty() ? a.host
                                 : QStringLiteral("%1 (%2)").arg(a.host, a.lastError);
}
} // namespace

StatusSummary summariseStatus(const StatusInputs& in)
{
    using S = PushRegistrar::State;
    StatusSummary out;
    const bool pushWanted = in.compiledIn && in.notificationsOn
                            && (in.ios || !in.backgroundChosen);

    // Per-account tally. An account counts as "on the background
    // connection" when the server will not (or cannot yet) deliver to us
    // through the gateway: refused, failing, or nothing registrable at all
    // (we have a token but no valid gateway).
    QStringList gatewayRefused, refused, failed, namesOff;
    int busy = 0;
    for (const AccountStatus& a : in.accounts) {
        bool lacksPusher = false;
        switch (a.state) {
        case S::Registered: ++out.registered; break;
        case S::Busy: ++busy; break;
        case S::GatewayRefused:
            ++out.refused; gatewayRefused << a.host; lacksPusher = true; break;
        case S::Refused:
            ++out.refused; refused << hostWithReason(a); lacksPusher = true; break;
        case S::Failed:
            ++out.failed; failed << hostWithReason(a); lacksPusher = true; break;
        case S::Idle:
            lacksPusher = in.hasToken;
            break;
        }
        if (lacksPusher) out.onBackground << a.host;
        if (a.namesRefused) namesOff << a.host;
    }
    gatewayRefused.removeDuplicates();
    namesOff.removeDuplicates();
    out.onBackground.removeDuplicates();

    // ---- Android background connection ----
    if (!in.ios) {
        if (in.backgroundChosen || !in.compiledIn || in.bridgeUnavailable)
            out.backgroundSyncWanted = true;
        else if (in.notificationsOn
                 && (in.tokenRetrying || !in.gatewayValid || !out.onBackground.isEmpty()))
            out.backgroundSyncWanted = true;
    }
    if (in.ios || !pushWanted) out.onBackground.clear();

    // ---- Status line ----
    if (!in.compiledIn || !in.notificationsOn) return out;
    if (!pushWanted) {
        out.text = trp("Using a background connection instead of push.");
        return out;
    }
    if (in.permissionBlocked) {
        out.text = trp("Notifications are blocked in system settings.");
        return out;
    }
    if (in.bridgeUnavailable) {
        out.text = in.ios
            ? trp("Push notifications are not available: allow notifications for BSFChat "
                  "in iOS Settings.")
            : trp("Push is not available on this device (no Google Play services?). "
                  "Using a background connection instead.");
        return out;
    }
    if (in.tokenRetrying) {
        out.text = in.ios
            ? trp("Could not reach the push service yet; retrying.")
            : trp("Could not reach the push service yet; retrying. A background "
                  "connection keeps notifications coming meanwhile.");
        return out;
    }
    if (!in.hasToken) {
        out.text = trp("Waiting for the push service…");
        return out;
    }
    if (!in.gatewayValid) {
        out.text = in.ios
            ? trp("The push gateway URL in Advanced settings is not valid, so push is off.")
            : trp("The push gateway URL in Advanced settings is not valid, so BSFChat "
                  "uses a background connection instead.");
        return out;
    }

    const QString sep = QStringLiteral(", ");
    QStringList parts;
    if (out.registered > 0 || (out.refused == 0 && out.failed == 0))
        parts << trp("Push is on for %n server(s).", out.registered);
    if (busy > 0) parts << trp("Registering with %n server(s)…", busy);
    if (!gatewayRefused.isEmpty()) {
        parts << (in.ios
            ? trp("%1 does not allow this push gateway, so it can only notify you "
                  "while BSFChat is open.")
            : trp("%1 does not allow this push gateway."))
                 .arg(gatewayRefused.join(sep));
    }
    if (!refused.isEmpty())
        parts << trp("%n server(s) refused push: %1.", refused.size()).arg(refused.join(sep));
    if (!failed.isEmpty())
        parts << trp("Could not register push with %n server(s), retrying: %1.", failed.size())
                     .arg(failed.join(sep));
    if (!out.onBackground.isEmpty())
        parts << trp("BSFChat keeps a background connection for %1.")
                     .arg(out.onBackground.join(sep));
    if (!namesOff.isEmpty() && in.showNames)
        parts << trp("%1 only sends \"New message\".").arg(namesOff.join(sep));
    out.text = parts.join(QLatin1Char(' '));
    return out;
}

} // namespace bsfchat::client::push
