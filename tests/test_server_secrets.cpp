// Chat-server access tokens leave the plaintext settings file (audit C-5),
// and removing a server leaves no credential behind in it (C-4).
//
// Builds before this one wrote `servers/N/accessToken` (and a never-used
// `identityRefreshToken`) into QSettings — the registry, ~/.config, Android
// prefs, an iOS plist in iCloud backups. The first launch of this build moves
// each token into the OS keychain and deletes it from the file, along with
// the rows above `size` that beginWriteArray never removed. With no keychain
// the token still works for this run but is not written anywhere.

#include "core/SecretStore.h"
#include "core/ServerSecrets.h"

#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

class TestServerSecrets : public QObject {
    Q_OBJECT

    QTemporaryDir dir;
    int n = 0;

    // A fresh INI file per test, laid out exactly as Settings writes it.
    QString freshPath() { return dir.filePath(QStringLiteral("s%1.ini").arg(++n)); }

    static void writeLegacy(QSettings& s, const QList<QStringList>& rows)
    {
        s.beginWriteArray(QStringLiteral("servers"), int(rows.size()));
        for (int i = 0; i < rows.size(); ++i) {
            s.setArrayIndex(i);
            s.setValue("url", rows[i].value(0));
            s.setValue("userId", rows[i].value(1));
            s.setValue("accessToken", rows[i].value(2));
            s.setValue("deviceId", QStringLiteral("DEV%1").arg(i));
            s.setValue("displayName", QStringLiteral("name%1").arg(i));
            s.setValue("identityRefreshToken", QStringLiteral("legacy-rt"));
            s.setValue("identityProviderUrl", QStringLiteral("https://id.example"));
        }
        s.endArray();
        s.sync();
    }

    // Every value in the file, as one string, to grep for a secret.
    static QString everything(QSettings& s)
    {
        QString out;
        for (const QString& k : s.allKeys()) out += k + '=' + s.value(k).toString() + '\n';
        return out;
    }

private slots:
    void plaintextTokensMoveToTheStoreAndLeaveTheFile()
    {
        QSettings s(freshPath(), QSettings::IniFormat);
        writeLegacy(s, {{"https://a.example", "@me:a", "tokA"},
                        {"https://b.example", "@me:b", "tokB"}});
        MemorySecretStore store;
        ServerSecrets secrets(&store);

        const auto r = secrets.migrate(s);
        QCOMPARE(r.moved, 2);
        QCOMPARE(r.memoryOnly, 0);
        QVERIFY(r.rewrote);

        QCOMPARE(store.read(ServerSecrets::accessTokenKey("https://a.example", "@me:a"))
                     .value_or(QString()), QStringLiteral("tokA"));
        QCOMPARE(store.read(ServerSecrets::accessTokenKey("https://b.example", "@me:b"))
                     .value_or(QString()), QStringLiteral("tokB"));

        const QString file = everything(s);
        QVERIFY2(!file.contains("tokA") && !file.contains("tokB"), qPrintable(file));
        QVERIFY2(!file.contains("legacy-rt"), qPrintable(file));
        QVERIFY(!file.contains("accessToken"));
        // Everything that is not secret is untouched.
        QCOMPARE(s.beginReadArray("servers"), 2);
        s.setArrayIndex(1);
        QCOMPARE(s.value("url").toString(), QStringLiteral("https://b.example"));
        QCOMPARE(s.value("userId").toString(), QStringLiteral("@me:b"));
        QCOMPARE(s.value("deviceId").toString(), QStringLiteral("DEV1"));
        QCOMPARE(s.value("displayName").toString(), QStringLiteral("name1"));
        QCOMPARE(s.value("identityProviderUrl").toString(), QStringLiteral("https://id.example"));
        s.endArray();

        // And the token is served from the store, in this run and the next.
        QCOMPARE(secrets.accessToken("https://a.example", "@me:a"), QStringLiteral("tokA"));
        ServerSecrets nextRun(&store);
        QCOMPARE(nextRun.migrate(s).rewrote, false); // nothing left to clean
        QCOMPARE(nextRun.accessToken("https://b.example", "@me:b"), QStringLiteral("tokB"));
    }

