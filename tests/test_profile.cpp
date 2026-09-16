// Multi-instance (`--profile <name>`) namespace resolution.
//
// This is deliberately a pure-logic test: it never constructs the GUI app
// (launching bsfchat-app on a dev Mac triggers a TCC permission storm), it
// only pins the four names a second instance must not share with the first
// — the QSettings domain, the AppDataLocation that LocalCache lives under,
// the macOS log directory, and the single-instance IPC socket.
//
// The load-bearing assertion is DefaultIsUnchanged: with no flag and no
// env var everything must resolve to exactly what shipped before, or every
// existing install silently loses its settings, cache and logins.

#include "core/AppProfile.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>

using namespace bsfchat;

namespace {

// profileFromArguments takes argv, so build one.
QString parse(const QStringList& args, const QByteArray& env = {})
{
    QList<QByteArray> storage;
    storage.reserve(args.size());
    std::vector<char*> argv;
    for (const QString& a : args) {
        storage.append(a.toLocal8Bit());
    }
    for (QByteArray& a : storage) argv.push_back(a.data());
    return profileFromArguments(static_cast<int>(argv.size()), argv.data(), env);
}

} // namespace

class TestProfile : public QObject {
    Q_OBJECT
private slots:

    // --- The compatibility guarantee -----------------------------------

    void defaultIsUnchanged()
    {
        QCOMPARE(activeProfile(), QString());
        QCOMPARE(organizationNameForProfile(QString()), QStringLiteral("BSFChat"));
        QCOMPARE(applicationNameForProfile(QString()), QStringLiteral("BSFChat"));
        QCOMPARE(socketSuffixForProfile(QString()), QString());

        // No flag, no env var -> no profile.
        QCOMPARE(parse({"bsfchat-app"}), QString());
        QCOMPARE(parse({"bsfchat-app", "bsfchat://example.org/room/x"}), QString());
    }

    // --- Argument / env parsing ----------------------------------------

    void parsesSpaceSeparatedFlag()
    {
        QCOMPARE(parse({"bsfchat-app", "--profile", "alice"}),
                 QStringLiteral("alice"));
    }

    void parsesEqualsForm()
    {
        QCOMPARE(parse({"bsfchat-app", "--profile=bob"}), QStringLiteral("bob"));
    }

    void flagBeatsEnvironment()
    {
        QCOMPARE(parse({"bsfchat-app", "--profile", "alice"}, "bob"),
                 QStringLiteral("alice"));
    }

    void fallsBackToEnvironment()
    {
        QCOMPARE(parse({"bsfchat-app"}, "bob"), QStringLiteral("bob"));
    }

    void trailingFlagWithNoValueIsIgnored()
    {
        // `--profile` as the last argument must not read past argv's end.
        QCOMPARE(parse({"bsfchat-app", "--profile"}), QString());
    }

    void coexistsWithAUrlArgument()
    {
        QCOMPARE(parse({"bsfchat-app", "--profile", "alice",
                        "bsfchat://example.org/room/x"}),
                 QStringLiteral("alice"));
    }

    // --- Sanitizing ----------------------------------------------------

    void sanitizeKeepsSafeCharacters()
    {
        QCOMPARE(sanitizeProfileName("alice"), QStringLiteral("alice"));
        QCOMPARE(sanitizeProfileName("Alice-2_test.1"),
                 QStringLiteral("Alice-2_test.1"));
    }

    void sanitizeRejectsPathTraversal()
    {
        // The profile lands in a directory name and a socket name; a name
        // that can escape either is the whole risk of this feature.
        const QString cleaned = sanitizeProfileName("../../etc/passwd");
        QVERIFY(!cleaned.contains(QLatin1Char('/')));
        QVERIFY(!cleaned.startsWith(QLatin1Char('.')));
        QCOMPARE(sanitizeProfileName("a/b"), QStringLiteral("a_b"));
        QCOMPARE(sanitizeProfileName("a b"), QStringLiteral("a_b"));
        QCOMPARE(sanitizeProfileName("a:b\\c"), QStringLiteral("a_b_c"));
    }

