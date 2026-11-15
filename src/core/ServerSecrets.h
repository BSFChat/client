#pragma once
// Chat-server access tokens, kept out of the plain settings file (audit C-5).
//
// Settings' "servers" array used to hold every server's accessToken in
// QSettings: the Windows registry, ~/.config, Android shared prefs, and an
// iOS plist that goes into iCloud backups. Each token is a bearer credential
// for that account on that server, so it now lives in SecretStore (the OS
// keychain), one entry per (server URL, user id), and the settings file keeps
// only what is not secret.
//
// No secure store (a Linux session without a Secret Service, or a build
// without QtKeychain): the token is held in memory for this run and never
// written anywhere, so the user signs in again next launch. Android (Keystore)
// and iOS (Keychain) always have one, so mobile sign-in stays persistent.
//
// Pure of Settings on purpose, so the migration can be tested against a
// scratch QSettings and a MemorySecretStore.

#include <QHash>
#include <QString>

class QSettings;
class SecretStore;

class ServerSecrets {
public:
    // nullptr = SecretStore::instance(), resolved at each use so a test's
    // setInstanceForTesting() is honoured.
    explicit ServerSecrets(SecretStore* store = nullptr) : m_store(store) {}

    static QString accessTokenKey(const QString& serverUrl, const QString& userId);

    struct Migration {
        int moved = 0;          // plaintext tokens now in the secure store
        int memoryOnly = 0;     // plaintext tokens kept for this run only
        bool rewrote = false;   // the "servers" array was rewritten
    };
    // Move any plaintext secret out of `settings`' "servers" array — the
    // accessToken, and the never-used legacy identityRefreshToken, which is
    // simply dropped — and rewrite the array without them and without the
    // stale rows above `size` that beginWriteArray leaves behind (C-4).
    // Idempotent; a settings file with nothing to clean is left untouched.
    // Call once, before anything reads tokens.
    Migration migrate(QSettings& settings);

    // The token for this account, or empty. Memory first; the store is read
    // once per key per run (synchronously — startup only, in practice).
    QString accessToken(const QString& serverUrl, const QString& userId);
    // Record a token; empty forgets it. Never blocks: the store is written
    // asynchronously, and the value is served from memory meanwhile.
    void setAccessToken(const QString& serverUrl, const QString& userId, const QString& token);
    void forget(const QString& serverUrl, const QString& userId)
    {
        setAccessToken(serverUrl, userId, QString());
    }

    // Whether tokens survive a restart on this system.
    bool persistent() const;

private:
    SecretStore& store() const;
    SecretStore* m_store = nullptr;
    // key -> token; an empty value means "known absent".
    QHash<QString, QString> m_cache;
};