    void staleRowsAboveSizeAreRemoved()
    {
        QSettings s(freshPath(), QSettings::IniFormat);
        writeLegacy(s, {{"https://a.example", "@me:a", "tokA"},
                        {"https://gone.example", "@me:gone", "tokGone"}});
        // What an old removeServer left: size 1, row 2 still on disk.
        s.beginWriteArray("servers", 1);
        s.setArrayIndex(0);
        s.setValue("url", "https://a.example");
        s.endArray();
        s.sync();
        QVERIFY(everything(s).contains("tokGone"));

        MemorySecretStore store;
        ServerSecrets secrets(&store);
        const auto r = secrets.migrate(s);
        QVERIFY(r.rewrote);
        QCOMPARE(r.moved, 1); // the stale row's token is deleted, not migrated
        QVERIFY(!everything(s).contains("tokGone"));
        QVERIFY(!everything(s).contains("gone.example"));
        QCOMPARE(store.size(), 1);
    }

    void aCleanFileIsLeftAlone()
    {
        QSettings s(freshPath(), QSettings::IniFormat);
        s.beginWriteArray("servers", 1);
        s.setArrayIndex(0);
        s.setValue("url", "https://a.example");
        s.setValue("userId", "@me:a");
        s.endArray();
        s.sync();
        MemorySecretStore store;
        ServerSecrets secrets(&store);
        const auto r = secrets.migrate(s);
        QVERIFY(!r.rewrote);
        QCOMPARE(r.moved, 0);
        QCOMPARE(store.size(), 0);
    }

    // The first of two rows for one account is the one ServerManager keeps,
    // so its token must be the one that survives.
    void duplicateRowsKeepTheFirstToken()
    {
        QSettings s(freshPath(), QSettings::IniFormat);
        writeLegacy(s, {{"https://a.example", "@me:a", "first"},
                        {"https://a.example", "@me:a", "second"}});
        MemorySecretStore store;
        ServerSecrets secrets(&store);
        QCOMPARE(secrets.migrate(s).moved, 1);
        QCOMPARE(secrets.accessToken("https://a.example", "@me:a"), QStringLiteral("first"));
    }

    // No secure store: the token still works this run, and is not on disk
    // anywhere afterwards. The user signs in again next launch.
    void withNoSecureStoreTheTokenIsMemoryOnly()
    {
        QSettings s(freshPath(), QSettings::IniFormat);
        writeLegacy(s, {{"https://a.example", "@me:a", "tokA"}});
        MemorySecretStore store;
        store.setAvailable(false);
        ServerSecrets secrets(&store);

        const auto r = secrets.migrate(s);
        QCOMPARE(r.moved, 0);
        QCOMPARE(r.memoryOnly, 1);
        QVERIFY(!secrets.persistent());
        QVERIFY(!everything(s).contains("tokA"));
        QCOMPARE(store.size(), 0);
        QCOMPARE(secrets.accessToken("https://a.example", "@me:a"), QStringLiteral("tokA"));

        // A new sign-in this run: usable, still not persisted.
        secrets.setAccessToken("https://b.example", "@me:b", "tokB");
        QCOMPARE(secrets.accessToken("https://b.example", "@me:b"), QStringLiteral("tokB"));
        QCOMPARE(store.size(), 0);

        ServerSecrets nextRun(&store);
        QVERIFY(nextRun.accessToken("https://a.example", "@me:a").isEmpty());
    }

    void forgettingDeletesFromTheStore()
    {
        MemorySecretStore store;
        ServerSecrets secrets(&store);
        secrets.setAccessToken("https://a.example", "@me:a", "tokA");
        QCOMPARE(store.size(), 1);
        secrets.forget("https://a.example", "@me:a");
        QCOMPARE(store.size(), 0);
        QVERIFY(secrets.accessToken("https://a.example", "@me:a").isEmpty());
        // A token written by an earlier run, never read in this one, is
        // still deleted.
        store.write(ServerSecrets::accessTokenKey("https://b.example", "@me:b"), "old");
        ServerSecrets fresh(&store);
        fresh.forget("https://b.example", "@me:b");
        QCOMPARE(store.size(), 0);
    }

    void keysSeparateServersAndAccounts()
    {
        const QString a = ServerSecrets::accessTokenKey("https://a.example", "@x:a");
        QVERIFY(a != ServerSecrets::accessTokenKey("https://a.example", "@y:a"));
        QVERIFY(a != ServerSecrets::accessTokenKey("https://b.example", "@x:a"));
    }
};

QTEST_GUILESS_MAIN(TestServerSecrets)
#include "test_server_secrets.moc"
