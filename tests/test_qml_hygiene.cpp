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
};

QTEST_APPLESS_MAIN(QmlHygieneTest)
#include "test_qml_hygiene.moc"
