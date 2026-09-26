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
#include <QHash>
#include <QStringList>
#include <QVariantList>

// The REAL timeline model, out of bsfchat-lib. Not a stub: the point of
// tst_messageview_real.qml is to measure what the shipped ListView does with
// a real row count, real delegate heights and the real countChanged /
// rowsInserted traffic a MessageModel emits. A QAbstractListModel stub would
// reproduce the row count and none of the timing, and the timing is where
// the bottom-anchor bug lived.
#include "model/MessageModel.h"

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

    // ── the surface qml/components/MessageView.qml reads ───────────────
    //
    // Added for tests/qml_components/tst_messageview_real.qml. The banner
    // cases above do not touch any of it and are unaffected.
    //
    // `messageModel` hands out a REAL MessageModel (src/model/MessageModel.h,
    // already in bsfchat-lib). MessageView's whole scroll apparatus is
    // written against that class by name — it calls isPinnedToEnd,
    // scrollPolicy, restoreIndexForDivider and firstUnreadEventIdAfterTs on
    // it as Q_INVOKABLEs — so substituting a QAbstractListModel stub would
    // not merely be less real, it would not load.
    Q_PROPERTY(QObject* messageModel READ messageModel CONSTANT)
    Q_PROPERTY(QString activeRoomId READ activeRoomId
               WRITE setActiveRoomId NOTIFY activeRoomIdChanged)
    Q_PROPERTY(QString activeRoomName READ activeRoomName
               WRITE setActiveRoomName NOTIFY activeRoomNameChanged)
    Q_PROPERTY(QString activeRoomTopic READ activeRoomTopic
               WRITE setActiveRoomTopic NOTIFY activeRoomTopicChanged)
    Q_PROPERTY(QString typingDisplay READ typingDisplay
               WRITE setTypingDisplay NOTIFY typingDisplayChanged)
    Q_PROPERTY(QString serverUrl READ serverUrl CONSTANT)
    Q_PROPERTY(bool canAttach READ canAttach WRITE setCanAttach
               NOTIFY canAttachChanged)
    Q_PROPERTY(bool initialSyncComplete READ initialSyncComplete
               WRITE setInitialSyncComplete NOTIFY initialSyncCompleteChanged)
    Q_PROPERTY(QStringList pinnedEventIds READ pinnedEventIds
               NOTIFY pinnedEventIdsChanged)
    Q_PROPERTY(QVariantList categorizedRooms READ categorizedRooms
               NOTIFY categorizedRoomsChanged)

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

    // ── MessageView's reads ────────────────────────────────────────────

    QObject* messageModel() const { return m_messageModel; }
    MessageModel* typedMessageModel() const { return m_messageModel; }

    QString activeRoomId() const { return m_activeRoomId; }
    void setActiveRoomId(const QString& v)
    {
        if (m_activeRoomId == v) return;
        m_activeRoomId = v;
        emit activeRoomIdChanged();
    }

    QString activeRoomName() const { return m_activeRoomName; }
    void setActiveRoomName(const QString& v)
    {
        if (m_activeRoomName == v) return;
        m_activeRoomName = v;
        emit activeRoomNameChanged();
    }

    QString activeRoomTopic() const { return m_activeRoomTopic; }
    void setActiveRoomTopic(const QString& v)
    {
        if (m_activeRoomTopic == v) return;
        m_activeRoomTopic = v;
        emit activeRoomTopicChanged();
    }

    QString typingDisplay() const { return m_typingDisplay; }
    void setTypingDisplay(const QString& v)
    {
        if (m_typingDisplay == v) return;
        m_typingDisplay = v;
        emit typingDisplayChanged();
    }

    QString serverUrl() const { return QStringLiteral("https://test.invalid"); }

    bool canAttach() const { return m_canAttach; }
    void setCanAttach(bool v)
    {
        if (m_canAttach == v) return;
        m_canAttach = v;
        emit canAttachChanged();
    }

    bool initialSyncComplete() const { return m_initialSyncComplete; }
    void setInitialSyncComplete(bool v)
    {
        if (m_initialSyncComplete == v) return;
        m_initialSyncComplete = v;
        emit initialSyncCompleteChanged();
    }

    QStringList pinnedEventIds() const { return m_pinnedEventIds; }
    QVariantList categorizedRooms() const { return m_categorizedRooms; }

    // The calls MessageView makes into the connection. Recorded rather than
    // ignored where a test asserts on them; inert otherwise. None of them
    // does any work, because none of the behaviour under measurement is on
    // the far side of one — MessageView's timeline geometry is decided
    // entirely by the model's row count and the view's own bookkeeping.
    Q_INVOKABLE void loadOlderMessages(const QString& = {}, int = 0)
    {
        ++m_loadOlderCalls;
    }
    Q_INVOKABLE void fillHistoryForViewport(const QString& = {}, int = 0) {}
    Q_INVOKABLE void markRoomRead(const QString& = {}) {}
    Q_INVOKABLE void noteTimelineVisible(bool = true) {}
    Q_INVOKABLE void setTimelineAtBottom(bool v) { m_timelineAtBottom = v; }
    Q_INVOKABLE void togglePinnedEvent(const QString& = {}) {}
    Q_INVOKABLE void toggleReaction(const QString& = {}, const QString& = {}) {}
    Q_INVOKABLE void redactEvent(const QString& = {}, const QString& = {}) {}
    Q_INVOKABLE void sendMediaMessage(const QString& = {}) {}
    Q_INVOKABLE void activateRoomByName(const QString& = {}) {}
    Q_INVOKABLE QString messageLink(const QString& eventId) const
    {
        return QStringLiteral("https://test.invalid/#/room/!r/") + eventId;
    }

    // ── the test's own handles on the model ────────────────────────────
    //
    // appendLocalEcho is the cheapest real row there is: no bsfchat::RoomEvent
    // to build, no own-user-id plumbing, and it lands through
    // beginInsertRows/endInsertRows + countChanged exactly as a synced
    // message does — which is the signal traffic MessageView's
    // _jumpToEnd/onCountChanged path keys off.
    Q_INVOKABLE void pushMessages(int n)
    {
        for (int i = 0; i < n; ++i) {
            const QString id = QStringLiteral("echo-%1").arg(++m_echoSeq);
            m_messageModel->appendLocalEcho(
                id,
                QStringLiteral("message %1").arg(m_echoSeq),
                QString(),
                QStringLiteral("@tester:test.invalid"));
        }
    }

    Q_INVOKABLE void clearMessages()
    {
        m_messageModel->clear();
        m_echoSeq = 0;
    }

    Q_INVOKABLE int messageCount() const { return m_messageModel->rowCount(); }
    Q_INVOKABLE bool timelineAtBottom() const { return m_timelineAtBottom; }
    Q_INVOKABLE int loadOlderCalls() const { return m_loadOlderCalls; }
    Q_INVOKABLE void resetTimelineRecording()
    {
        m_loadOlderCalls = 0;
        m_timelineAtBottom = false;
    }

