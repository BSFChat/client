// Runner for the QML tests under tests/qml_components/ — the ones that
// instantiate REAL shipped components out of the BSFChat module.
//
// This is the target that the whole bsfchat-lib split exists for. Until the
// QML module moved off the executable and onto a static library, no test
// binary could `import BSFChat`, so every QML test under tests/qml/ is a
// hand-built REPLICA of the structure it means to check: the rules in
// qml/js/*.js get exercised, and whether the shipped .qml file actually reads
// those rules is asserted by grepping its source text in
// tests/test_qml_hygiene.cpp. A timeline-anchoring bug shipped through that
// gap.
//
// Here the component under test is the file that ships. Nothing is copied.
//
// WHAT THIS RUNNER HAS TO PROVIDE
//
// A shipped component is not a pure function; it expects the environment
// src/main.cpp builds around it. Two pieces of that, and only two, are needed
// to bring ConnectionBanner.qml up:
//
//   * the `AppSettings` QML SINGLETON, because qml/theme/Theme.qml binds
//     isDark/variant/accentHue/accessibility to it and every component in the
//     module reads Theme. main.cpp registers the real Settings here.
//   * the `serverManager` CONTEXT PROPERTY, which is what the banner reads
//     its four connection properties from and what its button calls.
//
// Both are stubbed rather than real, deliberately, and for different reasons:
//
//   * Settings' constructor builds a QMediaDevices on desktop to keep the
//     audio device lists live. That brings up Qt's multimedia backend, and
//     this machine's rule is that no test touches the microphone or camera
//     stack. FakeAppSettings answers the four properties Theme.qml reads,
//     with the real defaults from src/core/Settings.cpp, and the real
//     Theme.qml computes every colour from them.
//   * ServerManager/ServerConnection would need a server. The banner reads
//     four Q_PROPERTYs off the active connection and calls one method on the
//     manager; FakeServerManager has exactly that surface, with the same
//     names, types and NOTIFY-signal behaviour, so the binding dependency
//     tracking under test is the real thing.
//
// The stubs are the ENVIRONMENT. The component is not stubbed.
#include <QtQuickTest>

#include <QObject>
#include <QQmlContext>
#include <QQmlEngine>
#include <QString>

// A stand-in for src/net/ServerConnection.h, carrying only the four
// properties qml/js/ConnectionBanner.js reads. Writable from QML so a test
// case can drive the connection through its states and watch the real
// component react — the thing a replica cannot do at all.
//
// connectionStatus is the same int contract ServerConnection documents:
// 0 = disconnected, 1 = connected/syncing, 2 = reconnecting, 3 = expired.
class FakeServerConnection : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int connectionStatus READ connectionStatus
               WRITE setConnectionStatus NOTIFY connectionStatusChanged)
    Q_PROPERTY(QString syncErrorMessage READ syncErrorMessage
               WRITE setSyncErrorMessage NOTIFY syncErrorMessageChanged)
    Q_PROPERTY(bool needsReauth READ needsReauth
               WRITE setNeedsReauth NOTIFY needsReauthChanged)
    Q_PROPERTY(bool reauthInProgress READ reauthInProgress
               WRITE setReauthInProgress NOTIFY reauthInProgressChanged)

public:
    using QObject::QObject;

    int connectionStatus() const { return m_status; }
    void setConnectionStatus(int v)
    {
        if (m_status == v) return;
        m_status = v;
        emit connectionStatusChanged();
    }

    QString syncErrorMessage() const { return m_syncError; }
    void setSyncErrorMessage(const QString& v)
    {
        if (m_syncError == v) return;
        m_syncError = v;
        emit syncErrorMessageChanged();
    }

    bool needsReauth() const { return m_needsReauth; }
    void setNeedsReauth(bool v)
    {
        if (m_needsReauth == v) return;
        m_needsReauth = v;
        emit needsReauthChanged();
    }

    bool reauthInProgress() const { return m_reauthInProgress; }
    void setReauthInProgress(bool v)
    {
        if (m_reauthInProgress == v) return;
        m_reauthInProgress = v;
        emit reauthInProgressChanged();
    }

signals:
    void connectionStatusChanged();
    void syncErrorMessageChanged();
    void needsReauthChanged();
    void reauthInProgressChanged();

private:
    // Default to the healthy state, so every test case starts from "the
    // banner says nothing" and has to put it into the state it asserts on.
    int m_status = 1;
    QString m_syncError;
    bool m_needsReauth = false;
    bool m_reauthInProgress = false;
};

// A stand-in for src/net/ServerManager.h with the three members the banner
// touches: the active connection, its index, and the re-auth call the button
// makes. reauthCalls/lastReauthIndex are the recording half — they are what
// lets a test assert that pressing the real Button reaches the real slot with
// the real index, which is wiring no .js-level test can see.
class FakeServerManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject* activeServer READ activeServer
               WRITE setActiveServer NOTIFY activeServerChanged)
    Q_PROPERTY(int activeServerIndex READ activeServerIndex
               WRITE setActiveServerIndex NOTIFY activeServerIndexChanged)
    Q_PROPERTY(int reauthCalls READ reauthCalls NOTIFY reauthCallsChanged)
    Q_PROPERTY(int lastReauthIndex READ lastReauthIndex
               NOTIFY reauthCallsChanged)

