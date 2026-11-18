// The real OS credential store behind SecretStore::instance(): Keychain,
// Credential Manager, Secret Service or Android Keystore via QtKeychain.
//
// Skips where no secure store is reachable (a headless CI runner has no
// Secret Service), which is also the case the app handles by keeping the
// BSFChat ID sign-in in memory only. A store that never answers (a CI
// runner's locked login keychain waiting on a prompt nobody can see) hits
// SecretStore's deadline and skips too. To run it on Linux:
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

// Not QTEST_GUILESS_MAIN: on macOS a plain QCoreApplication uses the UNIX
// event dispatcher, which never drains the main GCD queue QtKeychain posts
// its completions to, so no job would ever finish (the app's QApplication
// gets the CoreFoundation dispatcher and does not have this problem).
int main(int argc, char** argv)
{
#ifdef Q_OS_DARWIN
    qputenv("QT_EVENT_DISPATCHER_CORE_FOUNDATION", "1");
#endif
    QCoreApplication app(argc, argv);
    TestSecretStore tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}
#include "test_secret_store.moc"