signals:
    void connectionStatusChanged();
    void syncErrorMessageChanged();
    void needsReauthChanged();
    void reauthInProgressChanged();

    void activeRoomIdChanged();
    void activeRoomNameChanged();
    void activeRoomTopicChanged();
    void typingDisplayChanged();
    void canAttachChanged();
    void initialSyncCompleteChanged();
    void pinnedEventIdsChanged();
    void categorizedRoomsChanged();
    // MessageView's Connections block declares onMessageReceived. Declared
    // here so that block resolves rather than warning about a signal the
    // target does not have — nothing in these tests emits it.
    void messageReceived(const QString& roomId, const QString& senderDisplayName,
                         const QString& body, const QString& eventId,
                         bool mentionsMe);

private:
    // Default to the healthy state, so every test case starts from "the
    // banner says nothing" and has to put it into the state it asserts on.
    int m_status = 1;
    QString m_syncError;
    bool m_needsReauth = false;
    bool m_reauthInProgress = false;

    MessageModel* m_messageModel = new MessageModel(this);
    QString m_activeRoomId = QStringLiteral("!room:test.invalid");
    QString m_activeRoomName = QStringLiteral("general");
    QString m_activeRoomTopic;
    QString m_typingDisplay;
    bool m_canAttach = true;
    bool m_initialSyncComplete = true;
    QStringList m_pinnedEventIds;
    QVariantList m_categorizedRooms;
    int m_echoSeq = 0;
    int m_loadOlderCalls = 0;
    bool m_timelineAtBottom = false;
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
        m_connection->clearMessages();
        m_connection->resetTimelineRecording();
        m_connection->setActiveRoomId(QStringLiteral("!room:test.invalid"));
        setActiveServer(m_connection);
        setActiveServerIndex(0);
        m_reauthCalls = 0;
        m_lastReauthIndex = -1;
        m_copiedText.clear();
        m_openedLinks.clear();
        emit reauthCallsChanged();
    }

    Q_INVOKABLE void reauthenticateServer(int index)
    {
        ++m_reauthCalls;
        m_lastReauthIndex = index;
        emit reauthCallsChanged();
    }

    // MessageView's context-menu actions. Recorded, so a test can say the
    // shipped menu item reached the shipped call rather than that the string
    // appears in the file.
    Q_INVOKABLE void copyToClipboard(const QString& text) { m_copiedText = text; }
    Q_INVOKABLE void openMessageLink(const QString& url) { m_openedLinks << url; }
    Q_INVOKABLE QString lastCopiedText() const { return m_copiedText; }
    Q_INVOKABLE QStringList openedLinks() const { return m_openedLinks; }

    // The live connection, typed, so a QML test can drive the timeline.
    Q_INVOKABLE QObject* connection() const { return m_connection; }

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
    QString m_copiedText;
    QStringList m_openedLinks;
};

