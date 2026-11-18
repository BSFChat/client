#include "core/SecretStore.h"

#include "core/AppProfile.h"

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QPointer>
#include <QTimer>

#ifdef BSFCHAT_HAVE_KEYCHAIN
#  include <qtkeychain/keychain.h> // the in-tree (FetchContent) layout
#endif

namespace {

#ifdef BSFCHAT_HAVE_KEYCHAIN
class KeychainSecretStore : public SecretStore {
public:
    bool write(const QString& key, const QString& value) override
    {
        auto* job = new QKeychain::WritePasswordJob(service());
        job->setKey(key);
        job->setTextData(value);
        const Outcome out = run(job);
        if (out.error != QKeychain::NoError) {
            qWarning().noquote() << "[SecretStore] could not store a secret in the OS keychain:"
                                 << out.errorString;
            return false;
        }
        return true;
    }

    std::optional<QString> read(const QString& key) override
    {
        auto* job = new QKeychain::ReadPasswordJob(service());
        job->setKey(key);
        const Outcome out = run(job);
        if (out.error != QKeychain::NoError) {
            if (out.error != QKeychain::EntryNotFound) {
                qWarning().noquote() << "[SecretStore] could not read the OS keychain:"
                                     << out.errorString;
            }
            return std::nullopt;
        }
        return out.text;
    }

    bool remove(const QString& key) override
    {
        auto* job = new QKeychain::DeletePasswordJob(service());
        job->setKey(key);
        const Outcome out = run(job);
        return out.error == QKeychain::NoError || out.error == QKeychain::EntryNotFound;
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
    // A job that outlived its deadline. QtKeychain runs jobs strictly one
    // at a time, so everything after it would queue behind it: until it
    // finishes (and deletes itself) synchronous calls fail at once instead
    // of each waiting out the deadline again.
    QPointer<QKeychain::Job> m_stuck;

    // Generous: a macOS keychain prompt is answered by a human. Without a
    // deadline a job whose completion never arrives froze the caller for
    // good. That is what a GUI-less QCoreApplication on macOS does:
    // QtKeychain posts completion to the main GCD queue, which only the
    // CoreFoundation event dispatcher drains (QApplication's, not
    // QCoreApplication's), so test_secret_store sat in initTestCase until
    // QTest's watchdog aborted it. A Keychain call blocked behind a
    // SecurityAgent prompt that can never be shown is the same hang.
    static constexpr int kDeadlineMs = 30000;

    struct Outcome {
        QKeychain::Error error = QKeychain::OtherError;
        QString errorString;
        QString text;
    };

    static QString service()
    {
        // Profile-aware, like every other piece of persisted state
        // (see core/AppProfile.h): two profiles must never share a login.
        return QStringLiteral("com.bsfchat.%1").arg(bsfchat::applicationName());
    }

    // Takes ownership of `job`. It deletes itself when it finishes, which
    // may be after the deadline: QtKeychain's Apple completion dereferences
    // the job's private data without a null check, so a job must never be
    // destroyed while its request is still in flight.
    Outcome run(QKeychain::Job* job)
    {
        job->setAutoDelete(true);
        Outcome out;
        if (m_stuck) {
            job->deleteLater();
            out.errorString = QStringLiteral("an earlier keychain request has not completed");
            return out;
        }
        bool finished = false;
        QEventLoop loop;
        // Context `&loop`: a completion after the deadline finds the
        // connection gone with the loop and touches nothing on this stack.
        QObject::connect(job, &QKeychain::Job::finished, &loop, [&](QKeychain::Job* j) {
            finished = true;
            out.error = j->error();
            out.errorString = j->errorString();
            if (auto* r = qobject_cast<QKeychain::ReadPasswordJob*>(j)) out.text = r->textData();
            loop.quit();
        });
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        deadline.start(kDeadlineMs);
        job->start();
        loop.exec(QEventLoop::ExcludeUserInputEvents);
        if (!finished) {
            m_stuck = job;
            out.error = QKeychain::OtherError;
            out.errorString = QStringLiteral("the OS keychain did not answer within %1 s")
                                  .arg(kDeadlineMs / 1000);
        }
        return out;
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
