#include "core/ServerSecrets.h"

#include "core/SecretStore.h"

#include <QDebug>
#include <QSet>
#include <QSettings>
#include <QVariantMap>

namespace {
constexpr auto kServersArray = "servers";
constexpr auto kAccessToken = "accessToken";
// Written by every build before this one and read by none: a per-server
// refresh-token slot that was never filled in by the client. Dropped, not
// migrated — nothing could use it, and it must not stay in plaintext.
constexpr auto kLegacyRefreshToken = "identityRefreshToken";
} // namespace

SecretStore& ServerSecrets::store() const
{
    return m_store ? *m_store : SecretStore::instance();
}

QString ServerSecrets::accessTokenKey(const QString& serverUrl, const QString& userId)
{
    // A space cannot occur in either half (URLs percent-encode it, Matrix
    // user ids forbid it), so two different pairs can never share a key.
    return QStringLiteral("access-token:") + serverUrl + QLatin1Char(' ') + userId;
}

bool ServerSecrets::persistent() const
{
    return store().available();
}

ServerSecrets::Migration ServerSecrets::migrate(QSettings& settings)
{
    Migration result;
    bool dirty = false;
    QList<QVariantMap> rows;
    QSet<QString> seen;

    const int size = settings.beginReadArray(QLatin1String(kServersArray));
    for (int i = 0; i < size; ++i) {
        settings.setArrayIndex(i);
        QVariantMap row;
        const QStringList keys = settings.childKeys();
        for (const QString& k : keys) {
            if (k == QLatin1String(kAccessToken) || k == QLatin1String(kLegacyRefreshToken)) {
                dirty = true;
                continue;
            }
            row.insert(k, settings.value(k));
        }
        const QString token = settings.value(QLatin1String(kAccessToken)).toString();
        rows.append(row);
        if (token.isEmpty()) continue;

        const QString key = accessTokenKey(row.value(QStringLiteral("url")).toString(),
                                           row.value(QStringLiteral("userId")).toString());
        // Duplicate rows for one account (an old bug; ServerManager drops
        // them right after this): the FIRST row is the one it keeps, so its
        // token is the one that must survive.
        if (seen.contains(key)) continue;
        seen.insert(key);
        if (store().write(key, token)) {
            ++result.moved;
        } else {
            // No secure store. Still removed from the file below: the user
            // asked for "never in plaintext" over "never signed out", so this
            // run keeps it in memory and the next launch asks for a sign-in.
            ++result.memoryOnly;
        }
        m_cache.insert(key, token);
    }
    settings.endArray();

    // Rows above `size`: beginWriteArray(n) never deleted them, so a server
    // removed before this build could still have its token on disk (C-4).
    settings.beginGroup(QLatin1String(kServersArray));
    const QStringList groups = settings.childGroups();
    settings.endGroup();
    for (const QString& g : groups) {
        bool ok = false;
        const int n = g.toInt(&ok); // QSettings arrays are 1-based on disk
        if (!ok || n < 1 || n > size) {
            dirty = true;
            break;
        }
    }

    if (!dirty) return result;

    settings.remove(QLatin1String(kServersArray));
    settings.beginWriteArray(QLatin1String(kServersArray), int(rows.size()));
    for (int i = 0; i < rows.size(); ++i) {
        settings.setArrayIndex(i);
        for (auto it = rows[i].constBegin(); it != rows[i].constEnd(); ++it)
            settings.setValue(it.key(), it.value());
    }
    settings.endArray();
    settings.sync();
    result.rewrote = true;

    if (result.moved || result.memoryOnly) {
        qInfo().nospace() << "[ServerSecrets] moved " << result.moved
                          << " access token(s) out of the settings file into the OS keychain"
                          << (result.memoryOnly
                                  ? QStringLiteral("; %1 kept for this run only (no secure store)")
                                        .arg(result.memoryOnly)
                                  : QString());
    }
    return result;
}

QString ServerSecrets::accessToken(const QString& serverUrl, const QString& userId)
{
    const QString key = accessTokenKey(serverUrl, userId);
    auto it = m_cache.constFind(key);
    if (it != m_cache.constEnd()) return it.value();
    const QString value = store().read(key).value_or(QString());
    m_cache.insert(key, value);
    return value;
}

void ServerSecrets::setAccessToken(const QString& serverUrl, const QString& userId,
                                   const QString& token)
{
    const QString key = accessTokenKey(serverUrl, userId);
    auto it = m_cache.constFind(key);
    if (it != m_cache.constEnd() && it.value() == token) return;
    m_cache.insert(key, token);
    if (token.isEmpty()) store().removeLater(key);
    else store().writeLater(key, token);
}
