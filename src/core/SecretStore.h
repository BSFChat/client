#pragma once
// Secrets that must never be written to the plain settings file.
//
// The one that motivated this is the BSFChat ID refresh token: it can mint a
// sign-in to ANY chat server the account can reach (the identity provider's
// refresh grant takes a `resource`), so a copy of it is a copy of the user.
// Long-lived bearer tokens read off disk by malware are the classic way chat
// accounts get stolen, so it lives in the operating system's credential
// store, scoped to this device:
//
//   macOS / iOS   Keychain (not iCloud-synchronised)
//   Windows       Credential Manager
//   Linux         Secret Service (GNOME Keyring, KWallet) via libsecret
//   Android       Android Keystore-wrapped storage
//
// via QtKeychain. When no secure store is available — a build without
// BSFCHAT_ENABLE_KEYCHAIN, or a Linux session with no Secret Service running —
// writes FAIL rather than falling back to a file: the caller keeps the secret
// in memory only, so the user stays signed in until the app quits and signs
// in again next launch. Inconvenient, deliberately: that is the trade the
// user asked for over "convenient and stealable".
//
// The interface is synchronous because every caller is on the GUI thread at
// a point where it needs the answer before it can continue (startup restore,
// persisting a rotated token BEFORE using what it bought). The keychain
// backend runs a local event loop around QtKeychain's job; reads and writes
// are small and local.

#include <QList>
#include <QString>

#include <optional>

class SecretStore {
public:
    virtual ~SecretStore() = default;

    // False when nothing was stored (no secure store, or it refused).
    virtual bool write(const QString& key, const QString& value) = 0;
    // nullopt when absent OR when the store could not be read.
    virtual std::optional<QString> read(const QString& key) = 0;
    // True when the key is gone afterwards (including "was never there").
    virtual bool remove(const QString& key) = 0;
    // Whether write() can succeed at all on this system.
    virtual bool available() const = 0;

    // Fire-and-forget variants, for callers that must not spin a local event
    // loop: Settings persists credentials from inside arbitrary signal
    // handlers, and a nested loop there would re-enter them. Jobs still run
    // one at a time in submission order, so a later read or remove sees the
    // write. A failure is logged and otherwise lost: the caller already holds
    // the value in memory. Default: the synchronous call.
    virtual void writeLater(const QString& key, const QString& value) { write(key, value); }
    virtual void removeLater(const QString& key) { remove(key); }

    // The process-wide store: the OS keychain, namespaced by the active
    // profile's application name so `--profile alice` and the default
    // profile never read each other's secrets.
    static SecretStore& instance();
    // Tests swap in a MemorySecretStore. Pass nullptr to restore the default.
    static void setInstanceForTesting(SecretStore* store);
};

// In-memory store: what tests use, and nothing else. Survives nothing.
class MemorySecretStore : public SecretStore {
public:
    bool write(const QString& key, const QString& value) override;
    std::optional<QString> read(const QString& key) override;
    bool remove(const QString& key) override;
    bool available() const override { return m_available; }
    // Simulate a system with no secure store.
    void setAvailable(bool available) { m_available = available; }
    int size() const;

private:
    bool m_available = true;
    struct Entry { QString key; QString value; };
    std::optional<qsizetype> indexOf(const QString& key) const;
    QList<Entry> m_entries;
};
