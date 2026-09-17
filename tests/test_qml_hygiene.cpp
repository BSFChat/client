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
//
// v0.0.44-rc.7 added three more of the same kind, all of them warnings in a
// log nobody reads during a release, all of them invisible to a green ctest:
//
//   Settings.h        Q_INVOKABLE declared in the leading (private) block, so
//                     QML could not call it
//   LinkPreview.qml   an XMLHttpRequest callback still firing after the
//                     delegate that installed it was destroyed
//   VoiceDock.qml     Window.window written inside Connections, which is not
//                     an Item, so it silently evaluated to null
//
// One check each, below. Each one fails on the commit before its fix.
//
// NOT checked here: "every _name used in a QML file is declared in it". It
// was considered and dropped. It needs a JS lexer to be trustworthy —
// MessageInput.qml alone contains /`([^`\n]+)`/, a regex literal holding
// backticks, which a naive scanner reads as a template literal and then
// swallows the next twelve lines of real code, including a function
// declaration, and reports that function as undeclared. More to the point it
// would not have caught the bug that prompted it: `_failed` and
// `_looksLikeChallenge` were both declared on LinkPreview. What went wrong
// was when the callback ran, not what it named.
#include <QtTest>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QRegularExpression>

namespace {

// Comments only — string literals are left alone. Stripping can therefore
// only ever remove text (a "//" inside a URL string eats the rest of that
// line), so the scans below can miss an offender but cannot invent one.
QString withoutComments(QString src)
{
    static const QRegularExpression block(QStringLiteral(R"(/\*.*?\*/)"),
                                          QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression line(QStringLiteral("//[^\n]*"));
    return src.remove(block).remove(line);
}

QString readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

QStringList filesUnder(const QString& root, const QString& glob)
{
    QStringList out;
    QDirIterator it(root, QStringList{glob}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) out << it.next();
    out.sort();
    return out;
}

} // namespace

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

