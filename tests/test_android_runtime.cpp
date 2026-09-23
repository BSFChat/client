// Android runtime-correctness guards.
//
// None of what this file protects can be exercised by a test process on a
// Mac: it is Java running against a platform that only exists on a phone.
// What CAN be checked here is the SHAPE of that code, and the shape is
// precisely where every one of these bugs lived — an ordering, a gate, a
// manifest attribute. Each case below fails on the commit before its fix.
//
// In the same spirit as test_qml_hygiene: scanning the source is not as
// good as running it, and it is a great deal better than a green ctest
// that proves nothing about the platform the code actually runs on.
//
// What is deliberately NOT claimed here: that any of this WORKS on a
// device. These guards stop a regression, they do not substitute for
// hardware verification.

#include <QtTest>

#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include "core/AndroidNotifier.h"

namespace {

QString readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

QString androidDir() { return QStringLiteral(BSFCHAT_ANDROID_DIR); }
QString srcDir()     { return QStringLiteral(BSFCHAT_SRC_DIR); }

QString java(const QString& name)
{
    return readAll(androidDir() + "/src/com/bsfchat/client/" + name);
}

// Comments removed, so a structural assertion cannot be satisfied — or
// defeated — by prose. Several of the comments in these files quote the
// very constructs being checked for ("No @Override — see above"), which is
// exactly the kind of thing that makes a source scan lie.
//
// Stripping can only ever delete text, so a check can miss an offender but
// cannot invent a passing one. None of these files contains "//" inside a
// string literal.
QString code(QString src)
{
    static const QRegularExpression block(
        QStringLiteral(R"(/\*.*?\*/)"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression line(QStringLiteral("//[^\n]*"));
    return src.remove(block).remove(line);
}

QString javaCode(const QString& name) { return code(java(name)); }

// The body of a Java method, from its signature to the closing brace at
// the same indentation. Crude, but the sources here are 4-space-indented
// plain Java with no nested class bodies at method indent, so it holds —
// and an over-long match can only make a "contains" assertion weaker, not
// invent a passing one.
QString methodBody(const QString& src, const QString& signatureFragment)
{
    const qsizetype start = src.indexOf(signatureFragment);
    if (start < 0) return QString();
    const qsizetype open = src.indexOf('{', start);
    if (open < 0) return QString();
    const qsizetype close = src.indexOf(QStringLiteral("\n    }"), open);
    if (close < 0) return src.mid(open);
    return src.mid(open, close - open);
}

} // namespace

class TestAndroidRuntime : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    // --- manifest -------------------------------------------------------
    void manifestParsesAndKeepsVersionPlaceholders();
    void activityIsSingleTaskForTheOidcRedirect();
    void activityHandlesRotationWithoutRecreation();
    void oauthIntentFilterIsBrowsableAndHostScoped();
    void everyServiceClassExistsAndHasItsFgsPermission();

    // --- MediaProjection ordering (issue 1) ------------------------------
    void projectionIsOpenedOnlyAfterStartForeground();
    void consentResultIsNeverCachedForReuse();

    // --- foreground-service start window (issue 2) -----------------------
    void syncServiceStartIsGatedOnTheAppBeingActive();
    void everyStartForegroundIsInsideATryBlock();

    // --- dataSync 6h cap (issue 3) ---------------------------------------
    void syncServiceHandlesTheAndroid15Timeout();
    void timeoutDegradesInsteadOfRestarting();

    // --- minSdk 28 (issue 5) ---------------------------------------------
    void micAndCameraFgsTypesAreGatedOnApi30NotApi29();
    void cameraFgsTypeIsGatedOnTheCameraPermission();

    // --- no sensor permission prompts at launch --------------------------
    void noMediaBackendIsBroughtUpInAConstructor();
};

void TestAndroidRuntime::initTestCase()
{
    QVERIFY2(!readAll(androidDir() + "/AndroidManifest.xml").isEmpty(),
             "android/AndroidManifest.xml not readable — check "
             "BSFCHAT_ANDROID_DIR");
}

// ---------------------------------------------------------------- manifest

void TestAndroidRuntime::manifestParsesAndKeepsVersionPlaceholders()
{
    const QString m = readAll(androidDir() + "/AndroidManifest.xml");

    // androiddeployqt parses this as strict XML before Gradle sees it, and
    // XML forbids "--" inside a comment. A comment that breaks that rule
    // fails the Android build with a parse error and nothing else.
    static const QRegularExpression comment(
        QStringLiteral("<!--(.*?)-->"),
        QRegularExpression::DotMatchesEverythingOption);
    auto it = comment.globalMatch(m);
    while (it.hasNext()) {
        const QString body = it.next().captured(1);
        QVERIFY2(!body.contains(QStringLiteral("--")),
                 qPrintable("manifest comment contains a double dash, which "
                            "is not legal XML: " + body.left(120)));
    }

    // These must match Qt's own template byte for byte, trailing
    // space-dash-dash included, or androiddeployqt does not recognise them
    // and the build ships versionCode 1 / versionName 1.0.
    QVERIFY(m.contains(
        QStringLiteral("android:versionCode=\"-- %%INSERT_VERSION_CODE%% --\"")));
    QVERIFY(m.contains(
        QStringLiteral("android:versionName=\"-- %%INSERT_VERSION_NAME%% --\"")));
}

void TestAndroidRuntime::activityIsSingleTaskForTheOidcRedirect()
{
    const QString m = readAll(androidDir() + "/AndroidManifest.xml");

    // singleTop only routes an intent to onNewIntent when the instance is
    // already on top of the SAME task. A browser handing back the OIDC
    // redirect starts it with FLAG_ACTIVITY_NEW_TASK, which against a
    // singleTop activity was observed creating a SECOND task — and the
    // PKCE verifier for the sign-in in flight lives only in the first one.
    QVERIFY2(m.contains(QStringLiteral("android:launchMode=\"singleTask\"")),
             "BSFChatActivity must be singleTask: the OIDC redirect arrives "
             "with FLAG_ACTIVITY_NEW_TASK and anything weaker can start a "
             "second instance that has no PKCE verifier.");
    QVERIFY(!m.contains(QStringLiteral("android:launchMode=\"singleTop\"")));
}

void TestAndroidRuntime::activityHandlesRotationWithoutRecreation()
{
    const QString m = readAll(androidDir() + "/AndroidManifest.xml");
    static const QRegularExpression re(
        QStringLiteral("android:configChanges=\"([^\"]*)\""));
    const auto match = re.match(m);
    QVERIFY2(match.hasMatch(), "no configChanges on the activity");
    const QStringList flags = match.captured(1).split('|');

    // Qt's template list. An activity recreation tears the
    // QGuiApplication down under a running native main(), so every one of
    // these has to be handled in-process. "orientation" and "screenSize"
    // together are what make a rotate NOT recreate the activity.
    for (const char* required : {"orientation", "uiMode", "screenLayout",
                                 "screenSize", "smallestScreenSize",
                                 "layoutDirection", "locale", "fontScale",
                                 "keyboard", "keyboardHidden", "navigation",
                                 "mcc", "mnc", "density"}) {
        QVERIFY2(flags.contains(QLatin1String(required)),
                 qPrintable(QStringLiteral("configChanges is missing '%1', "
                                           "which Qt's template declares")
                                .arg(QLatin1String(required))));
    }
    // Ours: the API 31 accessibility "Bold text" toggle is a configuration
    // change Qt's list predates, and without it that toggle recreates the
    // activity.
    QVERIFY(flags.contains(QStringLiteral("fontWeightAdjustment")));
}

void TestAndroidRuntime::oauthIntentFilterIsBrowsableAndHostScoped()
{
    const QString m = readAll(androidDir() + "/AndroidManifest.xml");

    // A redirect from a browser is only delivered if the filter carries
    // BROWSABLE as well as DEFAULT, and the data element must name both
    // scheme and host or it matches nothing.
    QVERIFY(m.contains(QStringLiteral("android.intent.category.BROWSABLE")));
    QVERIFY(m.contains(
        QStringLiteral("<data android:scheme=\"bsfchat\" android:host=\"oauth\" />")));

    // The scheme is NOT claimed bare. That is deliberate (a room deep link
    // is not a sign-in callback), and it is also what keeps this filter
    // from colliding with the ACTION_SEND share filter below it.
    QVERIFY2(!m.contains(QStringLiteral("<data android:scheme=\"bsfchat\" />")),
             "claiming the bare bsfchat scheme would route room deep links "
             "through the sign-in filter");

    // And the C++ side agrees on what a callback looks like.
    const QString oidc = readAll(srcDir() + "/identity/OidcRequest.h");
    QVERIFY(oidc.contains(
        QStringLiteral("kNativeRedirectUri = \"bsfchat://oauth/callback\"")));
}

void TestAndroidRuntime::everyServiceClassExistsAndHasItsFgsPermission()
{
    const QString m = readAll(androidDir() + "/AndroidManifest.xml");

    static const QRegularExpression svc(
        QStringLiteral("<service\\s+android:name=\"([^\"]+)\"[^>]*?"
                       "android:foregroundServiceType=\"([^\"]+)\""),
        QRegularExpression::DotMatchesEverythingOption);
    auto it = svc.globalMatch(m);
    int seen = 0;
    while (it.hasNext()) {
        const auto match = it.next();
        ++seen;
        const QString cls = match.captured(1);
        QVERIFY2(cls.startsWith(QStringLiteral("com.bsfchat.client.")),
                 qPrintable("unexpected service class " + cls));
        const QString file = cls.section('.', -1) + ".java";
        QVERIFY2(!java(file).isEmpty(),
                 qPrintable("manifest names " + cls + " but "
                            "android/src/com/bsfchat/client/" + file
                            + " does not exist"));

        // Android 14 checks a per-type permission at startForeground().
        for (const QString& type : match.captured(2).split('|')) {
            QString perm = QStringLiteral("android.permission.FOREGROUND_SERVICE_");
            for (QChar c : type) {
                if (c.isUpper()) perm += '_';
                perm += c.toUpper();
            }
            QVERIFY2(m.contains(perm),
                     qPrintable("service " + cls + " declares type '" + type
                                + "' but " + perm + " is not requested"));
        }
    }
    QCOMPARE(seen, 3); // Voice, Sync, MediaProjection
}

// ------------------------------------------------- MediaProjection ordering

void TestAndroidRuntime::projectionIsOpenedOnlyAfterStartForeground()
{
    const QString helper = javaCode(QStringLiteral("ScreenCaptureHelper.java"));
    const QString service = javaCode(QStringLiteral("MediaProjectionService.java"));
    QVERIFY(!helper.isEmpty() && !service.isEmpty());

    // The bug: startForegroundService() only ENQUEUES onStartCommand, so a
    // getMediaProjection() in the same method necessarily runs before the
    // service reaches the foreground — which Android 14 answers with a
    // SecurityException, every time.
    const QString onResult =
        methodBody(helper, QStringLiteral("public void onActivityResult("));
    QVERIFY2(!onResult.isEmpty(), "onActivityResult not found");
    QVERIFY2(onResult.contains(QStringLiteral("startForegroundService")),
             "the consent result should start the foreground service");
    QVERIFY2(!onResult.contains(QStringLiteral("getMediaProjection")),
             "getMediaProjection() must NOT run in the same method as "
             "startForegroundService() — the service is not foreground yet, "
             "and Android 14+ throws SecurityException");

    // It belongs in the callback the service makes after startForeground().
    const QString onForegrounded =
        methodBody(helper, QStringLiteral("void onServiceForegrounded("));
    QVERIFY2(onForegrounded.contains(QStringLiteral("getMediaProjection")),
             "getMediaProjection() should be in onServiceForegrounded()");
    QCOMPARE(helper.count(QStringLiteral("getMediaProjection(")), qsizetype(1));

    // And on the service side the callback must come after startForeground.
    const QString onStart =
        methodBody(service, QStringLiteral("public int onStartCommand("));
    const qsizetype fg = onStart.indexOf(QStringLiteral("startForeground("));
    const qsizetype cb = onStart.indexOf(QStringLiteral("onServiceForegrounded("));
    QVERIFY2(fg >= 0 && cb >= 0 && fg < cb,
             "MediaProjectionService must call startForeground() BEFORE it "
             "hands the consent result back to ScreenCaptureHelper");

    // Android 14 also wants the stop-callback registered before the
    // VirtualDisplay exists.
    const qsizetype reg = onForegrounded.indexOf(QStringLiteral("registerCallback"));
    const qsizetype vd = onForegrounded.indexOf(QStringLiteral("createVirtualDisplay"));
    QVERIFY2(reg >= 0 && vd >= 0 && reg < vd,
             "registerCallback() must precede createVirtualDisplay()");
}

void TestAndroidRuntime::consentResultIsNeverCachedForReuse()
{
    const QString helper = javaCode(QStringLiteral("ScreenCaptureHelper.java"));

    // From Android 14 a consent result is single-use: converting the same
    // Intent twice throws. The guard is that the helper holds no Intent
    // field at all, so there is nothing to convert a second time — every
    // start goes back through createScreenCaptureIntent().
    static const QRegularExpression intentField(
        QStringLiteral("^\\s*(private|protected|public|static|final|volatile"
                       "|\\s)*Intent\\s+\\w+\\s*(=|;)"),
        QRegularExpression::MultilineOption);
    auto it = intentField.globalMatch(helper);
    while (it.hasNext()) {
        const QString line = it.next().captured(0).trimmed();
        QVERIFY2(!line.startsWith(QStringLiteral("private"))
                     && !line.startsWith(QStringLiteral("static")),
                 qPrintable("ScreenCaptureHelper must not cache a consent "
                            "Intent (single-use from Android 14): " + line));
    }
    QVERIFY(helper.contains(QStringLiteral("createScreenCaptureIntent()")));
}

// --------------------------------------------- foreground-service start window

void TestAndroidRuntime::syncServiceStartIsGatedOnTheAppBeingActive()
{
    const QString notifier = code(readAll(srcDir() + "/core/AndroidNotifier.cpp"));
    QVERIFY(!notifier.isEmpty());

    const qsizetype gate = notifier.indexOf(
        QStringLiteral("QGuiApplication::applicationState() != Qt::ApplicationActive"));
    const qsizetype start = notifier.indexOf(QStringLiteral("\"startForegroundService\""));
    QVERIFY2(gate >= 0,
             "SyncService must not be started without checking that the app "
             "is in the foreground: Android 12+ makes a background start a "
             "fatal ForegroundServiceStartNotAllowedException");
    QVERIFY2(start > gate,
             "the foreground-state check has to come before the start call");

    // And a start that the platform refuses anyway must be noticed rather
    // than silently swallowed by QJniObject's own exception clearing.
    QVERIFY(notifier.contains(QStringLiteral("checkAndClearExceptions()")));

    // The service is also stopped when the last connection goes. It never
    // was: stopSyncService() had no caller in the whole tree, so a
    // signed-out app kept burning the Android 15 dataSync budget.
    const QString nm = readAll(srcDir() + "/core/NotificationManager.cpp");
    QVERIFY2(nm.contains(QStringLiteral("stopSyncService()")),
             "nothing calls stopSyncService()");
}

void TestAndroidRuntime::everyStartForegroundIsInsideATryBlock()
{
    // An uncaught ForegroundServiceStartNotAllowedException /
    // SecurityException out of startForeground() unwinds into
    // ActivityThread and kills the process. Every one of our services has
    // to survive being told no.
    for (const char* file : {"VoiceService.java", "SyncService.java",
                             "MediaProjectionService.java"}) {
        const QString src = javaCode(QLatin1String(file));
        const QString body =
            methodBody(src, QStringLiteral("public int onStartCommand("));
        QVERIFY2(!body.isEmpty(), qPrintable(QString("no onStartCommand in ")
                                             + file));
        const qsizetype tryAt = body.indexOf(QStringLiteral("try {"));
        const qsizetype fgAt = body.indexOf(QStringLiteral("startForeground("));
        QVERIFY2(tryAt >= 0 && fgAt > tryAt,
                 qPrintable(QString(file) + ": startForeground() must be "
                            "inside a try block"));
        QVERIFY(body.contains(QStringLiteral("catch (Throwable")));
    }
}

// -------------------------------------------------------- dataSync 6h cap

void TestAndroidRuntime::syncServiceHandlesTheAndroid15Timeout()
{
    const QString sync = javaCode(QStringLiteral("SyncService.java"));

    // Android 15 stops a dataSync FGS after ~6h/24h via
    // Service.onTimeout(startId, fgsType), and expects stopSelf() within a
    // few seconds or it throws ForegroundServiceDidNotStopInTimeException.
    QVERIFY2(sync.contains(QStringLiteral("public void onTimeout(int startId, int fgsType)")),
             "SyncService must override onTimeout(int, int) — without it "
             "Android 15 crashes the process when the dataSync budget runs "
             "out");

    // It must NOT carry @Override: the local toolchain compiles against a
    // platform older than API 35, where the two-arg form does not exist.
    const qsizetype at = sync.indexOf(QStringLiteral("public void onTimeout(int startId, int fgsType)"));
    QVERIFY2(!sync.mid(qMax(qsizetype(0), at - 80), 80).contains(QStringLiteral("@Override")),
             "onTimeout(int, int) must not be @Override — it does not exist "
             "below compileSdk 35 and the annotation is a compile error there");

    const QString handler =
        methodBody(sync, QStringLiteral("private void handleTimeout("));
    QVERIFY(handler.contains(QStringLiteral("stopSelf(")));
    QVERIFY(handler.contains(QStringLiteral("notifyNativeStopped")));

    // Firebase stays out of it. The service's whole reason to exist is
    // that push does not go through Google.
    QVERIFY(!sync.contains(QStringLiteral("firebase"), Qt::CaseInsensitive));
    QVERIFY(!sync.contains(QStringLiteral("FirebaseMessaging")));
}

void TestAndroidRuntime::timeoutDegradesInsteadOfRestarting()
{
    // The C++ half of the same decision, and the one piece of this that a
    // desktop test can actually RUN: a timeout must clear the active flag,
    // say so, and not leave the app believing background sync is live.
    AndroidNotifier notifier;
    QSignalSpy activeSpy(&notifier, &AndroidNotifier::backgroundSyncActiveChanged);
    QSignalSpy downSpy(&notifier, &AndroidNotifier::backgroundSyncUnavailable);

    QVERIFY(!notifier.backgroundSyncActive());

    notifier.onSyncServiceStopped(QStringLiteral("dataSync-budget-exhausted"));
    QCOMPARE(downSpy.count(), 1);
    QCOMPARE(downSpy.at(0).at(0).toString(),
             QStringLiteral("dataSync-budget-exhausted"));
    QVERIFY(!notifier.backgroundSyncActive());

    // A second stop for the same reason is not a second state change.
    notifier.onSyncServiceStopped(QStringLiteral("dataSync-budget-exhausted"));
    QCOMPARE(activeSpy.count(), 0);
    QCOMPARE(downSpy.count(), 2);

    // Source guard for the policy itself: after a budget stop we wait for
    // the app to be foregrounded rather than hammering the platform.
    const QString n = readAll(srcDir() + "/core/AndroidNotifier.cpp");
    QVERIFY(n.contains(QStringLiteral("m_budgetExhausted = true")));
    QVERIFY(n.contains(QStringLiteral("if (!m_syncWanted || m_syncRunning || m_budgetExhausted) return;")));
}

// ------------------------------------------------------------- minSdk 28

void TestAndroidRuntime::micAndCameraFgsTypesAreGatedOnApi30NotApi29()
{
    const QString voice = javaCode(QStringLiteral("VoiceService.java"));

    // FOREGROUND_SERVICE_TYPE_MICROPHONE (128) and _CAMERA (64) are API 30
    // constants; only DATA_SYNC / MEDIA_PROJECTION date from API 29. With
    // minSdk 28 there are real 28/29 devices in range, and passing an
    // unknown type bit to startForeground() there is out of contract.
    const QString body =
        methodBody(voice, QStringLiteral("private static int foregroundTypeFor("));
    QVERIFY2(body.contains(QStringLiteral("Build.VERSION_CODES.R")),
             "the microphone/camera FGS types are API 30, so the gate must "
             "be R, not Q");
    QVERIFY(!body.contains(QStringLiteral("Build.VERSION_CODES.Q")));

    // dataSync and mediaProjection ARE API 29, so those two keep the Q gate.
    QVERIFY(javaCode(QStringLiteral("SyncService.java"))
                .contains(QStringLiteral("Build.VERSION_CODES.Q")));
    QVERIFY(javaCode(QStringLiteral("MediaProjectionService.java"))
                .contains(QStringLiteral("Build.VERSION_CODES.Q")));

    // minSdk itself, so the reasoning above stays true.
    const QString cmake = readAll(srcDir() + "/../CMakeLists.txt");
    QVERIFY(cmake.contains(QStringLiteral("BSFCHAT_ANDROID_MIN_SDK \"28\"")));
}

void TestAndroidRuntime::cameraFgsTypeIsGatedOnTheCameraPermission()
{
    const QString voice = javaCode(QStringLiteral("VoiceService.java"));
    const QString body =
        methodBody(voice, QStringLiteral("private static int foregroundTypeFor("));

    // Android 14 throws SecurityException from startForeground() for a type
    // whose runtime permission is not held. The manifest declares
    // microphone|camera, but a user joining voice has granted RECORD_AUDIO
    // and nothing else — so a fixed MICROPHONE|CAMERA threw on the most
    // ordinary path there is.
    QVERIFY2(body.contains(QStringLiteral("android.Manifest.permission.CAMERA"))
                 && body.contains(
                     QStringLiteral("FOREGROUND_SERVICE_TYPE_CAMERA")),
             "the camera FGS type must be conditional on the CAMERA "
             "permission actually being granted");
    QVERIFY(body.contains(QStringLiteral("android.Manifest.permission.RECORD_AUDIO")));
    QVERIFY(voice.contains(QStringLiteral("checkSelfPermission")));

    // Turning the camera on mid-call has to re-run that, or the call keeps
    // a microphone-only anchor and loses video on backgrounding.
    QVERIFY(voice.contains(QStringLiteral("public static void refreshForegroundType(")));
    QVERIFY(code(readAll(srcDir() + "/voice/CameraController.cpp"))
                .contains(QStringLiteral("refreshVoiceService()")));
}

// ----------------------------------------- no sensor prompts at app launch

void TestAndroidRuntime::noMediaBackendIsBroughtUpInAConstructor()
{
    // Observed on a device: camera and microphone permission prompts fired
    // on the SIGN-IN screen, before the user had touched a media feature.
    // Bringing Qt's multimedia stack up is enough to provoke them on
    // Android, and two constructors were doing it at startup. Play treats
    // an unprompted sensitive-permission request as a policy problem.
    const QString settings = code(readAll(srcDir() + "/core/Settings.cpp"));
    QCOMPARE(settings.count(QStringLiteral("new QMediaDevices")), qsizetype(1));
    QVERIFY2(methodBody(settings, QStringLiteral("void Settings::ensureMediaDevices("))
                 .contains(QStringLiteral("new QMediaDevices")),
             "QMediaDevices must only be constructed from "
             "ensureMediaDevices(), never from the Settings constructor on "
             "mobile");

    const QString cam = code(readAll(srcDir() + "/voice/CameraController.cpp"));
    QCOMPARE(cam.count(QStringLiteral("new QCamera")), qsizetype(1));
    const QString ensure =
        methodBody(cam, QStringLiteral("void CameraController::ensureCaptureSession("));
    QVERIFY2(ensure.contains(QStringLiteral("new QCamera"))
                 && ensure.contains(QStringLiteral("new QMediaCaptureSession")),
             "QCamera / QMediaCaptureSession must only be built in "
             "ensureCaptureSession(), which mobile reaches at the first "
             "camera start and not before");

    // The constructor may still prime them on desktop, where there is no
    // prompt to provoke — but only there.
    const QString ctor =
        methodBody(cam, QStringLiteral("CameraController::CameraController("));
    if (ctor.contains(QStringLiteral("ensureCaptureSession()"))) {
        QVERIFY2(ctor.contains(QStringLiteral("!defined(Q_OS_ANDROID)"))
                     && ctor.contains(QStringLiteral("!defined(Q_OS_IOS)")),
                 "an eager ensureCaptureSession() in the constructor must be "
                 "excluded on Android and iOS");
    }
}

QTEST_GUILESS_MAIN(TestAndroidRuntime)
#include "test_android_runtime.moc"