public:
    explicit FakeServerManager(QObject* parent = nullptr)
        : QObject(parent), m_connection(new FakeServerConnection(this))
    {
    }

    QObject* activeServer() const { return m_activeServer; }
    void setActiveServer(QObject* v)
    {
        if (m_activeServer == v) return;
        m_activeServer = v;
        emit activeServerChanged();
    }

    int activeServerIndex() const { return m_activeServerIndex; }
    void setActiveServerIndex(int v)
    {
        if (m_activeServerIndex == v) return;
        m_activeServerIndex = v;
        emit activeServerIndexChanged();
    }

    int reauthCalls() const { return m_reauthCalls; }
    int lastReauthIndex() const { return m_lastReauthIndex; }

    // Back to the state a fresh test case expects. Called from init() in the
    // QML TestCase rather than relying on case ordering.
    Q_INVOKABLE void resetForTest()
    {
        m_connection->setConnectionStatus(1);
        m_connection->setSyncErrorMessage(QString());
        m_connection->setNeedsReauth(false);
        m_connection->setReauthInProgress(false);
        setActiveServer(m_connection);
        setActiveServerIndex(0);
        m_reauthCalls = 0;
        m_lastReauthIndex = -1;
        emit reauthCallsChanged();
    }

    Q_INVOKABLE void reauthenticateServer(int index)
    {
        ++m_reauthCalls;
        m_lastReauthIndex = index;
        emit reauthCallsChanged();
    }

signals:
    void activeServerChanged();
    void activeServerIndexChanged();
    void reauthCallsChanged();

private:
    FakeServerConnection* m_connection = nullptr;
    QObject* m_activeServer = nullptr;
    int m_activeServerIndex = 0;
    int m_reauthCalls = 0;
    int m_lastReauthIndex = -1;
};

// The four properties qml/theme/Theme.qml binds to, with the defaults
// src/core/Settings.cpp returns when nothing is stored. Theme.qml itself is
// the shipped file — only its input is substituted.
class FakeAppSettings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY themeChanged)
    Q_PROPERTY(QString layoutVariant READ layoutVariant
               WRITE setLayoutVariant NOTIFY layoutVariantChanged)
    Q_PROPERTY(int accentHue READ accentHue
               WRITE setAccentHue NOTIFY accentHueChanged)
    Q_PROPERTY(bool accessibilityMode READ accessibilityMode
               WRITE setAccessibilityMode NOTIFY accessibilityModeChanged)

public:
    using QObject::QObject;

    QString theme() const { return m_theme; }
    void setTheme(const QString& v)
    {
        if (m_theme == v) return;
        m_theme = v;
        emit themeChanged();
    }

    QString layoutVariant() const { return m_layoutVariant; }
    void setLayoutVariant(const QString& v)
    {
        if (m_layoutVariant == v) return;
        m_layoutVariant = v;
        emit layoutVariantChanged();
    }

    int accentHue() const { return m_accentHue; }
    void setAccentHue(int v)
    {
        if (m_accentHue == v) return;
        m_accentHue = v;
        emit accentHueChanged();
    }

    bool accessibilityMode() const { return m_accessibilityMode; }
    void setAccessibilityMode(bool v)
    {
        if (m_accessibilityMode == v) return;
        m_accessibilityMode = v;
        emit accessibilityModeChanged();
    }

signals:
    void themeChanged();
    void layoutVariantChanged();
    void accentHueChanged();
    void accessibilityModeChanged();

private:
    // src/core/Settings.cpp: theme "dark", layoutVariant "standard",
    // accentHue 180, accessibilityMode false.
    QString m_theme = QStringLiteral("dark");
    QString m_layoutVariant = QStringLiteral("standard");
    int m_accentHue = 180;
    bool m_accessibilityMode = false;
};

class QmlComponentsSetup : public QObject
{
    Q_OBJECT

public:
    QmlComponentsSetup() = default;

public slots:
    void qmlEngineAvailable(QQmlEngine* engine)
    {
        // Same registration as src/main.cpp, for the same reason: QML
        // singletons cannot see context properties, so Theme.qml needs
        // AppSettings as a typed singleton on the BSFChat URI.
        auto* settings = new FakeAppSettings(this);
        QQmlEngine::setObjectOwnership(settings, QQmlEngine::CppOwnership);
        qmlRegisterSingletonInstance<FakeAppSettings>(
            "BSFChat", 1, 0, "AppSettings", settings);

        auto* manager = new FakeServerManager(this);
        QQmlEngine::setObjectOwnership(manager, QQmlEngine::CppOwnership);
        manager->resetForTest();
        engine->rootContext()->setContextProperty("serverManager", manager);
    }
};

QUICK_TEST_MAIN_WITH_SETUP(qml_components, QmlComponentsSetup)

#include "qml_components_test_main.moc"