    // moc records a private Q_INVOKABLE but the metaobject does not offer it
    // to QML, so the call fails at runtime with "… is not a function" and
    // takes the rest of the handler with it. Q_PROPERTY is not affected by
    // access, which is what makes the mistake easy: put an invokable in the
    // Q_PROPERTY block it belongs with, at the top of a `class`, and
    // everything around it keeps working.
    //
    // The parse is textual but not naive: it tracks brace depth so a nested
    // class has its own access state, and it knows `class` defaults to
    // private while `struct` defaults to public. A base-class list
    // ("class Settings : public QObject") is not an access specifier and is
    // not treated as one.
    void everyQInvokableIsPublic()
    {
        static const QRegularExpression classDecl(
            QStringLiteral(R"(^\s*(?:template\s*<[^>]*>\s*)?(class|struct)\s+\w+)"));
        static const QRegularExpression accessSpec(
            QStringLiteral(R"(^\s*(public|private|protected)\s*(?:slots|Q_SLOTS)?\s*:)"));
        static const QRegularExpression signalsSpec(
            QStringLiteral(R"(^\s*(?:signals|Q_SIGNALS)\s*:)"));

        const QStringList headers = filesUnder(QStringLiteral(BSFCHAT_SRC_DIR),
                                               QStringLiteral("*.h"));
        QVERIFY2(!headers.isEmpty(), "no headers found under BSFCHAT_SRC_DIR");

        QStringList offenders;
        for (const QString& path : headers) {
            const QStringList lines = withoutComments(readAll(path)).split(QLatin1Char('\n'));
            int depth = 0;
            QList<QPair<int, QString>> scopes;   // brace depth -> current access
            QString pendingKind;                 // class/struct seen, awaiting '{'

            for (int i = 0; i < lines.size(); ++i) {
                const QString& line = lines.at(i);

                const auto decl = classDecl.match(line);
                if (decl.hasMatch()
                    && !line.left(line.indexOf(QLatin1Char('{'))).contains(QLatin1Char(';'))) {
                    pendingKind = decl.captured(1);
                }
                if (!scopes.isEmpty()) {
                    const auto acc = accessSpec.match(line);
                    if (acc.hasMatch()) scopes.last().second = acc.captured(1);
                    else if (signalsSpec.match(line).hasMatch())
                        scopes.last().second = QStringLiteral("signals");
                }
                if (line.contains(QLatin1String("Q_INVOKABLE"))) {
                    const QString access = scopes.isEmpty()
                        ? QStringLiteral("file scope") : scopes.last().second;
                    if (access != QLatin1String("public")) {
                        offenders << QStringLiteral("%1:%2 (%3): %4")
                                         .arg(QFileInfo(path).fileName())
                                         .arg(i + 1)
                                         .arg(access, line.trimmed());
                    }
                }
                for (const QChar c : line) {
                    if (c == QLatin1Char('{')) {
                        ++depth;
                        if (!pendingKind.isEmpty()) {
                            scopes.append({depth,
                                           pendingKind == QLatin1String("class")
                                               ? QStringLiteral("private")
                                               : QStringLiteral("public")});
                            pendingKind.clear();
                        }
                    } else if (c == QLatin1Char('}')) {
                        if (!scopes.isEmpty() && scopes.last().first == depth) scopes.removeLast();
                        --depth;
                    }
                }
            }
        }
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral("Q_INVOKABLE not under `public:` — QML cannot call these:\n  ")
                            + offenders.join(QStringLiteral("\n  "))));
    }

    // A QML XMLHttpRequest callback is a plain JS closure and nothing owns
    // it. It fires whether or not the object that installed it still exists,
    // and once that object is gone every unqualified name in the closure
    // resolves against a dead scope: reads raise ReferenceError, writes land
    // on the global object ("Invalid write to global property"). Delegates in
    // a message list are destroyed and recreated constantly, so this is the
    // normal case, not the edge case.
    //
    // The rule is therefore: if a component starts a request, it has to be
    // able to stop one. Nothing here can prove the abort is wired to the
    // right request — that is what tst_linkpreviewparse.qml and the manual
    // destroy-mid-fetch check are for — but a file that fetches and has no
    // teardown at all cannot be correct.
    void componentsThatFetchCanCancel()
    {
        const QStringList qmlFiles = filesUnder(QStringLiteral(BSFCHAT_QML_DIR),
                                                QStringLiteral("*.qml"));
        QVERIFY2(!qmlFiles.isEmpty(), "no QML files found under BSFCHAT_QML_DIR");

        QStringList offenders;
        for (const QString& path : qmlFiles) {
            const QString src = withoutComments(readAll(path));
            if (!src.contains(QLatin1String("new XMLHttpRequest"))) continue;
            if (src.contains(QLatin1String("Component.onDestruction"))
                && src.contains(QLatin1String(".abort()"))) {
                continue;
            }
            offenders << QFileInfo(path).fileName();
        }
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "these start an XMLHttpRequest but never abort one in "
                     "Component.onDestruction, so their callbacks can run on a "
                     "destroyed object: ") + offenders.join(QStringLiteral(", "))));
    }

    // Connections is a QObject, not an Item. `Window.window` inside one
    // attaches to the Connections object, which Qt refuses — "Window.window
    // does only support types deriving from Item" — and the expression is
    // null. Written as a guard (`if (Window.window && …)`) that null is
    // indistinguishable from "no window yet", so the body never runs and
    // nothing looks broken except the missing behaviour: in VoiceDock.qml it
    // meant screen-share and camera errors raised no toast at all, for as
    // long as the code has existed. Reach the window through an Item id.
    void connectionsDoNotUseTheWindowAttachedProperty()
    {
        static const QRegularExpression unqualified(
            QStringLiteral(R"((?<![\w.])Window\s*\.\s*window)"));

        const QStringList qmlFiles = filesUnder(QStringLiteral(BSFCHAT_QML_DIR),
                                                QStringLiteral("*.qml"));
        QStringList offenders;
        for (const QString& path : qmlFiles) {
            const QString src = withoutComments(readAll(path));
            int from = 0;
            while (true) {
                const qsizetype open = src.indexOf(QRegularExpression(QStringLiteral(R"(\bConnections\s*\{)")),
                                             from);
                if (open < 0) break;
                qsizetype i = src.indexOf(QLatin1Char('{'), open);
                const qsizetype bodyStart = i + 1;
                int nesting = 0;
                for (; i < src.size(); ++i) {
                    if (src.at(i) == QLatin1Char('{')) ++nesting;
                    else if (src.at(i) == QLatin1Char('}') && --nesting == 0) break;
                }
                const QString body = src.mid(bodyStart, i - bodyStart);
                if (unqualified.match(body).hasMatch())
                    offenders << QFileInfo(path).fileName();
                from = i + 1;
            }
        }
        offenders.removeDuplicates();
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "unqualified Window.window inside a Connections block (it is not an "
                     "Item, so this is null — go through an Item id): ")
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
