#pragma once

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

// Who the identity provider says you are, read out of the id_token it just
// issued.
//
// This exists because of a live-device finding: when the system browser
// already holds a session for the identity provider, the OIDC round trip
// completes without showing anything, and the client signs in as whoever
// that session belongs to. The owner expected a demo account and was
// silently signed in as himself; on a shared phone that is worse than
// confusing. Naming the account is the whole fix, and the id_token is the
// only place the name exists before any homeserver has answered — which
// matters most for the account that belongs to no server at all and so
// never produces a ServerConnection to ask.
//
// Deliberately NOT a second front door into the OIDC flow: no validation
// happens here and nothing here decides anything. IdentityClient has
// already run oidc::checkIdToken (audience + nonce) before this token
// reaches ServerManager; this is a read of a string we have accepted, for
// the purpose of printing it. A garbled token yields empty fields and the
// UI falls back to saying nothing rather than saying something wrong.
namespace bsfchat {

struct IdTokenAccount {
    // `name` from the profile scope — a human's display name. Empty when
    // the provider was asked for openid only, or the account has none.
    QString name;
    // `sub` — the account's stable id at the provider. Never empty for a
    // well-formed token, and the only field we can count on.
    QString subject;

    bool isKnown() const { return !name.isEmpty() || !subject.isEmpty(); }

    // What to put in front of a user: the display name when there is one,
    // else the subject. Callers show the other one underneath.
    QString label() const { return name.isEmpty() ? subject : name; }
};

inline IdTokenAccount idTokenAccount(const QString& idToken)
{
    const auto parts = idToken.split('.');
    if (parts.size() != 3) return {};
    const QByteArray payload = QByteArray::fromBase64(
        parts[1].toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    const QJsonObject claims = QJsonDocument::fromJson(payload).object();
    if (claims.isEmpty()) return {};

    IdTokenAccount out;
    out.name = claims.value(QStringLiteral("name")).toString();
    // The identity service puts the login handle in preferred_username on
    // /userinfo but not in the id_token; try it anyway so a provider that
    // does include it gets used, and fall back to `sub`.
    if (out.name.isEmpty())
        out.name = claims.value(QStringLiteral("preferred_username")).toString();
    out.subject = claims.value(QStringLiteral("sub")).toString();
    return out;
}

} // namespace bsfchat