// The two remaining context properties anything under MessageView reaches
// for. Both are recorders rather than no-ops: `haptics` is what
// MessageBubble's long-press path calls, and `appSettings` carries the read
// marker MessageView's unread divider is computed from, so a test that wants
// a divider has to be able to set it.
//
// NOT the real Haptics (it drives the platform's taptic engine) and NOT the
// real Settings (its constructor builds a QMediaDevices — see this file's
// header).
class FakeHaptics : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    Q_INVOKABLE void tick() { ++m_ticks; }
    Q_INVOKABLE void longPress() { ++m_longPresses; }
    Q_INVOKABLE int ticks() const { return m_ticks; }
    Q_INVOKABLE int longPresses() const { return m_longPresses; }
    Q_INVOKABLE void resetForTest() { m_ticks = 0; m_longPresses = 0; }

private:
    int m_ticks = 0;
    int m_longPresses = 0;
};

class FakeAppSettingsContext : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    // src/core/Settings.cpp's shape: keyed by room id, 0 when unset.
    Q_INVOKABLE qint64 lastReadTs(const QString& roomId) const
    {
        return m_lastRead.value(roomId, 0);
    }
    Q_INVOKABLE void setLastReadTs(const QString& roomId, qint64 ts)
    {
        m_lastRead.insert(roomId, ts);
    }
    Q_INVOKABLE void resetForTest() { m_lastRead.clear(); }

private:
    QHash<QString, qint64> m_lastRead;
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

        auto* haptics = new FakeHaptics(this);
        QQmlEngine::setObjectOwnership(haptics, QQmlEngine::CppOwnership);
        engine->rootContext()->setContextProperty("haptics", haptics);

        auto* appSettings = new FakeAppSettingsContext(this);
        QQmlEngine::setObjectOwnership(appSettings, QQmlEngine::CppOwnership);
        engine->rootContext()->setContextProperty("appSettings", appSettings);
    }
};

QUICK_TEST_MAIN_WITH_SETUP(qml_components, QmlComponentsSetup)

#include "qml_components_test_main.moc"
