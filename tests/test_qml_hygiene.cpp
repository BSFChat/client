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

// STILL not checked here, for the same reason: the general form of that rule.
//
// What IS checked, as of the 0.0.53 store submission, is the narrow case that
// actually shipped broken — a shared component reaching for a name that only
// one shell defines. QML ids resolve per component scope, so
//
//     qml/components/ChannelList.qml:  onClicked: loginDialog.open()
//
// is a ReferenceError under qml/mobile/MobileMain.qml, which declares the same
// LoginDialog under `loginDialogGlobal`. On a phone that button is "Add a
// server" on the empty state of a fresh install: no server, so no sign-in, so
// no app. Nothing caught it because nothing looked.
//
// The general rule needs a JS lexer; these two do not, because they match a
// *finite known vocabulary* (the ids and the root-level members the two shells
// actually declare) instead of trying to prove every identifier resolves:
//
//   aSharedComponentNeverReachesForAShellsId()
//   everyShellHelperASharedComponentCallsExistsOnBothShells()
//
// See each one's own comment for exactly what it does not catch.
#include <QtTest>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QRegularExpression>
#include <QSet>

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


// Every `id: foo` in a source text. Ids are lowerCamelCase by QML rule (the
// engine rejects a capitalised id), so the character class is exact.
QSet<QString> declaredIds(const QString& src)
{
    static const QRegularExpression re(QStringLiteral(R"(\bid\s*:\s*([a-z_]\w*)\b)"));
    QSet<QString> out;
    for (auto it = re.globalMatch(src); it.hasNext();) out.insert(it.next().captured(1));
    return out;
}

// Names a file introduces itself: properties, functions, signals, at any
// depth. Used only ever to SUPPRESS a report, so over-collecting is the safe
// direction — a nested `function foo()` hiding a real offender named `foo`
// costs a miss, never a false alarm.
QSet<QString> declaredMembers(const QString& src)
{
    static const QRegularExpression prop(
        QStringLiteral(R"(\bproperty\s+(?:alias\s+|var\s+|[\w.<>]+\s+)([A-Za-z_]\w*))"));
    static const QRegularExpression func(
        QStringLiteral(R"(\bfunction\s+([A-Za-z_]\w*)\s*\()"));
    static const QRegularExpression sig(
        QStringLiteral(R"(\bsignal\s+([A-Za-z_]\w*)\b)"));
    QSet<QString> out;
    for (const QRegularExpression* re : {&prop, &func, &sig})
        for (auto it = re->globalMatch(src); it.hasNext();) out.insert(it.next().captured(1));
    return out;
}

