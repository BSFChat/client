// Source-level guards for QML hazards that nothing else can catch headlessly.
//
// The BSFChat QML module is compiled into the app binary, so no test target
// can instantiate qml/theme/Theme.qml or anything that imports it. That is
// why v0.0.44-rc.6 shipped unable to open a window: Theme.qml declared
//     readonly property color onScrim: "#ffffff"
// and QML parses any `on` + Capital name as a signal handler, so a literal
// value there is a load error ("Cannot assign a value to a signal"), the
// singleton fails, every component importing it becomes "unavailable", and
// main.qml never loads. qmllint did not flag it and ctest was green.
//
// Until the module is split into a library the tests can import, this scans
// the source text for the shape of the bug.
#include <QtTest>
#include <QFile>
#include <QRegularExpression>

class QmlHygieneTest : public QObject {
    Q_OBJECT
private slots:
    void themeHasNoSignalHandlerShapedProperties()
    {
        QFile f(QStringLiteral(BSFCHAT_QML_DIR "/theme/Theme.qml"));
        QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), "Theme.qml not found");
        const QString src = QString::fromUtf8(f.readAll());

        // `onAccent` predates this test and only survives because its value
        // is a script expression (a ternary), which the parser accepts as a
        // handler body. It is grandfathered, not endorsed; do not add more.
        static const QRegularExpression decl(
            QStringLiteral(R"(^\s*(?:readonly\s+)?property\s+\w+\s+(on[A-Z]\w*)\s*:)"),
            QRegularExpression::MultilineOption);
        QStringList offenders;
        for (auto it = decl.globalMatch(src); it.hasNext();) {
            const QString name = it.next().captured(1);
            if (name != QLatin1String("onAccent")) offenders << name;
        }
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral("signal-handler-shaped theme tokens (rename them): ")
                            + offenders.join(QStringLiteral(", "))));
    }

    // ---- Add-server dialog (2026-09-17) --------------------------------
    //
    // Same constraint, same technique: LoginDialog.qml imports the BSFChat
    // module, so no headless test can instantiate it and check what is on
    // screen. What CAN be checked is the shape of the visibility rules, and
    // the rule that matters is the one whose absence caused the bug — a
    // user typed the product domain `bsfchat.com` (not the homeserver
    // `chat.bsfchat.com`), the probe of a web server failed, the dialog
    // treated failure as "password-only", and offered to register them an
    // account on nginx.
    //
    // The C++ half is pinned properly in test_server_discovery. These are
    // the two things only the QML can get wrong.

    void loginDialogNeverOffersPasswordsBeforeTheServerAdvertisesThem()
    {
        const QString src = readQml(QStringLiteral("/components/LoginDialog.qml"));

        // Every visibility rule that turns on the password/register side of
        // the dialog must ALSO require `dialog.probed` — i.e. a login-flows
        // document actually came back. `passwordAvailable` alone is not
        // enough, because it has a default value and the dialog is visible
        // before any probe has run.
        int checked = 0;
        for (const QString& binding : visibleBindings(src)) {
            if (!binding.contains(QLatin1String("dialog.passwordAvailable"))) continue;
            ++checked;
            QVERIFY2(binding.contains(QLatin1String("dialog.probed")),
                     qPrintable(QStringLiteral(
                                    "password-side visibility rule without dialog.probed: ")
                                + binding));
        }
        // If the rules were renamed out from under this test it must fail,
        // not quietly pass having checked nothing.
        QVERIFY2(checked >= 3, "expected the username, password and button blocks");
    }

    void loginDialogReportsWhatTheProbeFound()
    {
        const QString src = readQml(QStringLiteral("/components/LoginDialog.qml"));

        // The new signal is handled at all...
        QVERIFY2(src.contains(QLatin1String("function onServerProbed(")),
                 "LoginDialog stopped listening to ServerManager::serverProbed");
        // ...and each outcome the user can hit has words for it.
        QVERIFY2(src.contains(QLatin1String("No BSFChat server found at")),
                 "the not_a_server case lost its message");
        QVERIFY2(src.contains(QLatin1String("Found server at")),
                 "the .well-known redirect case lost its message");
        QVERIFY2(src.contains(QLatin1String("Could not reach")),
                 "the unreachable case lost its message");

        // And every connect goes to the RESOLVED homeserver, so the saved
        // server entry is the address the client actually talks to rather
        // than whatever domain the user happened to know.
        for (const char* call : {"serverManager.addServerWithOidc(",
                                 "serverManager.addServer(",
                                 "serverManager.registerServer("}) {
            const qsizetype at = src.indexOf(QLatin1String(call));
            QVERIFY2(at >= 0, call);
            QVERIFY2(src.mid(at + qsizetype(qstrlen(call)), 20).contains(
                         QLatin1String("dialog.targetUrl()")),
                     qPrintable(QStringLiteral("%1 still connects to the raw text field")
                                    .arg(QLatin1String(call))));
        }
    }

private:
    static QString readQml(const QString& relative)
    {
        QFile f(QStringLiteral(BSFCHAT_QML_DIR) + relative);
        [&] { QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(f.fileName())); }();
        return QString::fromUtf8(f.readAll());
    }

    // Each `visible:` binding as one line, continuation lines (those
    // starting with && or ||) folded in. QML wraps these across three or
    // four lines, so a naive per-line search would miss half of every rule.
    static QStringList visibleBindings(const QString& src)
    {
        QStringList out;
        const QStringList lines = src.split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            if (!lines[i].trimmed().startsWith(QLatin1String("visible:"))) continue;
            QString binding = lines[i].trimmed();
            for (int j = i + 1; j < lines.size(); ++j) {
                const QString next = lines[j].trimmed();
                if (!next.startsWith(QLatin1String("&&")) && !next.startsWith(QLatin1String("||")))
                    break;
                binding += QLatin1Char(' ') + next;
            }
            out << binding;
        }
        return out;
    }
};

QTEST_APPLESS_MAIN(QmlHygieneTest)
#include "test_qml_hygiene.moc"
