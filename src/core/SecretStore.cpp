#include "core/SecretStore.h"

#include "core/AppProfile.h"

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>

#ifdef BSFCHAT_HAVE_KEYCHAIN
#  include <qtkeychain/keychain.h> // the in-tree (FetchContent) layout
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

    void writeLater(const QString& key, const QString& value) override
    {
        auto* job = new QKeychain::WritePasswordJob(service());
        job->setAutoDelete(true);
        job->setKey(key);
        job->setTextData(value);
        QObject::connect(job, &QKeychain::Job::finished, [](QKeychain::Job* j) {
            if (j->error() != QKeychain::NoError) {
                qWarning().noquote() << "[SecretStore] could not store a secret in the OS keychain:"
                                     << j->errorString();
            }
        });
        job->start();
    }

    void removeLater(const QString& key) override
    {
        auto* job = new QKeychain::DeletePasswordJob(service());
        job->setAutoDelete(true);
        job->setKey(key);
        QObject::connect(job, &QKeychain::Job::finished, [](QKeychain::Job* j) {
            if (j->error() != QKeychain::NoError && j->error() != QKeychain::EntryNotFound) {
                qWarning().noquote() << "[SecretStore] could not delete a keychain entry:"
                                     << j->errorString();
            }
        });
        job->start();
    }

    bool available() const override
    {
        // Probed, not asked: QKeychain::isAvailable() answers true on Linux
        // whenever libsecret is linked, even with no Secret Service running,
        // and the first write then fails. One throwaway write+delete per run
        // is the only reliable answer. Cached: it does not change mid-run.
        if (!m_probed) {
            m_probed = true;
            auto* self = const_cast<KeychainSecretStore*>(this);
            const QString key = QStringLiteral("availability-probe");
            m_available = QKeychain::isAvailable() && self->write(key, QStringLiteral("probe"));
            if (m_available) self->remove(key);
        }
        return m_available;
    }

private:
    mutable bool m_probed = false;
    mutable bool m_available = false;

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
    void writeLater(const QString&, const QString&) override {}
    void removeLater(const QString&) override {}
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