// 1-based line number of an offset, for an error message a reader can open.
int lineOf(const QString& src, qsizetype offset)
{
    return static_cast<int>(QStringView(src).left(offset).count(QLatin1Char('\n'))) + 1;
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
    // A dialog that clamps its own height must be able to scroll.
    //
    // `height: Math.min(implicitHeight, parent.height - 32)` is the right way
    // to keep a dialog inside the window — but with a plain Layout as
    // contentItem the surplus is not compressed, it is clipped. LoginDialog
    // did exactly that: main.qml sets the window minimum to 500 px, the
    // sign-in form is taller, and at the minimum size the password fields and
    // the Sign In button sat below the cut with no way to reach them. The app
    // could not be signed into at a size it lets you resize to.
    //
    // There is no test binary that can instantiate these components (the
    // BSFChat QML module is compiled into the app), so this is a source scan:
    // if a file clamps a height against the parent, it must also contain a
    // Flickable or a ScrollView.
    void clampedDialogsCanScroll()
    {
        static const QRegularExpression clamps(
            QStringLiteral(R"(height\s*:\s*Math\.min\s*\(\s*implicitHeight)"));

        const QStringList qml = filesUnder(QStringLiteral(BSFCHAT_QML_DIR),
                                           QStringLiteral("*.qml"));
        QVERIFY2(!qml.isEmpty(), "no QML found under BSFCHAT_QML_DIR");

        QStringList offenders;
        int checked = 0;
        for (const QString& path : qml) {
            const QString src = withoutComments(readAll(path));
            if (!clamps.match(src).hasMatch()) continue;
            ++checked;
            if (!src.contains(QStringLiteral("Flickable"))
                && !src.contains(QStringLiteral("ScrollView"))
                && !src.contains(QStringLiteral("ListView"))) {
                offenders << QFileInfo(path).fileName();
            }
        }
        QVERIFY2(checked > 0,
                 "no clamped dialog found — has the pattern changed? This "
                 "guard is only meaningful while one exists.");
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "dialog clamps its height but cannot scroll, so content "
                     "past the clamp is unreachable: ")
                     + offenders.join(QStringLiteral(", "))));
    }

    // A surface that carries a SERVER's error text may not cut it silently.
    //
    // ToastHost is where almost every server error in this client lands:
    // login and registration failures, upload failures, a refused
    // notification-level change, every state write ServerSettings makes, and
    // voice. Its message Text carried `maximumLineCount: 4` with
    // `elide: Text.ElideRight` and no way out — the card is not selectable,
    // it is gone in six seconds, and the part an ellipsis eats is the END of
    // the sentence, which for a validation error is precisely the half that
    // says what to type instead.
    //
    // That is how the 2026-09-20 support call went: a server owner read a 400
    // about bot usernames as far as "…and", and the list of characters he was
    // allowed to use was never on his screen at all. It is a shared surface,
    // so it was never going to stay a one-off.
    //
    // The cap itself is fine and stays — a proxy's HTML error page must not
    // be able to paint a four-thousand-line card over the window. What is not
    // fine is a cap you cannot get past. So: if a toast clamps its message, it
    // must clamp it CONDITIONALLY, on some state the user can change.
    //
    // Source-scanned rather than exercised, for the usual reason: ToastHost
    // imports the BSFChat module, which only the app binary has.
    void aToastCannotSilentlyEatTheEndOfAServerError()
    {
        const QString src = withoutComments(readAll(
            QStringLiteral(BSFCHAT_QML_DIR "/components/ToastHost.qml")));
        QVERIFY2(!src.isEmpty(), "ToastHost.qml not found");

        // Both properties, wherever they appear in this file. A second Text
        // that clamps unconditionally is the same bug in a new place.
        static const QRegularExpression clamps(
            QStringLiteral(R"((maximumLineCount|elide)\s*:\s*([^\n]*))"));
        QStringList offenders;
        int checked = 0;
        for (auto it = clamps.globalMatch(src); it.hasNext();) {
            const auto m = it.next();
            ++checked;
            // `expanded` is the escape hatch the card offers. Any other
            // state would do — what may not appear is a constant.
            if (m.captured(2).contains(QStringLiteral("expanded"))) continue;
            if (m.captured(2).contains(QStringLiteral("Text.ElideNone"))) continue;
            offenders << m.captured(0).trimmed();
        }
        QVERIFY2(checked > 0,
                 "ToastHost no longer clamps its message at all — if that is "
                 "deliberate, delete this guard rather than leaving it green "
                 "on a file it stopped describing.");
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "a toast clamps a server's error text with no way to read "
                     "the rest of it: ")
                     + offenders.join(QStringLiteral("; "))));

        // And the way out has to be findable. An expandable card that looks
        // identical to one that fits is a feature nobody discovers.
        QVERIFY2(src.contains(QStringLiteral("truncated")),
                 "nothing in ToastHost notices that it truncated, so it cannot "
                 "offer to show the rest");
    }

    // A settings pane may not lay content out underneath its scrollbar.
    //
    // The owner's report, verbatim: "Right now, I can't fully read the
    // values of some settings because they are 'behind' the scrollbar (the
    // values are off to the right)." A ScrollBar attached to a Flickable,
    // ScrollView or ListView is an OVERLAY — Qt gives it no layout box — so
    // a content item bound to the full viewport width runs underneath it,
    // and a SettingRow puts its value at exactly that edge.
    //
    // The fix is ThemedScrollBar.reservedWidth, subtracted by whoever sizes
    // the content. Nothing at runtime notices it going missing — the layout
    // is still valid, just overlapped — and no headless test can measure the
    // overlap, so this scans for the one shape that always produces it: a
    // content item bound to the FULL viewport width, i.e. `<id>Flick.width`
    // or `ListView.view.width`, with no gutter taken back out.
    //
    // Panes that inset their content some other way are deliberately NOT
    // required to subtract anything — ChannelSettings.qml's column sits
    // 16px in from the ScrollView's edge, which already clears the bar's
    // 14px, and forcing a second gutter there would just make the dialog
    // lopsided. The check is about the binding that cannot be safe, not
    // about the presence of a bar.
    void settingsPanesReserveRoomForTheirScrollbar()
    {
        static const QRegularExpression fullWidth(
            QStringLiteral(R"((\w*Flick\.width|ListView\.view\.width))"));

        const QStringList panes = filesUnder(QStringLiteral(BSFCHAT_QML_DIR),
                                             QStringLiteral("*Settings*.qml"));
        QVERIFY2(!panes.isEmpty(), "no *Settings*.qml found under BSFCHAT_QML_DIR");

        QStringList offenders;
        int checked = 0;
        for (const QString& path : panes) {
            const QString src = withoutComments(readAll(path));
            if (!src.contains(QStringLiteral("ThemedScrollBar"))) continue;
            for (const QString& binding : widthBindings(src)) {
                if (!fullWidth.match(binding).hasMatch()) continue;
                ++checked;
                if (binding.contains(QStringLiteral("reservedWidth"))) continue;
                offenders << QStringLiteral("%1: %2")
                                 .arg(QFileInfo(path).fileName(), binding);
            }
        }
        QVERIFY2(checked > 0,
                 "no settings pane binds content to the full viewport width — "
                 "has the pattern changed? This guard is only meaningful while "
                 "one does.");
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "content is sized to the full scrolling viewport, so the "
                     "overlaid scrollbar covers its right edge — subtract "
                     "ThemedScrollBar.reservedWidth: ")
                     + offenders.join(QStringLiteral(" | "))));
    }

    // QSettings must be named through AppProfile, never with literals.
    //
    // --profile exists so two accounts can run on one machine, and it works
    // by giving each profile its own QSettings application name. Any code
    // that hard-codes ("BSFChat", "BSFChat") silently reads and writes the
    // DEFAULT profile's store instead of its own. That is how the beta
    // channel toggle came to be a no-op under --profile: UpdateChannel-
    // Settings wrote through Settings (profile-aware) while the Updater
    // read its own literal handle (not), so the switch moved nothing and
    // `updater.channel` disagreed with `appSettings.updateChannel` in the
    // same dialog. The default-constructed QSettings() form is fine — it
    // picks up QCoreApplication's names, which main() sets from the profile.
    void qsettingsAreNamedThroughAppProfile()
    {
        // Two string literals as the first two constructor arguments.
        // Written with \x22 rather than a literal quote: moc mis-lexes a
        // raw string that contains a double quote and silently drops the
        // rest of the class, which links as a missing vtable.
        static const QRegularExpression literalPair(
            QStringLiteral(R"(QSettings\s+\w+\s*\(\s*\x22[^\x22]*\x22\s*,\s*\x22)"));

        // Windows shell registration writes real registry paths through
        // QSettings, which has nothing to do with the profile store.
        const QStringList exempt{QStringLiteral("src/core/UrlHandler.cpp")};

        const QString root = QStringLiteral(BSFCHAT_SRC_DIR);
        QStringList sources = filesUnder(root, QStringLiteral("*.cpp"));
        sources += filesUnder(root, QStringLiteral("*.h"));
        QVERIFY2(!sources.isEmpty(), "no sources found under BSFCHAT_SRC_DIR");

        QStringList offenders;
        for (const QString& path : sources) {
            QString rel = path;
            const int cut = rel.indexOf(QStringLiteral("/src/"));
            if (cut >= 0) rel = rel.mid(cut + 1);
            if (exempt.contains(rel)) continue;
            if (literalPair.match(withoutComments(readAll(path))).hasMatch())
                offenders << rel;
        }
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "QSettings constructed with literal org/app names — use "
                     "bsfchat::organizationName()/applicationName() so --profile "
                     "is honoured: ") + offenders.join(QStringLiteral(", "))));
    }

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

    // ---- Cross-shell scope, the two halves ------------------------------
    //
    // qml/components/ is shared verbatim between qml/main.qml (desktop) and
    // qml/mobile/MobileMain.qml (phone). A component can therefore only rely
    // on what BOTH shells offer, and QML gives it two ways to forget that.

    // Half one: an `id`.
    //
    // A component that writes `loginDialog.open()` compiles, passes qmllint
    // and works on the desktop, because main.qml happens to declare
    // `id: loginDialog` in an enclosing component scope. Under MobileMain,
    // which declares the same dialog as `loginDialogGlobal`, the name
    // resolves to nothing and the handler dies with a ReferenceError.
    //
    // The rule: for every id either shell declares, no file under
    // qml/components/ may reference it unless that file declares it too.
    // The vocabulary is finite and known, so this needs no JS lexer.
    //
    // WHAT IT DOES NOT CATCH, precisely:
    //   * a name no shell declares at all (a plain typo). Out of scope --
    //     that is the general rule this file's header explains it cannot do.
    //   * a reference inside a string literal is counted, not skipped. Only
    //     comments are stripped. A string containing `someShellId.` or
    //     `someShellId(` would be reported; none exists, and one would be
    //     worth a look anyway.
    //   * a file that declares its own `property`/`function`/`signal` of the
    //     same name is skipped wholesale, so a second, genuinely cross-shell
    //     use of that same name in that same file is missed.
    //   * a reference built at runtime (`root["login" + "Dialog"]`). Nothing
    //     in the tree does this.
    //   * it reports the FIRST use of each name per file, not every use. The
    //     build fails either way; a fixer working from the message should
    //     re-run rather than assume one line per name is the whole of it.
    //   * it says nothing about whether a shared component is reachable from
    //     the mobile shell. It is deliberately stricter than that: a
    //     component nobody mounts on a phone today can be mounted tomorrow.
    void aSharedComponentNeverReachesForAShellsId()
    {
        const QString desktop = withoutComments(
            readAll(QStringLiteral(BSFCHAT_QML_DIR "/main.qml")));
        const QString mobile = withoutComments(
            readAll(QStringLiteral(BSFCHAT_QML_DIR "/mobile/MobileMain.qml")));
        QVERIFY2(!desktop.isEmpty(), "qml/main.qml not found");
        QVERIFY2(!mobile.isEmpty(), "qml/mobile/MobileMain.qml not found");

        QSet<QString> shellIds = declaredIds(desktop);
        shellIds.unite(declaredIds(mobile));
        QStringList vocabulary(shellIds.cbegin(), shellIds.cend());
        vocabulary.sort();   // QSet order is unspecified; keep output stable

        QStringList offenders;
        const QStringList shared = filesUnder(
            QStringLiteral(BSFCHAT_QML_DIR "/components"), QStringLiteral("*.qml"));
        for (const QString& path : shared) {
            const QString src = withoutComments(readAll(path));
            const QSet<QString> mine = declaredIds(src);
            const QSet<QString> members = declaredMembers(src);
            for (const QString& name : std::as_const(vocabulary)) {
                if (mine.contains(name) || members.contains(name)) continue;
                // A use, not a mention: the name at the head of a member
                // access or a call, not preceded by `.` (so `foo.loginDialog`
                // is someone else's property) or by a word character.
                const QRegularExpression use(
                    QStringLiteral(R"((?<![\w.$]))")
                    + QRegularExpression::escape(name)
                    + QStringLiteral(R"(\s*[.(])"));
                const QRegularExpressionMatch m = use.match(src);
                if (!m.hasMatch()) continue;
                offenders << QStringLiteral("%1:%2 reaches for `%3`")
                                 .arg(QFileInfo(path).fileName())
                                 .arg(lineOf(src, m.capturedStart()))
                                 .arg(name);
            }
        }
        offenders.sort();
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "shared component reaches for an id only a shell declares (QML ids "
                     "do not cross component scope, so this is a ReferenceError under the "
                     "other shell) — add a helper function to BOTH main.qml and "
                     "MobileMain.qml and call it through Window.window: ")
                     + offenders.join(QStringLiteral("; "))));
    }

    // Half two: a helper.
    //
    // The fix for half one is `Window.window.openLoginDialog()`, which is the
    // pattern the tree already uses (openUserSettings, openReportDialog, the
    // toast family). It only works if both shells implement it. `openSelfRoles`
    // did not: main.qml had it, MobileMain.qml did not, and ChannelList's
    // "Your Roles" item was the same bug wearing a different hat -- a
    // TypeError instead of a ReferenceError.
    //
    // The rule: every `Window.window.<name>` a shared component calls must be
    // declared on both shells. A shell may implement it as a deliberate no-op
    // (MobileMain's openShortcutsDialog is one); it may not simply be absent.
    //
    // WHAT IT DOES NOT CATCH, precisely:
    //   * which Window actually answers at runtime. A component instantiated
    //     inside VideoPopoutWindow / VideoFullscreenWindow sees THAT window,
    //     which implements none of these. Call sites there guard with a
    //     truthiness check (VoiceDock, MessageView already do); this rule does
    //     not verify the guard.
    //   * arity or argument types -- name presence only. A helper taking six
    //     arguments on one shell and two on the other passes.
    //   * a helper reached by any route other than the literal text
    //     `Window.window.<name>` (an alias, a stored reference).
    //   * declaredMembers() collects at any depth, so a same-named function
    //     nested inside an unrelated block on one shell would satisfy it.
    void everyShellHelperASharedComponentCallsExistsOnBothShells()
    {
        const QString desktop = withoutComments(
            readAll(QStringLiteral(BSFCHAT_QML_DIR "/main.qml")));
        const QString mobile = withoutComments(
            readAll(QStringLiteral(BSFCHAT_QML_DIR "/mobile/MobileMain.qml")));
        QVERIFY2(!desktop.isEmpty() && !mobile.isEmpty(), "shell sources not found");

        const QSet<QString> onDesktop = declaredMembers(desktop);
        const QSet<QString> onMobile = declaredMembers(mobile);

        static const QRegularExpression call(
            QStringLiteral(R"(Window\s*\.\s*window\s*\??\s*\.\s*([A-Za-z_]\w*))"));

        QStringList offenders;
        const QStringList shared = filesUnder(
            QStringLiteral(BSFCHAT_QML_DIR "/components"), QStringLiteral("*.qml"));
        for (const QString& path : shared) {
            const QString src = withoutComments(readAll(path));
            for (auto it = call.globalMatch(src); it.hasNext();) {
                const QRegularExpressionMatch m = it.next();
                const QString name = m.captured(1);
                const bool d = onDesktop.contains(name);
                const bool b = onMobile.contains(name);
                if (d && b) continue;
                offenders << QStringLiteral("%1:%2 calls Window.window.%3 (missing on %4)")
                                 .arg(QFileInfo(path).fileName())
                                 .arg(lineOf(src, m.capturedStart()))
                                 .arg(name)
                                 .arg(d ? QStringLiteral("MobileMain.qml")
                                        : b ? QStringLiteral("main.qml")
                                            : QStringLiteral("BOTH shells"));
            }
        }
        offenders.removeDuplicates();
        offenders.sort();
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "shared component calls a shell helper that one shell does not "
                     "implement (a TypeError there) — mirror it, no-op body if that is "
                     "what it means on that shell: ")
                     + offenders.join(QStringLiteral("; "))));
    }

    // A LinkPreview's `url` is a binding, and a live delegate sees it change
    // whenever its model row updates or it is recycled for another message.
    // `_fetch()` re-runs for the new URL, but every outcome property still
    // describes the old one, and `visible` gates on `_failed` with nothing
    // else ever clearing it — so one dead link hid every URL that delegate
    // was later handed. Same trick for the og* fields, which would otherwise
    // render (or, on the early-return paths, keep rendering) the previous
    // page's card under the new link.
    //
    // The guard: inside `function _fetch()`, `_failed = false` and a reset
    // of each og* field must come before the first `_failed = true` and
    // before any early `return`.
    void linkPreviewFetchStartsFromACleanSlate()
    {
        const QString src = withoutComments(readQml(QStringLiteral("/components/LinkPreview.qml")));

        const qsizetype open = src.indexOf(QRegularExpression(QStringLiteral(R"(function\s+_fetch\s*\(\s*\)\s*\{)")));
        QVERIFY2(open >= 0, "LinkPreview.qml no longer has _fetch()");
        qsizetype i = src.indexOf(QLatin1Char('{'), open);
        const qsizetype bodyStart = i + 1;
        int nesting = 0;
        for (; i < src.size(); ++i) {
            if (src.at(i) == QLatin1Char('{')) ++nesting;
            else if (src.at(i) == QLatin1Char('}') && --nesting == 0) break;
        }
        const QString body = src.mid(bodyStart, i - bodyStart);

        // The first thing that can end the function or record a failure.
        static const QRegularExpression firstExit(
            QStringLiteral(R"(\breturn\b|_failed\s*=\s*true)"));
        const qsizetype exitAt = body.indexOf(firstExit);
        QVERIFY2(exitAt >= 0, "_fetch() has no return and never fails — check the test, not the code");
        const QString prologue = body.left(exitAt);

        static const QRegularExpression clearFailed(QStringLiteral(R"(\b_failed\s*=\s*false\b)"));
        QVERIFY2(prologue.contains(clearFailed),
                 "_fetch() does not reset _failed before its first exit; a card that "
                 "failed once stays hidden for every URL it is later bound to");

        for (const char* field : {"ogTitle", "ogDescription", "ogImage", "ogSiteName"}) {
            const QRegularExpression clearField(
                QStringLiteral(R"(\b%1\s*=\s*"")").arg(QLatin1String(field)));
            QVERIFY2(prologue.contains(clearField),
                     qPrintable(QStringLiteral(
                         "_fetch() does not clear %1 before its first exit; the previous "
                         "URL's metadata renders under the new one").arg(QLatin1String(field))));
        }
    }

    // ---- Profile card (2026-09-18) -------------------------------------
    //
    // The same class of bug as LinkPreview's, in the same shape: one reused
    // popup, per-user state left over from the previous user. MemberList.qml
    // and MessageView.qml assign `userId` / `profileDisplayName` onto a single
    // UserProfileCard instance and call open() again for the next member.
    // `profileAvatarUrl` was not reset, and the reply that should have
    // overwritten it need never arrive — MatrixClient::getProfile returns
    // without emitting on any network error, and an account with neither a
    // display name nor a picture set is exactly what a homeserver may 404. So
    // clicking A then B showed B's name over A's photograph.
    //
    // The values are tested in tests/qml/tst_profilecardavatar.qml. What only
    // the source text can show is that the card still CALLS the reset when it
    // opens, and that the picture and the initial are still decided by one
    // binding rather than by two that can disagree.
    void profileCardOpensOnACleanSlate()
    {
        const QString src = withoutComments(readQml(QStringLiteral("/components/UserProfileCard.qml")));

        const QString body = blockBody(src, QStringLiteral(R"(onAboutToShow\s*:\s*\{)"));
        QVERIFY2(!body.isNull(), "UserProfileCard.qml no longer has an onAboutToShow block");

        // Before anything can leave the handler early.
        const qsizetype exitAt = body.indexOf(QRegularExpression(QStringLiteral(R"(\breturn\b)")));
        const QString prologue = exitAt >= 0 ? body.left(exitAt) : body;

        static const QRegularExpression clearAvatar(
            QStringLiteral(R"(\bprofileAvatarUrl\s*=\s*(""|ProfileCardAvatar\.avatarUrlOnOpen\s*\())"));
        QVERIFY2(prologue.contains(clearAvatar),
                 "UserProfileCard.qml does not clear profileAvatarUrl when it opens; "
                 "the previously-shown member's profile picture stays on the card for "
                 "the next one, and stays for good when their profile fetch never "
                 "answers");

        static const QRegularExpression clearNickname(QStringLiteral(R"(\bnickname\s*=\s*"")"));
        QVERIFY2(prologue.contains(clearNickname),
                 "UserProfileCard.qml does not clear nickname when it opens");

        // One source of truth for the tile: the Image's source and the
        // initial's visibility must be the same expression, or a URL that
        // resolves to nothing leaves a tile with neither in it.
        QVERIFY2(src.contains(QRegularExpression(
                     QStringLiteral(R"(source\s*:\s*profileCard\.avatarSource\b)"))),
                 "UserProfileCard.qml's avatar Image no longer sources from "
                 "profileCard.avatarSource");
        QVERIFY2(src.contains(QRegularExpression(
                     QStringLiteral(R"(visible\s*:\s*profileCard\.avatarSource\s*===\s*"")"))),
                 "UserProfileCard.qml's avatar initial is no longer shown on exactly "
                 "the condition that the Image has nothing to draw");
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

    // Joining a server by its address is the product's central act.
    //
    // BSFChat is self-hosted. "Somebody gave me an address" is not an
    // advanced case, it is THE case — and until 2026-09-24 it was a text
    // link inside the sign-in dialog, reading "Add a specific server",
    // below a divider, hidden by `property bool showManualServer: false`.
    // On the owner's own Pixel, on a fresh install, the owner of the
    // product had to be told where it was. A store reviewer handed
    // "sign in to uat.bsfchat.com with these credentials" would not have
    // found it, and that is a rejection, not a papercut.
    //
    // The property this pins: the path exists as a control of its own, on
    // both shells, one tap from the sign-in screen. It is written as four
    // checks because "reachable" has four separable ways to rot — the
    // dialog's entry point, the shells' helpers, an actual affordance that
    // calls them, and the chooser still being the screen the dialog opens
    // on.
    //
    // WHAT IT DOES NOT CATCH: whether any of it is on screen. Nothing
    // headless can measure that — a button behind a Loader that never
    // activates, or under the notch, passes. It also cannot tell a real
    // affordance from a dead one; it checks that the wiring exists, in the
    // places a human would look for it.
    void joiningAServerByAddressIsReachableOnBothShells()
    {
        const QString dialog = withoutComments(
            readQml(QStringLiteral("/components/LoginDialog.qml")));
        const QString desktop = withoutComments(readQml(QStringLiteral("/main.qml")));
        const QString mobile = withoutComments(
            readQml(QStringLiteral("/mobile/MobileMain.qml")));

        // 1. The dialog can be opened straight onto address entry.
        QVERIFY2(dialog.contains(QLatin1String("function openAtAddress(")),
                 "LoginDialog has no way to be opened on address entry — the "
                 "only route back in is the chooser, and a caller who already "
                 "knows the user has an address has to make them choose again");

        // 2. Both shells expose it under the same name. A helper on one
        //    shell only is a TypeError on the other; see
        //    everyShellHelperASharedComponentCallsExistsOnBothShells.
        struct Shell { const char* name; const QString* src; };
        const Shell shells[] = {{"main.qml", &desktop},
                                {"mobile/MobileMain.qml", &mobile}};
        for (const auto& shell : shells) {
            QVERIFY2(declaredMembers(*shell.src).contains(
                         QStringLiteral("openJoinByAddress")),
                     qPrintable(QStringLiteral("%1 does not declare openJoinByAddress()")
                                    .arg(QLatin1String(shell.name))));
        }

        // 3. Somebody actually offers it. A helper nothing calls is a
        //    helper nobody can reach, which is the state the mobile shell
        //    was already in with the address flow.
        int callers = 0;
        QStringList surfaces = filesUnder(QStringLiteral(BSFCHAT_QML_DIR "/components"),
                                          QStringLiteral("*.qml"));
        surfaces << QStringLiteral(BSFCHAT_QML_DIR "/mobile/MobileMain.qml")
                 << QStringLiteral(BSFCHAT_QML_DIR "/main.qml");
        for (const QString& path : std::as_const(surfaces)) {
            const QString src = withoutComments(readAll(path));
            // A declaration reads `function openJoinByAddress() {`, which
            // contains the call text verbatim — count only what is left
            // once the declarations are taken out, or a shell declaring the
            // helper and nobody offering it would satisfy this.
            const int uses = static_cast<int>(src.count(QLatin1String("openJoinByAddress()")))
                - static_cast<int>(src.count(QLatin1String("function openJoinByAddress()")));
            if (uses > 0) ++callers;
        }
        QVERIFY2(callers >= 2,
                 qPrintable(QStringLiteral("openJoinByAddress() is offered from %1 place(s); "
                                           "expected the shared empty state and at least one "
                                           "shell's own")
                                .arg(callers)));

        // 4. And the choice is on the screen the dialog opens on, not
        //    behind a disclosure: the dialog resets to the chooser every
        //    time it is shown, and the chooser's address button switches
        //    mode rather than expanding something.
        QVERIFY2(dialog.contains(QLatin1String("dialog.mode = \"choose\"")),
                 "LoginDialog no longer resets to the chooser when it opens");
        QVERIFY2(dialog.contains(QLatin1String("\"Join a server by address\"")),
                 "the chooser lost the words for the address path");
        const qsizetype button = dialog.indexOf(QLatin1String("id: joinByAddressButton"));
        QVERIFY2(button >= 0, "the chooser lost its address button");
        QVERIFY2(dialog.mid(button, 1600).contains(QLatin1String("dialog.mode = \"address\"")),
                 "the chooser's address button no longer leads to address entry");
    }

    // Sign-in must say who it signed in as.
    //
    // From the same device session: when the system browser already holds
    // a session for the identity provider, the OIDC round trip completes
    // without rendering anything, LoginDialog closed itself, and the
    // client was signed in as whoever that session belonged to. The owner
    // expected a demo account and was silently signed in as himself. On a
    // shared phone that is not confusing, it is wrong.
    //
    // The existing browser session is a FEATURE and this rule does not ask
    // for it to be removed — only that its result be visible and
    // reversible. So: the dialog may not close itself out of
    // identityLoginComplete, it must name the account, and it must offer a
    // way to undo.
    //
    // WHAT IT DOES NOT CATCH: "not you?" cannot end the session the system
    // browser holds — that needs `prompt=login` on the authorize request,
    // which belongs to the OIDC flow. It drops the local session and the
    // servers this sign-in added, and the dialog says the rest in words.
    // This rule pins the affordance, not the completeness of the sign-out.
    void signInNeverCompletesWithoutNamingTheAccount()
    {
        const QString dialog = withoutComments(
            readQml(QStringLiteral("/components/LoginDialog.qml")));

        const qsizetype handler =
            dialog.indexOf(QLatin1String("function onIdentityLoginComplete("));
        QVERIFY2(handler >= 0, "LoginDialog stopped listening for identityLoginComplete");
        // The body runs to the next `function ` at the same nesting; a
        // generous fixed window is enough to catch a close() put back.
        QVERIFY2(!dialog.mid(handler, 700).contains(QLatin1String("dialog.close()")),
                 "LoginDialog closes itself the moment identity sign-in completes — "
                 "with a live browser session that is the entire flow happening in "
                 "silence, as whoever the browser is signed in as");

        QVERIFY2(dialog.contains(QLatin1String("serverManager.identityAccountName"))
                     || dialog.contains(QLatin1String("serverManager.identityAccountId")),
                 "nothing in the sign-in dialog names the account that just signed in");
        QVERIFY2(dialog.contains(QLatin1String("Not you?")),
                 "the confirmation screen lost its \"not you?\" escape");
        QVERIFY2(dialog.contains(QLatin1String("serverManager.forgetIdentitySession()")),
                 "\"not you?\" no longer drops the identity session it is disowning");

        // And the C++ side it reads from. A Q_PROPERTY quietly renamed
        // leaves the QML above binding to undefined, which renders as an
        // empty string — i.e. back to saying nothing, silently.
        const QString header = withoutComments(
            readAll(QStringLiteral(BSFCHAT_SRC_DIR "/net/ServerManager.h")));
        QVERIFY2(!header.isEmpty(), "ServerManager.h not found");
        for (const char* member : {"identityAccountName", "identityAccountId",
                                   "forgetIdentitySession"}) {
            QVERIFY2(header.contains(QLatin1String(member)),
                     qPrintable(QStringLiteral("ServerManager no longer offers %1, which the "
                                               "sign-in confirmation is bound to")
                                    .arg(QLatin1String(member))));
        }
    }

    // An account that has joined nothing must not dead-end.
    //
    // D-M8 caught the first half of this: identity sign-in for an account
    // with an empty membership list closed the dialog and left the user in
    // an empty app with nothing said. The fix routed it to
    // identityLoginFailed, which the shell toasts as "Identity login
    // failed: your account isn't a member of any server yet — … or add a
    // server by URL below." Three things wrong with that. The sign-in did
    // not fail. There was no "below" on a phone, only a collapsed link.
    // And a toast is gone in four seconds.
    //
    // It now has its own signal and its own screen, which names the
    // account, says plainly what happened, and puts the one useful next
    // step — join a server by address — under the user's thumb.
    void anAccountWithNoServersIsToldWhatToDoNext()
    {
        const QString manager = withoutComments(
            readAll(QStringLiteral(BSFCHAT_SRC_DIR "/net/ServerManager.h")));
        QVERIFY2(manager.contains(QLatin1String("void identityHasNoServers()")),
                 "ServerManager no longer distinguishes \"joined nothing yet\" from a "
                 "failed sign-in, so it is back to being reported as an error");

        const QString impl = withoutComments(
            readAll(QStringLiteral(BSFCHAT_SRC_DIR "/net/ServerManager.cpp")));
        const qsizetype empty = impl.indexOf(QLatin1String("if (servers.isEmpty())"));
        QVERIFY2(empty >= 0, "the empty-membership branch is gone from the identity sync");
        QVERIFY2(impl.mid(empty, 300).contains(QLatin1String("emit identityHasNoServers()")),
                 "an empty membership list is reported on the failure channel again");

        const QString dialog = withoutComments(
            readQml(QStringLiteral("/components/LoginDialog.qml")));
        QVERIFY2(dialog.contains(QLatin1String("function onIdentityHasNoServers(")),
                 "LoginDialog does not handle identityHasNoServers, so the signal lands "
                 "nowhere and the dialog sits on \"Waiting for browser login…\" forever");
        const qsizetype screen = dialog.indexOf(QLatin1String("dialog.mode === \"noServers\""));
        QVERIFY2(screen >= 0, "there is no screen for the no-servers case");
        QVERIFY2(dialog.contains(QLatin1String("hasn't joined a BSFChat server yet")),
                 "the no-servers screen lost the sentence explaining what happened");
        // The way out has to be a control, not prose pointing at one.
        const qsizetype button = dialog.indexOf(QLatin1String("id: noServersJoinButton"));
        QVERIFY2(button >= 0,
                 "the no-servers screen has no button onto address entry — telling "
                 "someone to \"add a server by URL below\" when there is no below is "
                 "the dead end this replaced");
        QVERIFY2(dialog.mid(button, 1200).contains(QLatin1String("dialog.mode = \"address\"")),
                 "the no-servers screen's button no longer leads to address entry");
    }

    // An expired session must offer a way out of itself.
    //
    // On 2026-09-19 the homeserver's access_tokens table was purged. Clients
    // did the right thing all the way up to the last step: they recognised
    // the 401, stopped retrying, and put up a banner reading "Your session
    // has expired. Sign in again to reconnect." There was nothing to press.
    // The only affordance in the product — the server rail's Reconnect —
    // rebuilt the connection carrying the same dead token, so the server
    // logged ZERO login attempts while the user retried, and the documented
    // recovery became "remove the server and add it back".
    //
    // The state machine is pinned in test_session_auth and the dispatch in
    // ServerManager::reconnectServer. This is the half only the QML can get
    // wrong: a banner that states the problem and offers no action.
    void expiredSessionBannerOffersAWayOut()
    {
        for (const char* file : {"/components/MessageView.qml",
                                 "/components/ServerSidebar.qml"}) {
            const QString src = readQml(QLatin1String(file));
            QVERIFY2(src.contains(QLatin1String("needsReauth")),
                     qPrintable(QStringLiteral("%1 no longer notices an expired session")
                                    .arg(QLatin1String(file))));
            QVERIFY2(src.contains(QLatin1String("serverManager.reauthenticateServer(")),
                     qPrintable(QStringLiteral(
                                    "%1 shows the expired state with no way to act on it")
                                    .arg(QLatin1String(file))));
        }

        // And the rail must not keep offering the retry that cannot work:
        // the Reconnect item hides itself once the token has been rejected.
        const QString rail = readQml(QStringLiteral("/components/ServerSidebar.qml"));
        const qsizetype at = rail.indexOf(QLatin1String("serverManager.reconnectServer("));
        QVERIFY2(at >= 0, "the Reconnect item is gone — check this test, not the code");
        // Its visibility rule is the one mentioning needsReauth ABOVE it.
        const QString before = rail.left(at);
        QVERIFY2(before.lastIndexOf(QLatin1String("needsReauth")) > before.lastIndexOf(
                     QLatin1String("ServerCtxItem {")),
                 "Reconnect is still offered for a session only a login can fix");
    }

    // The voice room's video has to stay in ONE component, instantiated
    // by ONE Repeater over the feed list.
    //
    // This is the invariant behind "clicking a thumbnail must not make
    // the picture blink". A feed can be filling the main stage or sitting
    // in the bottom strip, and the obvious way to build that — a stage
    // component and a strip component, or a reparent on selection — means
    // the QML engine destroys the VideoOutput on one side and creates a
    // fresh one on the other every time the selection changes. A new
    // VideoOutput has no frames: it shows black until the next one
    // arrives, which on a 5 fps screen share is a fifth of a second of
    // nothing, on every click.
    //
    // So VideoFeedTile.qml holds the only VideoOutput, VoiceRoom holds a
    // Repeater over _feeds (whose identity changes only when the set of
    // feeds changes, never when the selection does), and moving a feed
    // between stage and strip is a write to x/y/width/height. None of
    // that is visible to a QML test — VoiceRoom imports the BSFChat
    // module, which only the app binary has — so it is pinned here.
    void voiceRoomVideoLivesInOneRepeatedComponent()
    {
        static const QRegularExpression videoOutput(
            QStringLiteral(R"(\bVideoOutput\s*\{)"));

        // Nothing else in the voice room may declare one. ParticipantTile
        // is named explicitly because it used to: a camera rendered into
        // the 220x180 avatar tile, which is the complaint this layout
        // answers, and the tile is not even on screen while a feed exists.
        for (const QString& relative : {QStringLiteral("/components/VoiceRoom.qml"),
                                        QStringLiteral("/components/ParticipantTile.qml")}) {
            const QString src = withoutComments(readQml(relative));
            QVERIFY2(!videoOutput.match(src).hasMatch(),
                     qPrintable(relative + QStringLiteral(
                         " declares a VideoOutput; voice-room video belongs in"
                         " VideoFeedTile.qml so selection cannot destroy it")));
        }

        const QString tile = withoutComments(
            readQml(QStringLiteral("/components/VideoFeedTile.qml")));
        int outputs = 0;
        for (auto it = videoOutput.globalMatch(tile); it.hasNext();) {
            it.next();
            ++outputs;
        }
        QCOMPARE(outputs, 1);
        // Attached once, on creation. Re-attaching on a property change
        // would stack duplicate outputs on the same per-peer sink.
        QVERIFY2(tile.contains(QLatin1String("Component.onCompleted")),
                 "VideoFeedTile no longer attaches its sink on creation");

        // And the tiles are produced by a Repeater over the feed list,
        // not by a stage instance plus strip instances.
        const QString room = withoutComments(
            readQml(QStringLiteral("/components/VoiceRoom.qml")));
        static const QRegularExpression feedTile(
            QStringLiteral(R"(\bVideoFeedTile\s*\{)"));
        int tiles = 0;
        for (auto it = feedTile.globalMatch(room); it.hasNext();) {
            it.next();
            ++tiles;
        }
        QCOMPARE(tiles, 1);
        QVERIFY2(room.contains(QLatin1String("model: room._feeds")),
                 "VoiceRoom no longer repeats its video tiles over _feeds");
    }

    // "Full screen" must mean the VIDEO, not the app window.
    //
    // The button in the voice-room header used to do
    //     Window.window.visibility = Window.FullScreen
    // which put the whole APP full screen — sidebar, channel list, member
    // strip, dock and all — with the video still in its little stage
    // panel in the middle of it. That is the thing the owner asked to
    // have changed, and it is a one-line regression away at all times:
    // `Window.window` is in scope in every one of these files and the
    // wrong version is shorter to write than the right one.
    //
    // So: none of the voice-room video files may write ANY window's
    // visibility. The fullscreen path is a dedicated top-level Window
    // whose own `visibility:` is a declarative binding on itself, which
    // is the shape this allows — an assignment with a dot before it is
    // reaching into somebody else's window and is what fails here.
    //
    // VideoPlayerCard.qml is deliberately NOT in this list: the inline
    // message-timeline player really does full-screen the app window,
    // that is a different feature, and it has its own rules in
    // qml/js/PlaybackMath.js.
    void fullscreenVideoNeverTouchesAnotherWindowsVisibility()
    {
        static const QRegularExpression foreignVisibility(
            QStringLiteral(R"(\.\s*visibility\s*=[^=])"));
        for (const QString& relative : {
                 QStringLiteral("/components/VoiceRoom.qml"),
                 QStringLiteral("/components/VideoFeedTile.qml"),
                 QStringLiteral("/components/VideoFullscreenWindow.qml"),
                 QStringLiteral("/components/VideoPopoutWindow.qml")}) {
            const QString src = withoutComments(readQml(relative));
            QVERIFY2(!foreignVisibility.match(src).hasMatch(),
                     qPrintable(relative + QStringLiteral(
                         " assigns another window's visibility; video"
                         " fullscreen is its own Window, and the app"
                         " window must not change state")));
        }

        // And the fullscreen window really is one: frameless, full
        // screen, and NOT a transient child. Each of those three is load
        // bearing on macOS — see the header of the file. Without the
        // frameless hint Qt takes the NATIVE fullscreen path, which puts
        // the window in a Space of its own and can strand an empty one
        // in Mission Control when it is destroyed.
        const QString fs = withoutComments(
            readQml(QStringLiteral("/components/VideoFullscreenWindow.qml")));
        QVERIFY2(fs.contains(QLatin1String("Qt.FramelessWindowHint")),
                 "the fullscreen window is no longer frameless: Qt would"
                 " take the native macOS fullscreen path and give it its"
                 " own Space");
        QVERIFY2(!fs.contains(QLatin1String("WindowFullscreenButtonHint")),
                 "WindowFullscreenButtonHint is exactly what puts a macOS"
                 " window into its own Space; do not add it");
        QVERIFY2(fs.contains(QLatin1String("visibility: Window.FullScreen")),
                 "the fullscreen window no longer declares itself full screen");
        QVERIFY2(fs.contains(QLatin1String("transientParent: null")),
                 "a transient child is ordered with its parent on macOS and"
                 " cannot own the screen");

        // The pop-out is an ordinary window, so it must NOT be frameless
        // (there would be no title bar to move or close it by) and must
        // not be a transient child (it would float above the app window
        // forever, which is what the pin toggle is for).
        const QString po = withoutComments(
            readQml(QStringLiteral("/components/VideoPopoutWindow.qml")));
        QVERIFY2(!po.contains(QLatin1String("FramelessWindowHint")),
                 "a frameless pop-out has no title bar to drag or close");
        QVERIFY2(po.contains(QLatin1String("transientParent: null")),
                 "a transient pop-out is always above the app window and"
                 " follows it between Spaces");
    }

    // A popped-out feed's in-room tile has to STAY LIVE, which is only
    // possible because one stream feeds several sinks
    // (tests/test_video_registry.cpp). The way that gets broken is by
    // "fixing" the pop-out to take the tile's surface — detaching the
    // tile, or hiding it, or dropping it out of the feed list while a
    // window is open. Nothing in the voice room may do any of that.
    void poppingAFeedOutDoesNotDisturbItsTile()
    {
        const QString room = withoutComments(
            readQml(QStringLiteral("/components/VoiceRoom.qml")));
        // The feed list is built from the roster and the controllers, not
        // filtered by what is popped out.
        QVERIFY2(!room.contains(QLatin1String("detachOutput")),
                 "VoiceRoom detaches a sink; the tile must keep its own");
        QVERIFY2(room.contains(QLatin1String("model: room._popouts")),
                 "pop-out windows are no longer instantiated from the"
                 " tested VideoWindows state");

        const QString tile = withoutComments(
            readQml(QStringLiteral("/components/VideoFeedTile.qml")));
        // `poppedOut` may drive a badge and the button label. It may not
        // drive the VideoOutput's existence or visibility, which would
        // black out the tile behind the window.
        for (const QString& binding : visibleBindings(tile)) {
            QVERIFY2(!binding.contains(QLatin1String("poppedOut"))
                         || binding.contains(QLatin1String("tile.poppedOut &&")),
                     qPrintable(QStringLiteral(
                         "a tile element is hidden while popped out, which"
                         " is how the live tile gets lost: ") + binding));
        }
    }

    // The rc.6 Theme.onScrim trap, checked everywhere rather than only in
    // Theme.qml: QML parses any `on` + Capital identifier as a signal
    // handler, so `property bool onStage: false` is not a property at all,
    // it is an assignment to a handler for a signal named `stage` — a load
    // error that makes the component unavailable and takes every file
    // importing it down too. Theme.qml was where it happened to bite; the
    // parser rule is global, and a component property is exactly as easy
    // to name that way as a colour token.
    void noQmlFileDeclaresASignalHandlerShapedProperty()
    {
        static const QRegularExpression decl(
            QStringLiteral(R"(^\s*(?:readonly\s+)?property\s+\w+\s+(on[A-Z]\w*)\s*:)"),
            QRegularExpression::MultilineOption);
        QStringList offenders;
        for (const QString& path : filesUnder(QStringLiteral(BSFCHAT_QML_DIR),
                                              QStringLiteral("*.qml"))) {
            const QString src = withoutComments(readAll(path));
            for (auto it = decl.globalMatch(src); it.hasNext();) {
                const QString name = it.next().captured(1);
                // Grandfathered: its value is a ternary, which the parser
                // accepts as a handler body. Do not add more.
                if (name == QLatin1String("onAccent")) continue;
                offenders << QFileInfo(path).fileName() + QLatin1Char(':') + name;
            }
        }
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral("signal-handler-shaped properties (rename them): ")
                            + offenders.join(QStringLiteral(", "))));
    }

    // Starting a composer upload without telling the composer.
    //
    // MessageInput.qml derives its disabled "Uploading…" state from a count of
    // the uploads it started, and decrements that count on the connection's
    // mediaSendCompleted / mediaSendFailed. Those are bare signals — they say
    // an upload ended and nothing about which one — so ANY call to
    // sendMediaMessage on the active connection hands the composer a
    // decrement, whether or not that caller ever handed it an increment.
    //
    // Three bugs on this counter have shipped. Two were fixed in
    // ServerConnection (the pre-flight emits are deferred; the avatar uploads
    // got a signal of their own). The third is the one this scan is for:
    // MobileMain.qml's Android share-intent handler called sendMediaMessage
    // and simply did not count it. Sharing a file into BSFChat while an
    // attachment was uploading unlocked the composer with the attachment still
    // on the wire; sharing with nothing in flight was an unmatched decrement
    // that the clamp in _noteUploadFinished absorbed without a word. The
    // ServerConnection comment predicting exactly this ("reordering the QML
    // fixes today's three sites and leaves the trap armed for the fourth") was
    // written while the fourth site was already in the tree.
    //
    // A convention did not hold for three call sites and will not hold for the
    // fifth, so it is a scan. Deliberately coarse: it pairs the two calls
    // per FILE and by count, not by control flow. It cannot tell a loop that
    // sends N and counts N from one that sends N and counts one — the C++
    // cases in test_composer_upload_lock.cpp are where multi-file balance is
    // pinned. What it does catch, and what nothing else catches headlessly, is
    // a call site that does not participate in the bookkeeping at all.
    void everyQmlUploadIsCounted()
    {
        // Calls, not declarations: `function noteUploadStarted()` is the
        // definition in MessageInput.qml and the forwarder in MessageView.qml,
        // and neither is a site that starts anything.
        static const QRegularExpression sends(QStringLiteral(R"(\bsendMediaMessage\s*\()"));
        static const QRegularExpression counts(
            QStringLiteral(R"((?<!function\s)\bnoteUploadStarted\s*\()"));

        QStringList offenders;
        int sitesChecked = 0;
        for (const QString& path : filesUnder(QStringLiteral(BSFCHAT_QML_DIR),
                                              QStringLiteral("*.qml"))) {
            const QString src = withoutComments(readAll(path));
            int nSends = 0;
            for (auto it = sends.globalMatch(src); it.hasNext(); it.next()) ++nSends;
            if (nSends == 0) continue;
            sitesChecked += nSends;

            int nCounts = 0;
            for (auto it = counts.globalMatch(src); it.hasNext(); it.next()) ++nCounts;
            if (nCounts < nSends) {
                offenders << QStringLiteral("%1 (%2 sendMediaMessage, %3 noteUploadStarted)")
                                 .arg(QFileInfo(path).fileName())
                                 .arg(nSends).arg(nCounts);
            }
        }
        QVERIFY2(sitesChecked > 0,
                 "no QML calls sendMediaMessage — has the upload path moved? "
                 "This guard is only meaningful while call sites exist.");
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "QML starts a composer upload without calling "
                     "noteUploadStarted, so the composer will be handed a "
                     "decrement it never matched: ")
                     + offenders.join(QStringLiteral(", "))));
    }

    // ThemedComboBox must not read a label straight off ComboBox.
    //
    // Channel settings > Role overrides shipped a dropdown of exactly the
    // right height with six blank rows and a blank closed field. Its model is
    // ServerConnection's `Q_PROPERTY(QJsonArray serverRoles ...)`, and
    // ComboBox resolves both textAt() and displayText through a textRole
    // lookup that reads a QVariantMap entry or a QAbstractListModel role and
    // returns "" for every row of a QJsonArray. The count comes through,
    // which is what makes it look like a colour bug.
    //
    // The trap worth spelling out: the code being guarded against here is not
    // a careless shortcut, it is the FIX for the previous instance of the same
    // symptom. ThemedComboBox once resolved labels with
    // `Array.isArray(cb.model) ? cb.model[i][textRole] : ""`, which blanked
    // the QVariantList-backed audio device combos in ClientSettings.qml
    // because Array.isArray is false for those — and true for exactly the
    // QJsonArray case textAt cannot read. Each single-source version blanks
    // the combos the other one serves. qml/js/ComboBoxText.js tries both, and
    // this guard is what stops a future edit from "simplifying" it back to
    // either half.
    //
    // Source scan, same constraint as the rest of this file: ThemedComboBox
    // imports the BSFChat module, so no test binary can instantiate it. The
    // resolver's own behaviour is covered by test_combobox_model, which
    // drives it against a real QJsonArray Q_PROPERTY.
    void themedComboBoxResolvesLabelsThroughTheSharedHelper()
    {
        const QString src = withoutComments(
            readQml(QStringLiteral("/components/ThemedComboBox.qml")));
        QVERIFY2(!src.isEmpty(), "ThemedComboBox.qml not found");

        QVERIFY2(src.contains(QStringLiteral("ComboBoxText.js")),
                 "ThemedComboBox.qml no longer imports ComboBoxText.js — a "
                 "bare textAt()/displayText goes blank on a QJsonArray model "
                 "(Role overrides), and a bare Array.isArray path goes blank "
                 "on a QVariantList one (audio devices)");

        // Every use of the two ComboBox label accessors has to be an argument
        // to the resolver, never the value of a binding on its own.
        static const QRegularExpression bare(
            QStringLiteral(R"RX((?<!ComboBoxText\.resolve\()(?:cb\.textAt\s*\(|cb\.displayText\b))RX"));
        QStringList offenders;
        for (auto it = bare.globalMatch(src); it.hasNext();)
            offenders << it.next().captured(0).trimmed();
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "ThemedComboBox.qml reads a label straight off ComboBox "
                     "instead of through ComboBoxText.resolve(): ")
                     + offenders.join(QStringLiteral(", "))));
    }

    // ──────────── the phone shell ────────────
    //
    // Everything below is a source scan for the same reason the scans above
    // are: the BSFChat QML module is compiled into the app binary, and the
    // one platform these rules are about is the one no test target can
    // instantiate. What they replace is "someone remembered to check on a
    // phone", which is exactly what did not happen for any of them.

    void theMobileShellReadsRealSafeAreaMargins()
    {
        // topInset was a hardcoded 0 with a comment explaining that Android
        // reserves the status bar for us. True on Android; on iOS the 48pt
        // header therefore drew under the notch / Dynamic Island, taking the
        // channel name and all three header buttons with it. Qt 6.9 gives us
        // the real numbers through the SafeArea attached property.
        const QString src = withoutComments(readQml(QStringLiteral("/mobile/MobileMain.qml")));

        QVERIFY2(src.contains(QStringLiteral("SafeArea.margins")),
                 "MobileMain does not read SafeArea.margins; the insets are "
                 "guesses again");

        // A literal inset is the bug, whatever number it is: it cannot be
        // right on both a notched phone and a flat one.
        static const QRegularExpression literalInset(
            QStringLiteral(R"(property\s+int\s+(top|bottom|left|right)Inset\s*:\s*-?\d+\s*$)"),
            QRegularExpression::MultilineOption);
        const auto m = literalInset.match(src);
        QVERIFY2(!m.hasMatch(),
                 qPrintable(QStringLiteral("hardcoded safe-area inset: %1")
                                .arg(m.captured(0).trimmed())));

        // The header has to consume the top inset, or reading it changes
        // nothing.
        QVERIFY2(src.contains(QStringLiteral("48 + root.topInset")),
                 "the mobile header does not reserve the top safe-area inset");
    }

    void theSoftwareKeyboardActuallyMovesSomething()
    {
        // keyboardHeight was computed and then read by nothing: the comment
        // promised "a Binding below" that did not exist, and the real
        // strategy was Android's adjustResize, which has no iOS equivalent.
        // On iOS the keyboard is simply drawn over the composer — in a chat
        // app, the first thing a reviewer does.
        const QString src = withoutComments(readQml(QStringLiteral("/mobile/MobileMain.qml")));

        QVERIFY2(src.contains(QStringLiteral("Qt.inputMethod.keyboardRectangle")),
                 "MobileMain no longer measures the keyboard");

        // Two consumers, and they are the two that matter: the content area
        // (the message composer and the thread composer live in it) and the
        // surface the modal dialogs position against (the sign-in dialog's
        // password field lives in that one).
        static const QRegularExpression consumer(
            QStringLiteral(R"(anchors\.bottomMargin\s*:\s*root\.bottomGap)"));
        int consumers = 0;
        for (auto it = consumer.globalMatch(src); it.hasNext(); it.next()) ++consumers;
        QCOMPARE(consumers, 2);

        // Android resizes the window for us (windowSoftInputMode=adjustResize);
        // adding our own push on top of that would lift the composer a whole
        // keyboard's height above the keyboard.
        QVERIFY2(src.contains(QStringLiteral("keyboardPush")),
                 "no platform gate on the manual keyboard push");
    }

    void theKeyboardPushNetsOffWhatEverythingElseAlreadyDid()
    {
        // The push is what is LEFT after the platform has had its way, and
        // both ways it can have it have to be netted off:
        //
        //   windowShrink    the window got shorter (Android's adjustResize,
        //                   or MobileKeyboard doing the same by hand on iOS)
        //   platformScroll  QIOSInputContext translated the whole scene up
        //
        // Missing the second one is what put the composer a whole keyboard
        // height in the air over a void on the device. Missing the first
        // would do exactly the same thing again the moment the window
        // resize works.
        const QString src = withoutComments(readQml(QStringLiteral("/mobile/MobileMain.qml")));

        for (const QString& term : {QStringLiteral("platformScroll"),
                                    QStringLiteral("windowShrink")}) {
            const QRegularExpression push(
                QStringLiteral(R"(property\s+int\s+keyboardPush\s*:(?:[^\n]*\n){0,8}?[^\n]*%1)")
                    .arg(term));
            QVERIFY2(push.match(src).hasMatch(),
                     qPrintable(QStringLiteral("keyboardPush does not subtract "
                                               "mobileKeyboard.%1").arg(term)));
        }

        // Every term is measured, so the expression needs no per-platform
        // branch — and must not grow one back, because a branch is an
        // assumption about what the platform did rather than a reading of it.
        static const QRegularExpression branched(
            QStringLiteral(R"(property\s+int\s+keyboardPush\s*:(?:[^\n]*\n){0,8}?[^\n]*Qt\.platform)"));
        QVERIFY2(!branched.match(src).hasMatch(),
                 "keyboardPush branches on the platform again; the three terms "
                 "are measurements and cover every platform between them");

        // Netting off is only half of it: something has to ask the platform
        // to recompute, or it keeps the scroll it decided on before our
        // layout existed.
        QVERIFY2(src.contains(QStringLiteral("mobileKeyboard.settle(")),
                 "nothing asks the platform to recompute its own scroll");
    }

    void theKeyboardBridgeExposesBothMeasurements()
    {
        // MobileMain binds to both on every layout pass; a rename that only
        // touched the C++ would be a silent ReferenceError in the hottest
        // binding in the shell, and the symptom would be the composer under
        // the keyboard rather than anything that looks like a typo.
        QFile f(QStringLiteral(BSFCHAT_SRC_DIR "/core/MobileKeyboard.h"));
        QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), "MobileKeyboard.h not found");
        const QString src = QString::fromUtf8(f.readAll());
        QVERIFY2(src.contains(QStringLiteral("Q_PROPERTY(int platformScroll")),
                 "platformScroll is not a QML property any more");
        QVERIFY2(src.contains(QStringLiteral("Q_PROPERTY(int windowShrink")),
                 "windowShrink is not a QML property any more");
    }

    void theKeyboardGapIsNeverAnimated()
    {
        // An eased gap is actively harmful, not merely slower: the iOS
        // plugin recomputes its scroll from where the cursor is at the
        // instant it is asked, so a gap still travelling reads as a gap
        // that is not there, and it scrolls the scene to make room that
        // was about to appear anyway. Android's window resize has never
        // been animated either.
        const QString src = withoutComments(readQml(QStringLiteral("/mobile/MobileMain.qml")));
        static const QRegularExpression animated(
            QStringLiteral(R"(Behavior\s+on\s+(bottomGap|anchors\.bottomMargin|keyboardPush))"));
        const auto m = animated.match(src);
        QVERIFY2(!m.hasMatch(),
                 qPrintable(QStringLiteral("the keyboard gap is animated (%1); the "
                                           "platform measures it mid-flight")
                                .arg(m.captured(0))));
    }

    void theMobileKeyboardBridgeIsReachableFromQml()
    {
        // MobileMain binds to `mobileKeyboard.platformScroll` on every
        // layout pass. A context property that main.cpp forgot to set is
        // a ReferenceError in the hottest binding in the shell.
        QFile f(QStringLiteral(BSFCHAT_SRC_DIR "/main.cpp"));
        QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), "main.cpp not found");
        const QString src = QString::fromUtf8(f.readAll());
        QVERIFY2(src.contains(QStringLiteral("setContextProperty(\"mobileKeyboard\"")),
                 "main.cpp never exposes the mobileKeyboard bridge to QML");
    }

    void everyLongPressSurfaceOnAPhoneHasALongPress()
    {
        // A context menu opened only by Qt.RightButton is a context menu that
        // does not exist on a phone. MemberList set the precedent (block and
        // report are in its menu, and those are store gates); these are the
        // rest of the menus MobileMain puts on screen.
        struct Surface { const char* file; const char* what; };
        const Surface surfaces[] = {
            {"/components/MemberList.qml",    "block and report a member"},
            {"/components/MessageBubble.qml", "reply, edit, delete, report a message"},
            {"/components/ChannelList.qml",   "mute, mark read, delete a channel"},
            {"/components/ServerSidebar.qml", "edit or remove a server"},
        };
        for (const auto& s : surfaces) {
            const QString src = withoutComments(readQml(QString::fromUtf8(s.file)));
            QVERIFY2(!src.isEmpty(), s.file);
            QVERIFY2(src.contains(QStringLiteral("Qt.RightButton")),
                     qPrintable(QStringLiteral("%1 no longer has the right-click "
                                               "menu this rule is about")
                                    .arg(QString::fromUtf8(s.file))));
            QVERIFY2(src.contains(QStringLiteral("onPressAndHold"))
                         || src.contains(QStringLiteral("onLongPressed")),
                     qPrintable(QStringLiteral("no long-press path in %1, so "
                                               "touch cannot reach: %2")
                                    .arg(QString::fromUtf8(s.file),
                                         QString::fromUtf8(s.what))));
        }
    }

    void aMobileSizeBranchIsNeverBelowTheTouchMinimum()
    {
        // `Theme.isMobile ? N : M` on a width or a height is, by definition,
        // somebody sizing a touch target. Apple's minimum is 44pt and it is
        // the number a reviewer measures, so N below 44 is a defect even
        // though it looks like a considered choice.
        // Anchored at the start of a line so `border.width: Theme.isMobile
        // ? 2 : 1` — a hairline, not a target — is not read as a 2pt button.
        static const QRegularExpression branch(
            QStringLiteral(R"(^\s*(?:Layout\.(?:preferred|minimum)(?:Width|Height)|implicit(?:Width|Height)|width|height)\s*:\s*Theme\.isMobile\s*\?\s*(\d+))"),
            QRegularExpression::MultilineOption);
        QStringList offenders;
        for (const QString& path : filesUnder(QStringLiteral(BSFCHAT_QML_DIR),
                                              QStringLiteral("*.qml"))) {
            const QString src = withoutComments(readAll(path));
            for (auto it = branch.globalMatch(src); it.hasNext();) {
                const auto m = it.next();
                if (m.captured(1).toInt() < 44) {
                    offenders << QStringLiteral("%1: %2")
                                     .arg(QFileInfo(path).fileName(), m.captured(0));
                }
            }
        }
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral("touch targets under 44pt:\n  ")
                                + offenders.join(QStringLiteral("\n  "))));
    }

    void revealOnHoverControlsThatMatterHaveATouchPath()
    {
        // A control at opacity 0 until hovered is, on a touch screen, a
        // control that is not there — the tap still works, but nothing ever
        // tells the user it exists. Each of these was the ONLY way to do the
        // thing it does, on a surface MobileMain actually shows.
        struct Gate { const char* file; const char* marker; const char* what; };
        const Gate gates[] = {
            {"/components/MessageView.qml",    "pinRowHover.containsMouse",
             "unpin a pinned message"},
            {"/components/UserSettings.qml",   "avatarMouse.containsMouse",
             "change your avatar"},
            {"/components/ChannelList.qml",    "catHeaderMouse.containsMouse",
             "add a channel to a category"},
            {"/components/ServerSettings.qml", "chSettingsMouse.containsMouse",
             "edit or delete a channel"},
            {"/components/ServerSettings.qml", "catHeaderHover.containsMouse",
             "add a channel from server settings"},
        };
        for (const auto& g : gates) {
            const QString src = withoutComments(readQml(QString::fromUtf8(g.file)));
            const QStringList bindings = visibleBindings(src) + opacityBindings(src);
            bool sawIt = false;
            for (const QString& b : bindings) {
                if (!b.contains(QString::fromUtf8(g.marker))) continue;
                sawIt = true;
                QVERIFY2(b.contains(QStringLiteral("Theme.isMobile")),
                         qPrintable(QStringLiteral("%1: reveal-on-hover with no "
                                                   "touch path, so a phone cannot "
                                                   "%2\n    %3")
                                        .arg(QString::fromUtf8(g.file),
                                             QString::fromUtf8(g.what), b)));
            }
            QVERIFY2(sawIt,
                     qPrintable(QStringLiteral("%1: no binding mentions %2 any "
                                               "more — retarget this rule rather "
                                               "than deleting it")
                                    .arg(QString::fromUtf8(g.file),
                                         QString::fromUtf8(g.marker))));
        }
    }

    void aServerWithNoChannelsSaysSoRatherThanPointingAtAnEmptyList()
    {
        // A brand-new BSFChat server used to hand its first user an empty
        // shell, and both shells then told them to go and pick a channel out
        // of a list that had none in it: the desktop timeline said "Pick a
        // channel / Choose one from the sidebar" and the phone said "Tap the
        // menu button to pick a server and channel". Advice that cannot be
        // followed is worse than no advice, because the reader spends the next
        // minute assuming they have missed something.
        //
        // The server now creates #general and a voice channel on a first run,
        // so this state should be rare — but "rare" is exactly when nobody
        // looks, and an admin who deletes every channel, or a member who can
        // see none of them, still lands here.
        //
        // Source scan, not behaviour: the rule is that BOTH shells distinguish
        // the two situations and that the phone's version offers a way
        // forward. What that way forward does when tapped is not something a
        // scan can know.
        const QString js = withoutComments(
            readQml(QStringLiteral("/js/TimelineOverlay.js")));
        QVERIFY2(js.contains(QStringLiteral("no-channels")),
                 "TimelineOverlay no longer distinguishes 'no channel selected'"
                 " from 'this server has none'");
        QVERIFY2(js.contains(QStringLiteral("channelCount")),
                 "emptyStateKind cannot tell the two apart without being told"
                 " how many channels there are");

        // The desktop timeline has to actually ASK the question. The kind
        // exists in the library either way; a caller that never passes a count
        // gets the old answer forever, which is how this rule fails silently.
        const QString view = withoutComments(
            readQml(QStringLiteral("/components/MessageView.qml")));
        QVERIFY2(view.contains(QStringLiteral("_channelCount")),
                 "MessageView never counts the server's channels, so the"
                 " desktop timeline can never reach the no-channels state");
        QVERIFY2(view.contains(QStringLiteral("emptyStateKind(")) &&
                     view.contains(QStringLiteral("_channelCount)")),
                 "MessageView counts channels but does not pass the count to"
                 " emptyStateKind");

        // Neither shell may make the claim before it has the answer. An empty
        // room list is ambiguous until the first sync lands, and reporting it
        // as zero would put "No channels yet" on screen for a round trip on
        // every cold start — a common wrong message traded for a rare one.
        // This is the arm most likely to be lost to a later simplification,
        // because the code reads perfectly well without it.
        QVERIFY2(view.contains(QStringLiteral("initialSyncComplete")),
                 "MessageView reports a channel count before the first sync has"
                 " landed, so an unsynced server reads as an empty one");

        // The phone shell draws its own empty state rather than the
        // timeline's — MessageView is a StackLayout page and is not the
        // current one when no room is selected — so the same distinction has
        // to be made a second time, over there.
        const QString mobile = withoutComments(
            readQml(QStringLiteral("/mobile/MobileMain.qml")));
        QVERIFY2(mobile.contains(QStringLiteral("_noChannels")),
                 "the phone shell still shows one 'no channel selected' state"
                 " for both situations");
        QVERIFY2(mobile.contains(QStringLiteral("No channels yet")),
                 "the phone shell never says the server has no channels");
        QVERIFY2(mobile.contains(QStringLiteral("initialSyncComplete")),
                 "the phone shell decides the server has no channels without"
                 " waiting for the first sync");

        // And it offers a way out. Being told there are no channels and given
        // nothing to press is the same dead end as before, one sentence better
        // informed — and on a phone the channel list, and the "+" that creates
        // one, is behind a drawer the reader has no reason to open.
        QVERIFY2(mobile.contains(QStringLiteral("leftDrawer.open()")),
                 "nothing on the phone's empty state opens the channel list");
        const int btn = mobile.indexOf(QStringLiteral("visible: !_noServers && _noChannels"));
        QVERIFY2(btn > 0,
                 "the empty state's button is no longer shown when the server"
                 " has no channels — retarget this rule rather than deleting it");

        // The button lives on the empty-state PAGE of the main column, which
        // is the structure fix/mobile-voice-overlays put in place: three
        // mutually exclusive StackLayout pages, none of which may carry its
        // own `visible:`. A control added here as another floating overlay
        // instead would be the exact regression that rule exists to stop, so
        // say so here too rather than leaving it to a reader to notice.
        QVERIFY2(mobile.contains(QStringLiteral("emptyOpenChannelsCta")),
                 "the empty state's channel-list button is gone");
        QVERIFY2(mobile.contains(QStringLiteral("MainSurface.mainPage(")),
                 "the mobile empty state is no longer a page of the main"
                 " column — see theMobileMainColumnHasExactlyOneWriter()");
    }

    void thePinnedListHasAWayInOnAPhone()
    {
        // The pin button lives in MessageView's chat header, and that whole
        // header is `visible: !Theme.isMobile` — so the pinned list, and the
        // unpin control that is the previous rule's whole subject, had no
        // entry point on a phone at all.
        const QString view = withoutComments(readQml(QStringLiteral("/components/MessageView.qml")));
        const QString mobile = withoutComments(readQml(QStringLiteral("/mobile/MobileMain.qml")));
        QVERIFY2(view.contains(QStringLiteral("function openPinnedMessages")),
                 "MessageView exposes no way for the shell to open the pinned list");
        QVERIFY2(mobile.contains(QStringLiteral("openPinnedMessages()")),
                 "the mobile shell never opens the pinned list");
    }

    void hapticsAreNotAndroidOnly()
    {
        // Haptics.cpp was #ifdef Q_OS_ANDROID throughout, so every
        // haptics.longPress() the QML fires was a no-op on iOS — the one
        // platform whose users read the absence of a tap as "the long press
        // did not register" and try again.
        QFile f(QStringLiteral(BSFCHAT_SRC_DIR "/core/Haptics.cpp"));
        QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), "Haptics.cpp not found");
        const QString src = QString::fromUtf8(f.readAll());
        QVERIFY2(src.contains(QStringLiteral("Q_OS_IOS")),
                 "Haptics has no iOS branch; long-press feedback is silent there");
        QVERIFY2(QFile::exists(QStringLiteral(BSFCHAT_SRC_DIR "/core/HapticsIos.mm")),
                 "HapticsIos.mm is missing");
    }



    // ── Phone form factor ────────────────────────────────────────────────
    //
    // The five rules below all came out of one round of looking at this app
    // on an actual phone. Each is a source scan for the same reason the rest
    // of this file is: the QML module is compiled into the app binary, so no
    // test target can instantiate any of it.

    // The app ships portrait-only, and the two places that say so agree.
    //
    // Both platforms used to rotate — iOS listed LandscapeLeft/Right and
    // Android's activity was screenOrientation="unspecified" — into a layout
    // that does not exist. There is no orientation-aware binding anywhere in
    // qml/ (the second half of this test), every phone surface is a vertical
    // stack sized for a portrait viewport, and on a landscape phone with the
    // keyboard up the message list comes out under a hundred points tall.
    //
    // This guard is not "landscape is forbidden forever". It is "the day
    // landscape comes back, it comes back WITH a layout" — the second half
    // fails the moment somebody writes one, which is the signal to revisit
    // the first half rather than a reason to work around it.
    void thePhoneShipsPortraitOnly()
    {
        const QString plist = readAll(QStringLiteral(BSFCHAT_ROOT_DIR
                                                     "/ios/Info.plist.in"));
        QVERIFY2(!plist.isEmpty(), "ios/Info.plist.in not found");
        QVERIFY2(plist.contains(QStringLiteral("UISupportedInterfaceOrientations")),
                 "Info.plist.in no longer declares UISupportedInterfaceOrientations "
                 "— without it iOS allows every orientation the device supports");
        // The <string> entries only. The rationale comment above them says
        // the word "landscape" a dozen times and must be allowed to.
        static const QRegularExpression landscapeEntry(
            QStringLiteral(R"(<string>\s*UIInterfaceOrientationLandscape\w*\s*</string>)"));
        QVERIFY2(!landscapeEntry.match(plist).hasMatch(),
                 "ios/Info.plist.in lists a landscape orientation again. Nothing "
                 "in qml/ lays out for it; see the comment on that key.");

        const QString manifest = readAll(QStringLiteral(BSFCHAT_ROOT_DIR
                                                        "/android/AndroidManifest.xml"));
        QVERIFY2(!manifest.isEmpty(), "android/AndroidManifest.xml not found");
        QVERIFY2(manifest.contains(QStringLiteral("android:screenOrientation=\x22portrait\x22")),
                 "the Android activity is not locked to portrait — iOS and "
                 "Android must make the same choice here or the two builds "
                 "disagree about what has been laid out");

        // And the reason the lock is right: nothing reads the orientation.
        static const QRegularExpression orientationAware(
            QStringLiteral(R"(Screen\.orientation|primaryOrientation|Qt\.LandscapeOrientation)"));
        QStringList aware;
        for (const QString& path : filesUnder(QStringLiteral(BSFCHAT_QML_DIR),
                                              QStringLiteral("*.qml"))) {
            if (orientationAware.match(withoutComments(readAll(path))).hasMatch())
                aware << QFileInfo(path).fileName();
        }
        QVERIFY2(aware.isEmpty(),
                 qPrintable(QStringLiteral(
                     "QML now reacts to device orientation (%1) — if a landscape "
                     "layout exists, the two manifests above should stop "
                     "refusing to rotate into it")
                     .arg(aware.join(QStringLiteral(", ")))));
    }

    // One horizontal gutter for every full-width surface on a phone.
    //
    // There were three. The shell header kept 10 px clear, the timeline 16
    // and the composer 8 — so the composer's rounded border was the outermost
    // thing on the screen, and at 8 px in, with the software keyboard up and
    // its bottom margin correspondingly small, the bottom-right of that
    // border falls inside the display's own ~55 pt corner radius and is cut
    // off. That is a clipping bug no safe-area inset describes: iOS reports
    // 0/0/0/0 horizontally in portrait even on a Dynamic Island device,
    // because the window IS inside the safe area. Theme.mobileGutter carries
    // the arithmetic.
    void onePhoneGutterForEveryFullWidthSurface()
    {
        const QString theme = withoutComments(
            readQml(QStringLiteral("/theme/Theme.qml")));
        QVERIFY2(theme.contains(QStringLiteral("property int mobileGutter")),
                 "Theme.mobileGutter is gone — the three surfaces below have "
                 "nothing left to agree on");

        struct Surface { const char* file; const char* what; };
        const Surface surfaces[] = {
            {"/mobile/MobileMain.qml",       "the shell header row"},
            {"/components/MessageView.qml",  "the timeline and the composer"},
        };
        for (const auto& s : surfaces) {
            const QString src = withoutComments(readQml(QString::fromUtf8(s.file)));
            QVERIFY2(src.contains(QStringLiteral("Theme.mobileGutter")),
                     qPrintable(QStringLiteral("%1 no longer keeps the phone "
                                               "gutter (%2)")
                                    .arg(QString::fromUtf8(s.file),
                                         QString::fromUtf8(s.what))));
        }

        // And specifically: no full-width surface may take a SMALLER margin
        // on a phone than it does on a desktop. That inversion — "the phone
        // is narrow, so give the content more of it" — is exactly what put
        // the composer's border in the corner of the glass.
        static const QRegularExpression tighterOnMobile(
            QStringLiteral(R"(Layout\.(?:left|right)Margin\s*:\s*Theme\.isMobile\s*\?\s*Theme\.sp\.s([1-6])\b)"));
        const QString view = withoutComments(
            readQml(QStringLiteral("/components/MessageView.qml")));
        QStringList offenders;
        for (auto it = tighterOnMobile.globalMatch(view); it.hasNext();)
            offenders << it.next().captured(0).simplified();
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "MessageView gives a phone a tighter horizontal margin "
                     "than a desktop; use Theme.mobileGutter: ")
                     + offenders.join(QStringLiteral(" | "))));
    }

    // A scrollbar on a touch screen is an indicator, not a handle.
    //
    // Qt's ScrollBar defaults to interactive: true, so the ~10 px strip it
    // occupies along the right edge of a Flickable eats presses and turns
    // them into thumb drags. On a phone that strip is where a thumb lands
    // when you flick the timeline near the bezel, and where MessageBubble's
    // swipe-to-reply begins. Neither iOS nor Android has a draggable
    // scrollbar; both flick the content.
    void touchScrollbarsAreIndicatorsNotHandles()
    {
        const QString src = withoutComments(
            readQml(QStringLiteral("/components/ThemedScrollBar.qml")));
        static const QRegularExpression guarded(
            QStringLiteral(R"(interactive\s*:\s*!\s*Theme\.isMobile)"));
        QVERIFY2(guarded.match(src).hasMatch(),
                 "ThemedScrollBar is interactive on touch again — it will "
                 "swallow flicks along the right edge of every list in the app");
    }

    // The unread divider is re-evaluated when the app comes back.
    //
    // `unreadBoundaryMs` is snapshotted once per visit by _enterRoomContext,
    // which keys on the (server, room, model) triple. A phone resumes into
    // the SAME triple, so nothing re-ran and the "New messages" divider stayed
    // parked where it had been when the app went away — until the user
    // switched rooms and came back, which on a one-channel server is never.
    //
    // Two halves, and the second is the one that gets lost in a refactor:
    // Suspended/Hidden only, never Inactive. Inactive is a transient focus
    // loss — a notification shade, a permission sheet, and on a desktop every
    // click on another window — and re-snapshotting on those would clear the
    // divider every time the user alt-tabbed away from the app.
    void theUnreadDividerIsReEvaluatedWhenTheAppComesBack()
    {
        const QString src = withoutComments(
            readQml(QStringLiteral("/components/MessageView.qml")));

        QVERIFY2(src.contains(QStringLiteral("Qt.ApplicationActive")),
                 "MessageView does not watch the application state, so the "
                 "unread divider still survives a background/foreground cycle "
                 "unchanged");
        QVERIFY2(src.contains(QStringLiteral("Qt.ApplicationSuspended")),
                 "the resume path is not gated on having actually been "
                 "suspended");
        QVERIFY2(!src.contains(QStringLiteral("Qt.ApplicationInactive")),
                 "MessageView reacts to Qt.ApplicationInactive — that fires on "
                 "a notification shade and on every desktop focus change, and "
                 "re-snapshotting there clears the divider under the user");

        // The boundary has to be re-read from settings on resume, not merely
        // recomputed against the frozen one: `_recomputeUnreadDivider` alone
        // resolves the SAME stale timestamp and moves nothing.
        static const QRegularExpression snapshot(
            QStringLiteral(R"(unreadBoundaryMs\s*=\s*_currentRoomId)"));
        int snapshots = 0;
        for (auto it = snapshot.globalMatch(src); it.hasNext();) { it.next(); ++snapshots; }
        QVERIFY2(snapshots >= 2,
                 qPrintable(QStringLiteral(
                     "the read marker is re-snapshotted in %1 place(s); room "
                     "entry and resume are two distinct ones")
                     .arg(snapshots)));
    }

    // Every composer has a send control you can see and tap.
    //
    // ThreadPanel's had none: the entire send path was Keys.onReturnPressed.
    // That is survivable on a desktop, where the main composer has taught you
    // that Return sends and shows a button beside it anyway. On a phone it is
    // a dead end — the software keyboard's return key is a newline glyph, the
    // field is single-line so pressing it does something invisible, and
    // nothing on screen says "post this".
    //
    // Checked as a named list rather than a pattern: "a text field that sends
    // on Return" also describes a dozen dialogs that have an explicit Save or
    // Add button three lines below, and those are fine.
    void everyComposerHasAVisibleSendControl()
    {
        struct Composer { const char* file; const char* sendCall; };
        const Composer composers[] = {
            {"/components/MessageInput.qml", "sendCurrentMessage()"},
            {"/components/ThreadPanel.qml",  "threadPanel._send()"},
        };
        for (const auto& c : composers) {
            const QString file = QString::fromUtf8(c.file);
            const QString call = QString::fromUtf8(c.sendCall);
            const QString src = withoutComments(readQml(file));
            QVERIFY2(src.contains(call),
                     qPrintable(QStringLiteral("%1 no longer calls %2 — retarget "
                                               "this rule rather than dropping it")
                                    .arg(file, call)));
            // A screen reader has to be told what it is…
            QVERIFY2(src.contains(QStringLiteral("Accessible.name: \x22Send")),
                     qPrintable(QStringLiteral("%1 has no control named \x22Send…\x22")
                                    .arg(file)));
            // …and a finger has to be able to reach it. An Accessible
            // annotation on nothing is worse than none.
            static const QRegularExpression tapToSend(
                QStringLiteral(R"(onClicked\s*:[^\n]*_?[Ss]end)"));
            QVERIFY2(tapToSend.match(src).hasMatch(),
                     qPrintable(QStringLiteral("%1 sends on Return but nothing "
                                               "in it sends on a tap")
                                    .arg(file)));
        }
    }

    // No dialog is sized in bare pixels.
    //
    // AddMemberDialog was `width: 460`, opens from the member list and from a
    // channel row — both of which are inside a drawer on a phone — and 460 is
    // wider than every iPhone in portrait, so it hung ~35 pt off each side
    // with its title and its Cancel/Add buttons cut in half. The house form
    // is Math.min(design width, parent.width − gutters); a bare number is a
    // dialog that has only ever been opened on the machine it was written on.
    void dialogsAreNeverWiderThanTheScreenTheyOpenOn()
    {
        static const QRegularExpression rootIsDialog(
            QStringLiteral(R"(^(?:Popup|Dialog)\s*\{)"),
            QRegularExpression::MultilineOption);
        // Column 0 `width:`/`height:` on a dialog root is the dialog's own
        // size; anything indented belongs to something inside it.
        static const QRegularExpression bareSize(
            QStringLiteral(R"(^    (width|height)\s*:\s*\d+\s*$)"),
            QRegularExpression::MultilineOption);

        QStringList offenders;
        int checked = 0;
        for (const QString& path : filesUnder(QStringLiteral(BSFCHAT_QML_DIR),
                                              QStringLiteral("*.qml"))) {
            const QString src = withoutComments(readAll(path));
            if (!rootIsDialog.match(src).hasMatch()) continue;
            ++checked;
            for (auto it = bareSize.globalMatch(src); it.hasNext();) {
                offenders << QStringLiteral("%1: %2")
                                 .arg(QFileInfo(path).fileName(),
                                      it.next().captured(0).simplified());
            }
        }
        QVERIFY2(checked > 0,
                 "no Popup/Dialog-rooted QML found — has the pattern changed? "
                 "This guard is only meaningful while one exists.");
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "dialog sized in bare pixels, so it does not fit a screen "
                     "narrower than the number: ")
                     + offenders.join(QStringLiteral(" | "))));
    }

    // A control that both BINDS and WRITES its own currentIndex must re-bind.
    //
    // D-C1, in the one shape that is easy to miss. `currentIndex: <expr>` on
    // a ComboBox looks like an ordinary binding, but selecting a row makes
    // ComboBox ASSIGN currentIndex itself, and that assignment destroys the
    // binding. The handler that then writes the backing property still works
    // — once. Afterwards the field is a fixed number, and anything else that
    // moves that property moves the pages and leaves the field behind.
    //
    // Found live on the phone section pickers that replaced the settings nav
    // rails. ServerSettings' onAboutToShow resets selectedSection to 0 on
    // every open, so after one pick the dialog reopened showing Overview with
    // the dropdown still reading "Bots" — the section list and the section on
    // screen disagreeing, with no way back but picking something else and
    // returning. The sliders in ClientSettings have restored their bindings
    // this way since the audio pane was written; the combos did not.
    //
    // SCOPED BY THE BOUND PROPERTY, not by the file, and the first draft of
    // this rule got that wrong. Matching "a file with a currentIndex binding
    // and an onActivated somewhere in it" flagged ChannelSettings.qml, where
    // the two belong to different combos: the slowmode one keeps its index in
    // sync imperatively (no binding to lose), and the role one is
    // `currentIndex: 0` — an INITIAL value, which is supposed to be replaced
    // the moment the user picks something. Only a binding to a property that
    // the file also assigns is the defect, so that is what is matched:
    // literals are skipped precisely because they are the legitimate case.
    //
    // The limit, stated: the RESTORE is looked for anywhere in the file, not
    // in the same block as the binding. A file with two such combos, one
    // fixed and one not, would pass. Narrowing that needs a real block parse;
    // until some file has two, this catches the shape at the point it is
    // introduced, which is when it is cheap to fix.
    void aComboBoxThatWritesItsOwnIndexRestoresTheBinding()
    {
        // currentIndex bound to a bare property path — not a literal, not an
        // inline expression. `(?![\w.$])` keeps the path whole.
        static const QRegularExpression boundToProperty(
            QStringLiteral(R"(^\s*currentIndex\s*:\s*([A-Za-z_$][\w.$]*)\s*$)"),
            QRegularExpression::MultilineOption);
        static const QRegularExpression restore(
            QStringLiteral(R"(currentIndex\s*=\s*Qt\.binding)"));

        QStringList offenders;
        int checked = 0;
        for (const QString& path : filesUnder(QStringLiteral(BSFCHAT_QML_DIR),
                                              QStringLiteral("*.qml"))) {
            const QString src = withoutComments(readAll(path));
            for (auto it = boundToProperty.globalMatch(src); it.hasNext();) {
                const QString prop = it.next().captured(1);
                // Is that property ever assigned in this file? If not, the
                // binding is never at risk — nothing writes currentIndex back.
                const QRegularExpression written(
                    QRegularExpression::escape(prop) + QStringLiteral(R"(\s*=[^=])"));
                if (!written.match(src).hasMatch()) continue;
                ++checked;
                if (restore.match(src).hasMatch()) continue;
                offenders << QStringLiteral("%1: currentIndex: %2")
                                 .arg(QFileInfo(path).fileName(), prop);
            }
        }
        QVERIFY2(checked > 0,
                 "nothing binds currentIndex to a property it also assigns any "
                 "more — has the pattern changed? This guard is only "
                 "meaningful while something does.");
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "currentIndex is bound to a property the same file writes, "
                     "with no Qt.binding restore — the control stops tracking "
                     "after the first selection: ")
                     + offenders.join(QStringLiteral(" | "))));
    }

    // The voice dock always has room for the button that ends the call.
    //
    // It did not. A single RowLayout with fixed margins, and a RowLayout
    // neither wraps nor clips: when its children's preferred widths exceed
    // the space, it lays them out past the edge. With the full desktop
    // control set on a 412 dp Pixel 6 Pro the row wanted 655 dp and the
    // disconnect button — last child of the centre cluster — began at
    // x = 569, which is 197 dp off the side of the screen. A call the owner
    // could not hang up.
    //
    // TWO HALVES, and the first is the one that has to hold.
    //
    // (a) Structure. The button is positioned from the dock's own right
    //     edge, and the row that holds everything else ends where the button
    //     begins. No number of controls added to its left can move it. This
    //     is checkable from the source text exactly as written.
    //
    // (b) Budget. This is only honest because of a property peculiar to this
    //     file: EVERY control in that row is a DockButton, a component
    //     declared in the same file with one known size. So the row's worst
    //     case really is countable — n buttons, n-1 gaps, the gutters and the
    //     leave button — and no implicit width has to be guessed at. It is
    //     not a general row-width checker and must not be extended into one;
    //     for any other row the numbers would be invented.
    //
    //     Deliberately pessimistic in two ways. It counts every DockButton in
    //     the row including the push-to-talk one, which is mutually exclusive
    //     with plain mute and so can never be on screen beside it — that is
    //     one button of free headroom. And it measures against 360 dp, the
    //     narrowest Android phone class we mean to support, not against the
    //     412 the bug was found on. Below 360 the identity cluster has
    //     already shrunk to nothing and the controls begin to clip, which is
    //     survivable; the leave button going anywhere is not.
    void theVoiceDockAlwaysHasRoomForTheLeaveButton()
    {
        const QString src = withoutComments(
            readQml(QStringLiteral("/components/VoiceDock.qml")));
        QVERIFY2(!src.isEmpty(), "VoiceDock.qml not found");

        // ── (a) structure ────────────────────────────────────────────
        const qsizetype leaveAt = src.indexOf(QStringLiteral("id: leaveBtn"));
        QVERIFY2(leaveAt > 0,
                 "VoiceDock.qml has no `leaveBtn` — if the hang-up control was "
                 "renamed, retarget this rule rather than dropping it");
        const qsizetype rowAt = src.indexOf(QStringLiteral("anchors.right: leaveBtn.left"));
        QVERIFY2(rowAt > 0,
                 "nothing in VoiceDock.qml is bounded by `leaveBtn.left` — the "
                 "control row can reach past the hang-up button again");
        QVERIFY2(leaveAt < rowAt,
                 "leaveBtn is declared after the row that anchors to it; an "
                 "anchor to a later sibling does not resolve");
        // The button's own position must come from the dock, not from a layout.
        const QString leaveBlock = src.mid(leaveAt, 600);
        QVERIFY2(leaveBlock.contains(QStringLiteral("anchors.right: parent.right")),
                 "leaveBtn is no longer anchored to the dock's right edge, so "
                 "its position is once again whatever its siblings leave over");
        QVERIFY2(!leaveBlock.contains(QStringLiteral("Layout.")),
                 "leaveBtn has Layout attached properties, which means it is "
                 "back inside a layout and can be pushed off the edge again");

        // ── (b) budget ───────────────────────────────────────────────
        // Every DockButton USAGE (the `component DockButton:` declaration is
        // excluded by requiring the brace to open a new object, not a type).
        static const QRegularExpression usage(
            QStringLiteral(R"((?<!component )DockButton\s*\{)"));
        int buttons = 0;
        for (auto it = usage.globalMatch(src); it.hasNext();) { it.next(); ++buttons; }
        QVERIFY2(buttons >= 2,
                 "fewer than two DockButtons in VoiceDock.qml — the dock has "
                 "been restructured and this budget no longer describes it");

        const int inRow      = buttons - 1;          // all but the anchored one
        const int touchSize  = 44;                   // Theme.isMobile branch
        const int clusterGap = 8;                    // Theme.sp.s3
        const int rowToLeave = 12;                   // Theme.sp.s5
        const int gutter     = 16;                   // Theme.mobileGutter
        const int narrowestPhoneDp = 360;

        const int worstCase = inRow * touchSize
                            + (inRow - 1) * clusterGap
                            + rowToLeave
                            + touchSize                 // the leave button
                            + 2 * gutter;
        QVERIFY2(worstCase <= narrowestPhoneDp,
                 qPrintable(QStringLiteral(
                     "the voice dock's controls need %1 dp at the touch size "
                     "and the narrowest phone we support is %2 dp. %3 buttons "
                     "in the row is one too many: drop one on mobile, or move "
                     "it into the voice room view.")
                     .arg(worstCase).arg(narrowestPhoneDp).arg(inRow)));
    }

    // Exactly one thing may decide what the mobile main column shows.
    //
    // The owner photographed a Pixel 6 Pro in voice channel #spikk, two
    // participants, live screen-share and camera tiles on screen — with the
    // words "No channel selected" composited across the middle of all of it.
    // The shell asked "what is on screen" twice, in two incompatible ways:
    //
    //     StackLayout { currentIndex: ...viewingVoiceRoom ? 1 : 0
    //                   MessageView { id: chatView
    //                                 visible: ...activeRoomId !== "" }
    //                   VoiceRoom { } }
    //     ColumnLayout { visible: !chatView.visible ... }   // the empty state
    //
    // A StackLayout WRITES `visible` on each of its children whenever
    // currentIndex changes — that is how it shows one page. So
    // `chatView.visible` meant "the chat page is on top", not "there is a
    // channel", and the two writers disagreed in both directions:
    // flipping to voice turned the empty state ON over the video, and a
    // channel arriving during a call re-fired MessageView's own binding and
    // turned the timeline ON over the video as well — the layout's write
    // does not kill a declared binding. Both are measured against a real
    // StackLayout in tests/qml/tst_mainsurface.qml.
    //
    // Hence two halves here, and (b) is the one that fails on the commit
    // before the fix:
    //
    //   (a) no DIRECT CHILD of that StackLayout declares its own `visible:`.
    //       Children are found by brace depth, so a `visible:` on a
    //       grandchild — the two CTAs inside the empty-state page — is not
    //       caught, and should not be: the layout does not own those.
    //   (b) no `visible:` binding ANYWHERE in the file reads the `.visible`
    //       of one of those children. That is the general form of
    //       `!chatView.visible`, and it stays wrong however it is spelled.
    //
    // What this does NOT catch: an overlay that is a sibling of the
    // StackLayout and simply has no `visible:` at all, or one gated on
    // something unrelated that happens to be true during a call. The sweep
    // that accompanied the fix found none — everything else over the column
    // is user-summoned (drawers, settings and profile popups, LoginDialog)
    // or deliberately always-on-top (ToastHost) — but a new one would have
    // to be caught by reading the file, not by this.
    void theMobileMainColumnHasExactlyOneWriter()
    {
        const QString src = withoutComments(
            readQml(QStringLiteral("/mobile/MobileMain.qml")));
        QVERIFY2(!src.isEmpty(), "MobileMain.qml not found");

        // One StackLayout in this file, and it is the main column. If a
        // second one is ever added, this rule is aimed at the wrong block
        // and needs retargeting rather than deleting.
        static const QRegularExpression stackDecl(QStringLiteral(R"(\bStackLayout\s*\{)"));
        int stacks = 0;
        for (auto it = stackDecl.globalMatch(src); it.hasNext();) { it.next(); ++stacks; }
        QCOMPARE(stacks, 1);

        const QString stack = blockBody(src, QStringLiteral(R"(\bStackLayout\s*\{)"));
        QVERIFY2(!stack.isEmpty(), "could not read the StackLayout's body");

        // Walk the body once, tracking brace depth relative to it. Depth 0 is
        // the StackLayout's own properties; depth 1 is inside a direct child.
        QStringList offenders;
        QStringList childIds;
        int depth = 0;
        const QStringList lines = stack.split(QLatin1Char('\n'));
        for (const QString& line : lines) {
            const QString t = line.trimmed();
            const int opens  = line.count(QLatin1Char('{'));
            const int closes = line.count(QLatin1Char('}'));
            // A declaration's own text sits at the depth BEFORE its brace.
            const int here = depth + (t.startsWith(QLatin1Char('}')) ? -closes : 0);
            if (here == 1) {
                if (t.startsWith(QLatin1String("visible:")))
                    offenders << t;
                static const QRegularExpression idDecl(
                    QStringLiteral(R"(\bid:\s*([a-z_]\w*))"));
                const auto m = idDecl.match(t);
                if (m.hasMatch()) childIds << m.captured(1);
            }
            depth += opens - closes;
        }

        // ── (a) ──────────────────────────────────────────────────────
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "a page of the mobile main column declares its own"
                     " `visible:`, which fights the StackLayout for the same"
                     " property and is how \"No channel selected\" got painted"
                     " over live video. Let the layout decide, via"
                     " MainSurface.mainPage(). Offending: ")
                     + offenders.join(QStringLiteral(" | "))));

        // ── (b) ──────────────────────────────────────────────────────
        QVERIFY2(!childIds.isEmpty(),
                 "no ids found on the StackLayout's pages — the depth walk"
                 " above has stopped matching the file's shape");
        for (const QString& binding : visibleBindings(src)) {
            for (const QString& id : childIds) {
                QVERIFY2(!binding.contains(id + QStringLiteral(".visible")),
                         qPrintable(QStringLiteral(
                             "something in MobileMain.qml keys its visibility"
                             " off `%1.visible`. That is a StackLayout page, so"
                             " that property means \"this page is on top\", not"
                             " anything about the app's state — and it is false"
                             " while the voice room is showing. Ask"
                             " MainSurface.js instead. Binding: %2")
                             .arg(id, binding)));
            }
        }

        // ── (c) ──────────────────────────────────────────────────────
        // Last, because (a) and (b) are the ones that describe the hazard
        // and they should be what a broken tree reports. This one is the
        // structural marker: the pages are chosen by the one tested
        // function, not by an inline conditional that can grow a second
        // opinion the way `... ? 1 : 0` did.
        QVERIFY2(stack.contains(QStringLiteral("MainSurface.mainPage(")),
                 "the main column no longer picks its page through"
                 " MainSurface.mainPage(); qml/js/MainSurface.js is the only"
                 " place that answer is allowed to be computed");
    }

    // Every .js the QML module imports is also REGISTERED in CMakeLists.txt.
    //
    // This one is here because the fix above shipped a crash. MainSurface.js
    // was created, imported by MobileMain.qml, unit-tested, reviewed and
    // merged — and never added to the qt_add_qml_module file list, so it was
    // not in the binary. On the owner's Pixel:
    //
    //   qrc:/qt/qml/BSFChat/qml/mobile/MobileMain.qml:6:1:
    //       Script qrc:/qt/qml/BSFChat/qml/js/MainSurface.js unavailable
    //   Process com.bsfchat.app has died
    //
    // Every layer of the net had the same hole — they all look at the SOURCE
    // TREE, and the bug was in what got packaged:
    //
    //   * the .js unit tests load the file from disk through
    //     QUICK_TEST_SOURCE_DIR, so a file missing from the module passes;
    //   * no desktop test instantiates MobileMain.qml (it imports the
    //     BSFChat module, which is compiled into the app binary);
    //   * the Android CI job builds an APK but never launches it, so an
    //     unresolvable import is not a build error;
    //   * and the rest of THIS file scans files on disk, which is exactly
    //     where the missing file was sitting, perfectly readable.
    //
    // The property is static and mechanical, so it belongs here, where it
    // costs milliseconds and names the line to add. It is also the general
    // form: it is not about MainSurface.js, it is about the next .js someone
    // adds to qml/ and imports without touching the build.
    //
    // Scans .qml AND .js sources, because a JS library can import another
    // one (qml/js/UpdateFormat.js does `.import "PlaybackMath.js"`), and
    // resolves each import against the IMPORTING file's directory, because
    // they are written relative ("../js/Foo.js", "../data/EmojiData.js",
    // "PlaybackMath.js") and land in more than one directory.
    //
    // Registration is checked as an exact whole-line match in CMakeLists.txt,
    // which is how that list is written — one path per line. A path merely
    // MENTIONED in a CMake `#` comment therefore does not count, which is the
    // intent.
    //
    // What this does NOT catch: a .js listed in CMakeLists.txt under the
    // wrong target, or a resource that is registered but given a different
    // alias. Both are visible in build-android/.qt/rcc/*.qrc, which is the
    // artefact to read if this rule passes and the device still says
    // "unavailable".
    void everyJsTheQmlModuleImportsIsRegisteredInCMake()
    {
        const QString rootDir = QStringLiteral(BSFCHAT_ROOT_DIR);
        const QString cmakeText = readAll(rootDir + QStringLiteral("/CMakeLists.txt"));
        QVERIFY2(!cmakeText.isEmpty(), "could not read the top-level CMakeLists.txt");

        // One list entry per line is how qt_add_qml_module is written here.
        QSet<QString> registered;
        for (const QString& line : cmakeText.split(QLatin1Char('\n'))) {
            const QString t = line.trimmed();
            if (t.endsWith(QLatin1String(".js")))
                registered.insert(t);
        }
        QVERIFY2(!registered.isEmpty(),
                 "no .js entries found in CMakeLists.txt — the module's file"
                 " list has moved and this rule is looking in the wrong place");

        // Both spellings: QML's `import "x.js" as X` and a JS library's own
        // `.import "x.js" as X`.
        static const QRegularExpression jsImport(
            // A plain escaped literal, not a raw string: AUTOMOC's parser
            // silently produces an EMPTY .moc when it meets a raw string with
            // a custom delimiter, and the only symptom is an undefined vtable
            // at link time.
            QStringLiteral("^\\s*\\.?import\\s+\"([^\"]+\\.js)\""),
            QRegularExpression::MultilineOption);

        const QString qmlDir = QStringLiteral(BSFCHAT_QML_DIR);
        QStringList offenders;
        QDirIterator it(qmlDir, {QStringLiteral("*.qml"), QStringLiteral("*.js")},
                        QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const QFileInfo fi(path);
            const QString src = withoutComments(readAll(path));
            for (auto m = jsImport.globalMatch(src); m.hasNext();) {
                const QString spelled = m.next().captured(1);
                const QString abs = QDir::cleanPath(
                    fi.absolutePath() + QLatin1Char('/') + spelled);
                const QString repoRel = QDir(rootDir).relativeFilePath(abs);
                const QString who = QDir(rootDir).relativeFilePath(path);
                if (!QFileInfo::exists(abs)) {
                    offenders << QStringLiteral("%1 imports \"%2\", which does"
                                                " not exist (resolved to %3)")
                                     .arg(who, spelled, repoRel);
                } else if (!registered.contains(repoRel)) {
                    offenders << QStringLiteral("%1 imports \"%2\" but %3 is"
                                                " not in CMakeLists.txt")
                                     .arg(who, spelled, repoRel);
                }
            }
        }

        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "a .js file is imported by the QML module but is not in"
                     " the qt_add_qml_module file list, so it is NOT in the"
                     " binary. The app loads on desktop from the source tree"
                     " and dies on the device with \"Script ... unavailable\"."
                     " Add the path to CMakeLists.txt. Offending:\n  ")
                     + offenders.join(QStringLiteral("\n  "))));
    }

