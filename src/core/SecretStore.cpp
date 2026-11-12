#include "core/SecretStore.h"

#include "core/AppProfile.h"

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>

#ifdef BSFCHAT_HAVE_KEYCHAIN
#  include <qt6keychain/keychain.h>
#endif

namespace {

#ifdef BSFCHAT_HAVE_KEYCHAIN
class KeychainSecretStore : public SecretStore {
public:
    bool write(const QString& key, const QString& value) override
    {
        QKeychain::WritePasswordJob job(service());
        job.setAutoDelete(false);
        job.setKey(key);
        job.setTextData(value);
        if (!run(job)) {
            qWarning().noquote() << "[SecretStore] could not store a secret in the OS keychain:"
                                 << job.errorString();
            return false;
        }
        return true;
    }

    std::optional<QString> read(const QString& key) override
    {
        QKeychain::ReadPasswordJob job(service());
        job.setAutoDelete(false);
        job.setKey(key);
        if (!run(job)) {
            if (job.error() != QKeychain::EntryNotFound) {
                qWarning().noquote() << "[SecretStore] could not read the OS keychain:"
                                     << job.errorString();
            }
            return std::nullopt;
        }
        return job.textData();
    }

    bool remove(const QString& key) override
    {
        QKeychain::DeletePasswordJob job(service());
        job.setAutoDelete(false);
        job.setKey(key);
        return run(job) || job.error() == QKeychain::EntryNotFound;
    }

    bool available() const override
    {
        // QtKeychain answers this per platform (on Linux: whether a Secret
        // Service or KWallet is reachable on the session bus).
        return QKeychain::isAvailable();
    }

private:
    static QString service()
    {
        // Profile-aware, like every other piece of persisted state
        // (see core/AppProfile.h): two profiles must never share a login.
        return QStringLiteral("com.bsfchat.%1").arg(bsfchat::applicationName());
    }

    static bool run(QKeychain::Job& job)
    {
        QEventLoop loop;
        QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
        job.start();
        loop.exec(QEventLoop::ExcludeUserInputEvents);
        return job.error() == QKeychain::NoError;
    }
};
#else
// No secure store compiled in: every write fails, so nothing is ever
// persisted and callers fall back to memory-only.
class KeychainSecretStore : public SecretStore {
public:
    bool write(const QString&, const QString&) override { return false; }
    std::optional<QString> read(const QString&) override { return std::nullopt; }
    bool remove(const QString&) override { return true; }
    bool available() const override { return false; }
};
#endif

SecretStore* g_override = nullptr;

} // namespace

SecretStore& SecretStore::instance()
{
    if (g_override) return *g_override;
    static KeychainSecretStore keychain;
    return keychain;
}

void SecretStore::setInstanceForTesting(SecretStore* store)
{
    g_override = store;
}

std::optional<qsizetype> MemorySecretStore::indexOf(const QString& key) const
{
    for (qsizetype i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).key == key) return i;
    }
    return std::nullopt;
}

bool MemorySecretStore::write(const QString& key, const QString& value)
{
    if (!m_available) return false;
    if (auto i = indexOf(key)) {
        m_entries[*i].value = value;
    } else {
        m_entries.append({key, value});
    }
    return true;
}

std::optional<QString> MemorySecretStore::read(const QString& key)
{
    if (auto i = indexOf(key)) return m_entries.at(*i).value;
    return std::nullopt;
}

bool MemorySecretStore::remove(const QString& key)
{
    if (auto i = indexOf(key)) m_entries.removeAt(*i);
    return true;
}

int MemorySecretStore::size() const
{
    return static_cast<int>(m_entries.size());
}
