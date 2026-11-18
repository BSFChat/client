#pragma once
// The in-flight half of a mobile sign-in, in a form that survives the process
// (audit M2-5).
//
// On Android the authorization page is shown by another app. If the OS kills
// BSFChat while that page is up — routine on low-memory phones, and what
// "Don't keep activities" does every time — the redirect cold-starts a new
// process that has never heard of the attempt. The PKCE verifier, state and
// nonce were memory-only, so the code could not be redeemed and the sign-in
// vanished without a word.
//
// IdentityClient now writes this record to SecretStore (Android Keystore-
// backed) when it shows the page, and a callback with no live attempt is
// checked against it. The record is single-use and short-lived: it is the
// PKCE verifier, so it is removed the moment it is used or abandoned, and
// one older than kPendingOidcLifetimeMs is refused even if still present.
//
// Pure (no store, no clock of its own) so tests drive it directly.

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QtGlobal>

#include <optional>

namespace oidc {

// SecretStore key. One slot: IdentityClient shows one page at a time.
inline constexpr const char* kPendingOidcKey = "oidc-pending-request";
// Ten minutes: twice the attempt's own five-minute timeout, so a user who
// finished in the browser just before the process died is still let in, and
// short enough that a stale verifier does not linger in the keystore.
inline constexpr qint64 kPendingOidcLifetimeMs = 10 * 60 * 1000;

struct PendingOidcRequest {
    QString providerUrl;
    QString codeVerifier;
    QString state;
    QString nonce;
    QString resource;   // empty for the account-level sign-in
    qint64 createdAtMs = 0;

    QString toJson() const
    {
        QJsonObject o;
        o.insert(QStringLiteral("v"), 1);
        o.insert(QStringLiteral("provider"), providerUrl);
        o.insert(QStringLiteral("verifier"), codeVerifier);
        o.insert(QStringLiteral("state"), state);
        o.insert(QStringLiteral("nonce"), nonce);
        o.insert(QStringLiteral("resource"), resource);
        // As a string: JSON doubles are exact to 2^53, but there is no reason
        // to rely on that for a timestamp.
        o.insert(QStringLiteral("created"), QString::number(createdAtMs));
        return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
    }

    static std::optional<PendingOidcRequest> fromJson(const QString& json)
    {
        const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
        if (!doc.isObject()) return std::nullopt;
        const QJsonObject o = doc.object();
        if (o.value(QStringLiteral("v")).toInt() != 1) return std::nullopt;
        PendingOidcRequest r;
        r.providerUrl = o.value(QStringLiteral("provider")).toString();
        r.codeVerifier = o.value(QStringLiteral("verifier")).toString();
        r.state = o.value(QStringLiteral("state")).toString();
        r.nonce = o.value(QStringLiteral("nonce")).toString();
        r.resource = o.value(QStringLiteral("resource")).toString();
        bool ok = false;
        r.createdAtMs = o.value(QStringLiteral("created")).toString().toLongLong(&ok);
        if (!ok || r.providerUrl.isEmpty() || r.codeVerifier.isEmpty() || r.state.isEmpty()
            || r.nonce.isEmpty()) {
            return std::nullopt;
        }
        return r;
    }

    // Usable at `nowMs`? A record from the future (clock moved back) is
    // treated as expired rather than trusted for longer.
    bool isFresh(qint64 nowMs) const
    {
        const qint64 age = nowMs - createdAtMs;
        return age >= 0 && age <= kPendingOidcLifetimeMs;
    }
};

// What the user is told when a callback arrives for an attempt that cannot be
// finished: no record, an expired one, or one for a different attempt.
inline QString interruptedSignInMessage()
{
    return QStringLiteral("Sign-in was interrupted — please try again.");
}

} // namespace oidc