private:
    // The text between the braces of the first block whose opening matches
    // `opener` (which must end at that block's `{`). Null when there is none.
    static QString blockBody(const QString& src, const QString& opener)
    {
        const qsizetype at = src.indexOf(QRegularExpression(opener));
        if (at < 0) return QString();
        qsizetype i = src.indexOf(QLatin1Char('{'), at);
        if (i < 0) return QString();
        const qsizetype bodyStart = i + 1;
        int nesting = 0;
        for (; i < src.size(); ++i) {
            if (src.at(i) == QLatin1Char('{')) ++nesting;
            else if (src.at(i) == QLatin1Char('}') && --nesting == 0) break;
        }
        return src.mid(bodyStart, i - bodyStart);
    }

    static QString readQml(const QString& relative)
    {
        QFile f(QStringLiteral(BSFCHAT_QML_DIR) + relative);
        [&] { QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(f.fileName())); }();
        return QString::fromUtf8(f.readAll());
    }

    // Each `width:` binding as one line, with continuation lines folded in.
    // QML wraps a subtraction onto the next line starting with `-`, and the
    // whole point of the scrollbar-gutter scan is whether that subtraction
    // is there, so an unfolded per-line search would report every wrapped
    // binding as an offender.
    static QStringList widthBindings(const QString& src)
    {
        QStringList out;
        const QStringList lines = src.split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            if (!lines[i].trimmed().startsWith(QLatin1String("width:"))) continue;
            QString binding = lines[i].trimmed();
            for (int j = i + 1; j < lines.size(); ++j) {
                const QString next = lines[j].trimmed();
                if (!next.startsWith(QLatin1Char('-')) && !next.startsWith(QLatin1Char('+')))
                    break;
                binding += QLatin1Char(' ') + next;
            }
            out << binding;
        }
        return out;
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

    // Same folding, for `opacity:`. Reveal-on-hover is written as an opacity
    // ramp about as often as it is written as a visibility flip, and the
    // touch-path rule has to see both.
    static QStringList opacityBindings(const QString& src)
    {
        QStringList out;
        const QStringList lines = src.split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            if (!lines[i].trimmed().startsWith(QLatin1String("opacity:"))) continue;
            QString binding = lines[i].trimmed();
            for (int j = i + 1; j < lines.size(); ++j) {
                const QString next = lines[j].trimmed();
                if (!next.startsWith(QLatin1String("&&")) && !next.startsWith(QLatin1String("||"))
                    && !next.startsWith(QLatin1Char('?')) && !next.startsWith(QLatin1Char(':')))
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
