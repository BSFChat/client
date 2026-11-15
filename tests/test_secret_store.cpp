// The real OS credential store behind SecretStore::instance(): Keychain,
// Credential Manager, Secret Service or Android Keystore via QtKeychain.
//
// Skips where no secure store is reachable (a headless CI runner has no
// Secret Service), which is also the case the app handles by keeping the
// BSFChat ID sign-in in memory only. To run it on Linux:
//   dbus-run-session -- sh -c 'echo x | gnome-keyring-daemon --unlock \
//       --components=secrets >/dev/null && ./test_secret_store'

#include "core/SecretStore.h"

#include <QCoreApplication>
#include <QTest>
#include <QUuid>

class TestSecretStore : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Namespaced away from any real profile's secrets.
        QCoreApplication::setApplicationName(QStringLiteral("BSFChat-secretstore-test"));
        if (!SecretStore::instance().available())
            QSKIP("no OS secure store on this system");
    }

    void aSecretRoundTripsAndIsRemoved()
    {
        SecretStore& store = SecretStore::instance();
        const QString key = QStringLiteral("test:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString value = QStringLiteral("refresh-token-") + QUuid::createUuid().toString();

        QVERIFY(!store.read(key).has_value());
        QVERIFY(store.write(key, value));
        QCOMPARE(store.read(key).value_or(QString()), value);

        // Overwrite, as every token rotation does.
        QVERIFY(store.write(key, value + "-2"));
        QCOMPARE(store.read(key).value_or(QString()), value + "-2");

        QVERIFY(store.remove(key));
        QVERIFY(!store.read(key).has_value());
        // Removing what is not there is not an error.
        QVERIFY(store.remove(key));
    }

    // The non-blocking variants Settings uses for access tokens: they run in
    // submission order with the blocking ones, so a read after a writeLater
    // sees the value and a read after a removeLater does not.
    void laterJobsRunInOrder()
    {
        SecretStore& store = SecretStore::instance();
        const QString key = QStringLiteral("test:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        store.writeLater(key, QStringLiteral("a"));
        store.writeLater(key, QStringLiteral("b"));
        QCOMPARE(store.read(key).value_or(QString()), QStringLiteral("b"));
        store.removeLater(key);
        QVERIFY(!store.read(key).has_value());
    }
};

QTEST_GUILESS_MAIN(TestSecretStore)
#include "test_secret_store.moc"