    void sanitizeTrimsAndCaps()
    {
        QCOMPARE(sanitizeProfileName("--alice--"), QStringLiteral("alice"));
        QCOMPARE(sanitizeProfileName(QString(80, QLatin1Char('x'))).size(), 32);
    }

    void sanitizeRejectsEmptyAndPunctuationOnly()
    {
        QCOMPARE(sanitizeProfileName(""), QString());
        QCOMPARE(sanitizeProfileName("   "), QString());
        QCOMPARE(sanitizeProfileName("///"), QString());
        QCOMPARE(sanitizeProfileName("..."), QString());
    }

    // --- The four namespaces -------------------------------------------

    void applicationNameCarriesTheProfile()
    {
        QCOMPARE(applicationNameForProfile("alice"),
                 QStringLiteral("BSFChat-alice"));
        // Organization is intentionally stable across profiles.
        QCOMPARE(organizationNameForProfile("alice"), QStringLiteral("BSFChat"));
    }

    void socketSuffixSeparatesInstances()
    {
        QCOMPARE(socketSuffixForProfile("alice"), QStringLiteral("-alice"));
        QVERIFY(socketSuffixForProfile("alice") != socketSuffixForProfile("bob"));
    }

    void settingsPathsDifferPerProfile()
    {
        // QSettings resolves its file from org + app name, so this is the
        // real check that two profiles cannot share a settings file.
        QSettings def(organizationNameForProfile(QString()),
                      applicationNameForProfile(QString()));
        QSettings alice(organizationNameForProfile("alice"),
                        applicationNameForProfile("alice"));
        QSettings bob(organizationNameForProfile("bob"),
                      applicationNameForProfile("bob"));

        QVERIFY(!def.fileName().isEmpty());
        QVERIFY(def.fileName() != alice.fileName());
        QVERIFY(alice.fileName() != bob.fileName());
        QVERIFY(alice.fileName().contains(QStringLiteral("BSFChat-alice")));
    }

    void appDataLocationDiffersPerProfile()
    {
        // LocalCache lives at AppDataLocation + "/cache" and the macOS file
        // log at ~/Library/Logs/<applicationName>; both follow the app name
        // set in main(), so exercise that here.
        const QString saved = QCoreApplication::applicationName();

        QCoreApplication::setApplicationName(
            applicationNameForProfile(QString()));
        const QString defaultData = QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation);

        QCoreApplication::setApplicationName(applicationNameForProfile("alice"));
        const QString aliceData = QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation);

        QCoreApplication::setApplicationName(saved);

        QVERIFY(!defaultData.isEmpty());
        QVERIFY(defaultData != aliceData);
        QVERIFY(aliceData.contains(QStringLiteral("BSFChat-alice")));
        // ...and the default path is still the plain one.
        QVERIFY(!defaultData.contains(QStringLiteral("BSFChat-")));
    }

    void activeProfileIsSanitizedOnSet()
    {
        setActiveProfile("../evil");
        QVERIFY(!activeProfile().contains(QLatin1Char('/')));
        QCOMPARE(applicationName(),
                 QStringLiteral("BSFChat-") + activeProfile());

        setActiveProfile("alice");
        QCOMPARE(activeProfile(), QStringLiteral("alice"));
        QCOMPARE(applicationName(), QStringLiteral("BSFChat-alice"));
        QCOMPARE(organizationName(), QStringLiteral("BSFChat"));

        // Reset — defaultIsUnchanged() must hold regardless of test order.
        setActiveProfile(QString());
        QCOMPARE(applicationName(), QStringLiteral("BSFChat"));
    }
};

QTEST_MAIN(TestProfile)
#include "test_profile.moc"
