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
};

QTEST_APPLESS_MAIN(QmlHygieneTest)
#include "test_qml_hygiene.moc"
